// Харнесс проверки расчёта спектра.
//
// Гоняет Worker по всем вычислительным путям (CPU/GPU x Gray/XOR x short/long
// + дуальный код) и сверяет результат с эталоном. Требование к спектру
// абсолютное: расхождение даже на единицу — провал.
//
// Спектр снимается штатным сигналом spectrumUpdated: числа в нём точные, а
// не float, как у графика.

#include <QCoreApplication>
#include <set>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QTextStream>

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <thread>

// dualcode.h тянет gmpxx.h, где есть std::numeric_limits<...>::min(). Его нужно
// разобрать до windows.h, иначе макросы min/max из windows.h ломают тело класса.
#include "worker.h"
#include "cudabuffers.h"
#include "dualcode.h"

#include <omp.h>
#include "autosavestore.h"
#include "reference.h"
#include "ui/axisticks.h"
#include "ui/updateintervals.h"
#include "bz.h"
#include "isd.h"
#include "leonkernel.cuh"
#include "leon.h"
#include "mixing.h"
#include "product.h"
#include "bch.h"
#include "hamming.h"
#include "parity.h"

#ifdef Q_OS_WIN
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

using Reference::Spectrum;
using Algorithm     = ComputationSettings::Algorithm;
using ComputeDevice = ComputationSettings::ComputeDevice;
using EnumerationType = ComputationSettings::EnumerationType;

static QTextStream g_out(stdout);
static int g_passed = 0;
static int g_failed = 0;
static bool g_gpuAvailable = false;
// План последнего расчёта Брауэра–Циммермана: множеств, строк, точно до веса.
// -1 — расчёт был не по Брауэру–Циммерману.
static int g_planSets = -1, g_planRows = -1, g_planExactUpTo = -1;
// Последняя оценка случайного поиска: попыток сделано/всего, вероятность
// пропуска, оценка ненайденного по весам.
static quint64 g_searchDone = 0, g_searchTotal = 0;
static double  g_searchMiss = -1.0;
static SpectrumFloat g_searchUnseen;
// Последняя строка плана кода произведения и его точность.
static QString g_productText;
static int     g_productExactUpTo = -1;

// ---------------------------------------------------------------- утилиты

// Ненулевые веса спектра. Числа длиннее 64 бит (Мак-Вильямс у длинного
// кода) в сверках не встречаются; попади такое — в counts насыщение, и
// сверка с эталоном его не пропустит.
static Spectrum toSpectrum(const SpectrumCounts& counts)
{
    Spectrum s;
    for (int w = 0; w < counts.size(); ++w)
        if (counts.counts.at(w) != 0)
            s[w] = counts.counts.at(w);
    return s;
}

static QString formatSpectrum(const Spectrum& s)
{
    QStringList parts;
    for (auto it = s.constBegin(); it != s.constEnd(); ++it)
        parts << QStringLiteral("%1:%2").arg(it.key()).arg(it.value());
    return QLatin1Char('{') + parts.join(QStringLiteral(", ")) + QLatin1Char('}');
}

// Убирает нулевые записи, чтобы сравнение не зависело от того, печатает ли
// реализация нули.
static Spectrum stripZeros(const Spectrum& s)
{
    Spectrum r;
    for (auto it = s.constBegin(); it != s.constEnd(); ++it)
        if (it.value() != 0) r[it.key()] = it.value();
    return r;
}

// ------------------------------------------------------------ запуск Worker

struct RunConfig
{
    QStringList matrix;
    Algorithm   algorithm  = Algorithm::SimpleXor;
    ComputeDevice device   = ComputeDevice::Cpu;
    int         maxRows    = 0;      // 0 => все строки
    int         threadsCpu = 4;
    int         blocksGpu  = 64;
    int         threadsGpu = 256;
    // Подбирать сетку замером вместо blocksGpu/threadsGpu.
    bool        autoTune   = false;
    // Брауэр–Циммерман: до какого веса нужен точный спектр.
    int         bzWeight   = 0;
    // Случайный поиск: до какого веса и с какой степенью пропуска.
    int         leonWeight = 0;
    int         leonMissExponent = 12;
    // Код произведения: вторая компонента, вес (0 — до границы ранга), ранг.
    QStringList matrix2;
    int         productWeight = 0;
    int         productRank   = 2;
    // Чем считать большие компоненты; по умолчанию — случайным поиском.
    Algorithm   productAlgorithm = Algorithm::RandomInfoSets;
    // До какой размерности компоненту произведения перебирать целиком.
    int         productBruteForceMaxK = Product::BRUTE_FORCE_MAX_K;
    // Где стохастическому поиску можно брать окно Штерна–Дюмера.
    Leon::WindowPolicy window;
    // Поиск по орбитам сдвигов у циклического кода.
    bool        cyclicSearch = true;
};

static ComputationSettings makeSettings(const RunConfig& cfg)
{
    ComputationSettings s;
    s.matrix        = cfg.matrix;
    s.algorithm = cfg.algorithm;
    s.enumType      = ComputationSettings::Full;
    s.maxRows       = cfg.maxRows > 0 ? cfg.maxRows : cfg.matrix.size();
    s.bzWeight      = cfg.bzWeight > 0 ? cfg.bzWeight : 8;
    s.leonWeight    = cfg.leonWeight > 0 ? cfg.leonWeight : 24;
    s.leonMissExponent = cfg.leonMissExponent;
    s.matrix2       = cfg.matrix2;
    s.productWeight = cfg.productWeight;
    s.productRank   = cfg.productRank;
    s.productAlgorithm = int(cfg.productAlgorithm);
    s.device       = cfg.device;
    s.deviceSettings.threadsCpu = cfg.threadsCpu;
    s.deviceSettings.blocksGpu  = cfg.blocksGpu;
    s.deviceSettings.threadsGpu = cfg.threadsGpu;
    s.autoTuneGrid          = cfg.autoTune;
    // Интервалы задраны так, чтобы за время теста чекпоинт не сработал:
    // сохранение состояния проверяется отдельными тестами.
    s.intervals.saveSpectrumInterval   = 100000;
    s.intervals.updateSpectrumInterval = 100000;
    return s;
}

// Автосохранения на время прогона складываются в свой каталог: боевой лежит
// в AppData пользователя, и топтать его тестами нельзя.
static QString autosaveRoot()
{
    return QDir::tempPath() + QStringLiteral("/SpectrumTests-autosave");
}

static AutosaveStore testStore()
{
    return AutosaveStore(autosaveRoot());
}

// Прогоняет расчёт синхронно и возвращает итоговый спектр.
//
// loadMode задаёт, начинать с нуля или продолжить с сохранённого состояния.
// checkpointEveryOps/stopAfterOps включают воспроизводимое прерывание.
static Spectrum runWorker(const RunConfig& cfg,
                          LoadMode loadMode           = LoadMode::Reset,
                          quint64  checkpointEveryOps = 0,
                          quint64  stopAfterOps       = 0)
{
    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());
    Spectrum captured;
    bool errored = false;
    QString errorMessage;

    QObject::connect(&worker, &Worker::spectrumUpdated,
                     [&captured](const SpectrumCounts& s) { captured = toSpectrum(s); });
    QObject::connect(&worker, &Worker::errorOccurred,
                     [&](const QString& m) { errored = true; errorMessage = m; });
    g_planSets = g_planRows = g_planExactUpTo = -1;
    QObject::connect(&worker, &Worker::planReady,
                     [](int sets, int rows, int exactUpTo) {
                         g_planSets = sets; g_planRows = rows; g_planExactUpTo = exactUpTo;
                     });
    g_searchDone = g_searchTotal = 0; g_searchMiss = -1.0; g_searchUnseen.clear();
    g_productText.clear(); g_productExactUpTo = -1;
    QObject::connect(&worker, &Worker::productPlanReady,
                     [](const QString& text, int exactUpTo) {
                         g_productText = text;
                         if (exactUpTo >= 0) g_productExactUpTo = exactUpTo;
                     });
    QObject::connect(&worker, &Worker::searchEstimateUpdated,
                     [](int, quint64 done, quint64 total, double miss, SpectrumFloat unseen) {
                         g_searchDone = done; g_searchTotal = total;
                         g_searchMiss = miss; g_searchUnseen = unseen;
                     });

    worker.setSettings(makeSettings(cfg));
    worker.setWindowPolicy(cfg.window);
    worker.setProductBruteForceMaxK(cfg.productBruteForceMaxK);
    worker.setCyclicSearch(cfg.cyclicSearch);
    worker.setCheckpointOpsPolicy(checkpointEveryOps, stopAfterOps);
    // Тестовые матрицы мелкие, и в боевом режиме подбор на них не запустился
    // бы вовсе — тесты про подбор стали бы пустыми.
    worker.setGridTuningThreshold(0.0);
    worker.initializeRunState(loadMode);
    worker.computeSpectrum();

    if (errored) {
        g_out << QStringLiteral("      ошибка от Worker: ") << errorMessage << Qt::endl;
        return Spectrum();
    }
    return stripZeros(captured);
}

// Стирает все сохранённые записи, чтобы прогон не зависел от предыдущего.
static void clearCheckpoints()
{
    testStore().removeAll();
}

// Полное число операций, которое должен выполнить расчёт при данных настройках.
// Нужно, чтобы отличить настоящий обрыв на середине от «досчитали до конца и
// только потом сохранились».
static quint64 expectedTotalOps(const RunConfig& cfg)
{
    const quint64 k = quint64(cfg.matrix.size());
    if (cfg.algorithm == Algorithm::GrayCode || cfg.algorithm == Algorithm::DualCode)
        return 1ULL << k;                    // код Грея перебирает все маски подряд

    quint64 maxRows = cfg.maxRows > 0 ? quint64(cfg.maxRows) : k;
    quint64 sets    = 1;
    if (cfg.algorithm == Algorithm::BrouwerZimmermann) {
        // Тот же план, что строит воркер: множества по матрице, их число и
        // глубина — по весу.
        int words = 0;
        const std::vector<quint64> packed = InfoSets::packRows(cfg.matrix, words);
        const int rows = cfg.matrix.size();
        const int cols = cfg.matrix.first().length();
        const int fit  = std::max(1, Constants::MAX_CONST_WORDS / std::max(1, rows * words));
        std::vector<InfoSets::InfoSet> found =
            InfoSets::find(packed.data(), rows, cols, words, std::min(Constants::MAX_INFO_SETS, fit));
        std::vector<int> overlaps;
        for (const InfoSets::InfoSet& set : found) overlaps.push_back(set.overlap);
        const int weight = cfg.bzWeight > 0 ? cfg.bzWeight : 8;
        sets = quint64(InfoSets::setsForWeight(overlaps, weight, rows, cols));
        overlaps.resize(size_t(sets));
        const InfoSets::Depth depth = InfoSets::depthForWeight(overlaps, weight, rows, cols);
        // Слои до последнего — по всем множествам, последний — по первым
        // lastLayerSets.
        quint64 below = 0;
        for (quint64 r = 0; r + 1 <= quint64(depth.maxRows); ++r)
            below += Reference::binom(k, r);
        return below * sets + Reference::binom(k, quint64(depth.maxRows)) * quint64(depth.lastLayerSets);
    }
    quint64 total = 0;
    for (quint64 r = 0; r <= maxRows; ++r)
        total += Reference::binom(k, r);
    return total * sets;
}

// Сколько операций записано в единственном сохранённом автосохранении.
// -1 — записи нет вовсе.
static qint64 savedDoneOps()
{
    const QVector<AutosaveEntry> entries = testStore().list();
    if (entries.isEmpty())
        return -1;
    return qint64(entries.first().record.state.doneOps);
}

// ------------------------------------------------------------------ проверка

static void check(const QString& name, const RunConfig& cfg, const Spectrum& expected)
{
    if (cfg.device == ComputeDevice::Gpu && !g_gpuAvailable) {
        g_out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
        return;
    }

    const Spectrum actual = runWorker(cfg);
    const Spectrum want   = stripZeros(expected);

    if (actual == want) {
        ++g_passed;
        g_out << "  ok       " << name << Qt::endl;
        return;
    }

    ++g_failed;
    g_out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
    g_out << QStringLiteral("      ожидалось: ") << formatSpectrum(want)   << Qt::endl;
    g_out << QStringLiteral("      получено:  ") << formatSpectrum(actual) << Qt::endl;

    // Показываем расхождения поимённо — «отклонение даже на единицу» должно
    // быть видно сразу.
    QList<int> weights = want.keys();
    for (int w : actual.keys())
        if (!weights.contains(w)) weights << w;
    std::sort(weights.begin(), weights.end());
    for (int w : weights) {
        const quint64 e = want.value(w, 0), a = actual.value(w, 0);
        if (e != a)
            g_out << QStringLiteral("      вес ") << w << QStringLiteral(": ожидалось ") << e << QStringLiteral(", получено ") << a
                  << QStringLiteral(" (разница ") << (qint64(a) - qint64(e)) << ")" << Qt::endl;
    }
}

// ------------------------------------------------------------------ сценарии

// Все пути, доступные для короткого кода, должны дать один и тот же спектр.
static void testShortCode(const QString& label, const QStringList& matrix,
                          const Spectrum& analytic)
{
    g_out << Qt::endl << label << " (k=" << matrix.size()
          << ", n=" << matrix.first().length() << ")" << Qt::endl;

    const Spectrum brute = Reference::bruteForce(matrix);

    // Сначала убеждаемся, что наивный перебор согласуется с аналитикой —
    // иначе эталону нельзя доверять.
    if (stripZeros(brute) != stripZeros(analytic)) {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   эталон: перебор разошёлся с аналитическим спектром")
              << Qt::endl
              << QStringLiteral("      аналитика: ") << formatSpectrum(stripZeros(analytic)) << Qt::endl
              << QStringLiteral("      перебор:   ") << formatSpectrum(stripZeros(brute))    << Qt::endl;
        return;
    }
    g_out << QStringLiteral("  ok       эталон: перебор == аналитический спектр") << Qt::endl;
    ++g_passed;

    RunConfig cfg; cfg.matrix = matrix;

    cfg.algorithm = Algorithm::SimpleXor; cfg.device = ComputeDevice::Cpu;
    check("CPU  XOR  (короткий)", cfg, brute);
    cfg.device = ComputeDevice::Gpu;
    check("GPU  XOR  (короткий)", cfg, brute);

    cfg.algorithm = Algorithm::GrayCode;  cfg.device = ComputeDevice::Cpu;
    check("CPU  Грей (короткий)", cfg, brute);
    cfg.device = ComputeDevice::Gpu;
    check("GPU  Грей (короткий)", cfg, brute);
}

// Сверка всех четырёх коротких путей с наивным перебором. Отдельно от
// testShortCode, где ещё требуется аналитический спектр: у произвольной матрицы
// его нет, но перебор остаётся абсолютным эталоном.
static void testShortCodeAgainstBruteForce(const QString& label, const QStringList& matrix)
{
    g_out << Qt::endl << label << QStringLiteral(" (k=") << matrix.size()
          << QStringLiteral(", n=") << matrix.first().length() << QStringLiteral(")") << Qt::endl;

    const Spectrum brute = Reference::bruteForce(matrix);

    RunConfig cfg; cfg.matrix = matrix;
    cfg.algorithm = Algorithm::SimpleXor; cfg.device = ComputeDevice::Cpu;
    check("CPU  XOR ", cfg, brute);
    cfg.device = ComputeDevice::Gpu;
    check("GPU  XOR ", cfg, brute);
    cfg.algorithm = Algorithm::GrayCode;  cfg.device = ComputeDevice::Cpu;
    check("CPU  Грей", cfg, brute);
    cfg.device = ComputeDevice::Gpu;
    check("GPU  Грей", cfg, brute);
}

// Длинный код: полный перебор невозможен, эталон — C(n,w) на единичной матрице.
static void testLongCode(int n, int maxRows)
{
    g_out << Qt::endl << QStringLiteral("Единичная матрица I(") << n << "), maxRows=" << maxRows
          << QStringLiteral(" — длинный путь") << Qt::endl;

    const Spectrum expected = Reference::identityPartialSpectrum(n, maxRows);

    RunConfig cfg;
    cfg.matrix    = Reference::identity(n);
    cfg.algorithm = Algorithm::SimpleXor;
    cfg.maxRows   = maxRows;

    cfg.device = ComputeDevice::Cpu;
    check("CPU  XOR  (длинный)", cfg, expected);
    cfg.device = ComputeDevice::Gpu;
    check("GPU  XOR  (длинный)", cfg, expected);
}

// Частичный перебор на коротком коде — сверяем с ограниченным перебором.
static void testPartialShort(const QStringList& matrix, int maxRows)
{
    g_out << Qt::endl << QStringLiteral("Частичный перебор, maxRows=") << maxRows << Qt::endl;

    // Ограниченный перебор строим здесь же, отдельно от production-кода.
    const int k = matrix.size();
    Spectrum expected;
    for (quint64 mask = 0; mask < (1ULL << k); ++mask) {
        int ones = 0;
        for (int i = 0; i < k; ++i) if (mask & (1ULL << i)) ++ones;
        if (ones > maxRows) continue;

        const int n = matrix.first().length();
        QVector<bool> cw(n, false);
        for (int i = 0; i < k; ++i)
            if (mask & (1ULL << i))
                for (int c = 0; c < n; ++c)
                    if (matrix[i].at(c) == QLatin1Char('1')) cw[c] = !cw[c];
        int weight = 0;
        for (int c = 0; c < n; ++c) if (cw[c]) ++weight;
        expected[weight] += 1;
    }

    RunConfig cfg;
    cfg.matrix    = matrix;
    cfg.algorithm = Algorithm::SimpleXor;
    cfg.maxRows   = maxRows;

    cfg.device = ComputeDevice::Cpu;
    check("CPU  XOR  частичный", cfg, expected);
    cfg.device = ComputeDevice::Gpu;
    check("GPU  XOR  частичный", cfg, expected);
}

// Дуальный код: считается спектр проверочной матрицы, затем восстанавливается
// исходный через тождества Мак-Вильямс. Результат обязан совпасть с перебором.
// Простая проверка условия: ok или ПРОВАЛ с именем.
static void expectStore(const QString& name, bool condition)
{
    if (condition) { ++g_passed; g_out << "  ok       " << name << Qt::endl; }
    else           { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl; }
}

static void testDualCode(const QString& label, const QStringList& matrix)
{
    g_out << Qt::endl << label << QStringLiteral(" — через дуальный код") << Qt::endl;

    const Spectrum brute = Reference::bruteForce(matrix);

    RunConfig cfg;
    cfg.matrix    = matrix;
    cfg.algorithm = Algorithm::DualCode;

    cfg.device = ComputeDevice::Cpu;
    check("CPU  дуальный", cfg, brute);
    cfg.device = ComputeDevice::Gpu;
    check("GPU  дуальный", cfg, brute);
}

// Преобразование Мак-Вильямс само по себе: спектр кода по спектру дуального
// против полного перебора самого кода, а также числа длиннее 64 бит.
static void testMacWilliams()
{
    g_out << Qt::endl << QStringLiteral("Мак-Вильямс: спектр кода по спектру дуального") << Qt::endl;

    struct Case { QString name; QStringList generator; };
    const QVector<Case> cases = {
        { QStringLiteral("Хэмминг (7,4)"), Reference::hamming7_4() },
        { QStringLiteral("Голей (24,12)"), Reference::golay24_12() },
        { QStringLiteral("rnd(10,30)"),    Reference::randomMatrix(10, 30, 21) },
        { QStringLiteral("rnd(14,26)"),    Reference::randomMatrix(14, 26, 22) },
        { QStringLiteral("rnd(16,33)"),    Reference::randomMatrix(16, 33, 23) },
    };
    for (const Case& c : cases) {
        const int n = c.generator.first().length();
        const QStringList parity = generatorToParity(c.generator);
        const Spectrum code = Reference::bruteForce(c.generator);
        const Spectrum dual = Reference::bruteForce(parity);
        std::vector<quint64> dualCounts(size_t(n) + 1, 0);
        for (auto it = dual.constBegin(); it != dual.constEnd(); ++it)
            dualCounts[size_t(it.key())] = it.value();

        const std::vector<mpz_class> a = macWilliams(dualCounts.data(), n, parity.size());
        bool same = int(a.size()) == n + 1 && parity.size() == n - c.generator.size();
        for (int w = 0; same && w <= n; ++w)
            same = a[size_t(w)] == mpz_class(QString::number(code.value(w, 0)).toStdString());
        expectStore(c.name + QStringLiteral(": совпадает с полным перебором"), same);
    }

    // Код с проверкой на чётность [100, 99]: дуальный — повторение {0, 1…1},
    // и спектр кода — C(100, w) на чётных весах. C(100, 50) ≈ 10^29 в 64 бита
    // не помещается: числа обязаны выйти точными, а в 64-битном виде —
    // насыщенными, не обрезанными.
    const int n = 100;
    std::vector<quint64> repetition(size_t(n) + 1, 0);
    repetition[0] = repetition[size_t(n)] = 1;
    const std::vector<mpz_class> even = macWilliams(repetition.data(), n, 1);
    const QVector<quint64> saturated = saturatedCounts(even);
    bool exact = int(even.size()) == n + 1, clamped = exact;
    for (int w = 0; exact && w <= n; ++w) {
        mpz_class binom;
        mpz_bin_uiui(binom.get_mpz_t(), static_cast<unsigned long>(n), static_cast<unsigned long>(w));
        const mpz_class expected = (w % 2 == 0) ? binom : mpz_class(0);
        exact = even[size_t(w)] == expected;
        const quint64 expected64 = mpz_sizeinbase(expected.get_mpz_t(), 2) > 64
                                     ? std::numeric_limits<quint64>::max()
                                     : quint64(QString::fromStdString(expected.get_str()).toULongLong());
        clamped = clamped && saturated[w] == expected64;
    }
    expectStore(QStringLiteral("[100,99]: числа длиннее 64 бит точны"), exact);
    expectStore(QStringLiteral("[100,99]: в 64 битах — насыщение"), clamped);
}

// Спектр строками «вес - число» и обратно: так он хранится в настройках
// между запусками. Числа длиннее 64 бит обязаны пройти туда и обратно
// точными.
static void testSpectrumCounts()
{
    g_out << Qt::endl << QStringLiteral("Спектр: строки «вес - число» и обратно") << Qt::endl;

    // [100,99]: C(100, 50) ≈ 10^29 в 64 бита не помещается.
    const int n = 100;
    std::vector<quint64> repetition(size_t(n) + 1, 0);
    repetition[0] = repetition[size_t(n)] = 1;
    const std::vector<mpz_class> exact = macWilliams(repetition.data(), n, 1);
    const SpectrumCounts big = spectrumCounts(exact);

    bool same = big.size() == n + 1;
    for (int w = 0; same && w <= n; ++w)
        same = big.decimal(w) == QString::fromStdString(exact[size_t(w)].get_str());
    expectStore(QStringLiteral("числа длиннее 64 бит видны точными"), same);

    const SpectrumCounts back = SpectrumCounts::fromLines(big.lines());
    bool roundTrip = back.size() == n + 1;
    for (int w = 0; roundTrip && w <= n; ++w)
        roundTrip = back.decimal(w) == big.decimal(w) && back.counts.at(w) == big.counts.at(w);
    expectStore(QStringLiteral("строки -> спектр -> строки без потерь"), roundTrip);

    // Строки прежних версий и мусор: нечитаемое пропускается.
    const SpectrumCounts old = SpectrumCounts::fromLines(QStringList{
        QStringLiteral("0 - 1"), QStringLiteral("8 - 759"), QStringLiteral("мусор"),
        QStringLiteral("12 - 2576"), QStringLiteral("13 - abc"), QStringLiteral("x - 5") });
    expectStore(QStringLiteral("строки прежних версий читаются, мусор пропускается"),
                old.size() == 13 && old.counts.at(0) == 1 && old.counts.at(8) == 759
                && old.counts.at(12) == 2576 && old.exact.isEmpty()
                && old.lines() == QStringList({ QStringLiteral("0 - 1"), QStringLiteral("8 - 759"),
                                                QStringLiteral("12 - 2576") }));
}

// ---------------------------------------------------- чекпоинты

// Общий сценарий: посчитать целиком, затем посчитать с прерыванием и
// возобновлением — результаты обязаны совпасть точно.
//
// resumeCfg отличается от cfg, когда проверяется перенос состояния между
// разными конфигурациями железа.
static void checkResume(const QString& name, const RunConfig& cfg,
                        const RunConfig& resumeCfg,
                        quint64 checkpointEveryOps, quint64 stopAfterOps)
{
    const bool needsGpu = cfg.device == ComputeDevice::Gpu
                       || resumeCfg.device == ComputeDevice::Gpu;
    if (needsGpu && !g_gpuAvailable) {
        g_out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
        return;
    }

    clearCheckpoints();
    const Spectrum whole = runWorker(cfg);

    clearCheckpoints();
    // Первый проход: считаем до порога и останавливаемся на чекпоинте.
    runWorker(cfg, LoadMode::Reset, checkpointEveryOps, stopAfterOps);

    // Без этой проверки тест был бы бесполезен: если прерывание не сработало,
    // второй проход просто посчитал бы всё заново и сравнение прошло бы само
    // собой, ничего не проверив. Обрыв обязан быть строго внутри диапазона —
    // сохранение уже на последнем чанке ничего не доказывает.
    const qint64  saved = savedDoneOps();
    const quint64 total = expectedTotalOps(cfg);
    if (saved <= 0 || quint64(saved) >= total) {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   ") << name
              << QStringLiteral("  — обрыва не было: сохранено ") << saved
              << QStringLiteral(" из ") << total
              << QStringLiteral(" операций, возобновление не проверено") << Qt::endl;
        clearCheckpoints();
        return;
    }

    // Второй проход: продолжаем с сохранённого состояния до конца.
    const Spectrum resumed = runWorker(resumeCfg, LoadMode::FromCheckpoint);

    clearCheckpoints();

    if (resumed == whole) {
        ++g_passed;
        g_out << "  ok       " << name
              << QStringLiteral("  (обрыв на ") << saved << QStringLiteral(" оп.)") << Qt::endl;
        return;
    }

    ++g_failed;
    g_out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
    g_out << QStringLiteral("      целиком:      ") << formatSpectrum(whole)   << Qt::endl;
    g_out << QStringLiteral("      с прерыванием:") << formatSpectrum(resumed) << Qt::endl;

    QList<int> weights = whole.keys();
    for (int w : resumed.keys())
        if (!weights.contains(w)) weights << w;
    std::sort(weights.begin(), weights.end());
    for (int w : weights) {
        const quint64 e = whole.value(w, 0), a = resumed.value(w, 0);
        if (e != a)
            g_out << QStringLiteral("      вес ") << w << QStringLiteral(": целиком ") << e
                  << QStringLiteral(", с прерыванием ") << a
                  << QStringLiteral(" (разница ") << (qint64(a) - qint64(e)) << ")" << Qt::endl;
    }
}

// Настройки длинного кода, при которых расчёт заведомо режется на несколько
// чанков и его есть где прервать.
static RunConfig longConfig(ComputeDevice dev)
{
    RunConfig cfg;
    cfg.matrix     = Reference::identity(70);
    cfg.algorithm  = Algorithm::SimpleXor;
    cfg.maxRows    = 5;
    cfg.device     = dev;
    cfg.threadsCpu = 4;
    // Чанк GPU = блоки * нити * 4096 масок. При штатных 64x256 это 67 млн —
    // больше всей задачи, и обрыва не случилось бы.
    cfg.blocksGpu  = 8;
    cfg.threadsGpu = 32;
    return cfg;
}

static void testCheckpoints()
{
    g_out << Qt::endl << QStringLiteral("Чекпоинты: прерывание и возобновление") << Qt::endl;

    // Голей (24,12): 4096 комбинаций, прерывания в разных точках.
    RunConfig golay;
    golay.matrix = Reference::golay24_12();

    golay.algorithm = Algorithm::SimpleXor;
    for (ComputeDevice dev : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
        golay.device = dev;
        const QString who = QStringLiteral("%1 XOR Голей")
                                .arg(dev == ComputeDevice::Cpu ? QStringLiteral("CPU")
                                                               : QStringLiteral("GPU"));
        // Прерывание около 10 %, 50 % и 90 % пройденного.
        checkResume(who + QStringLiteral(", обрыв ~10%"), golay, golay, 400,  400);
        checkResume(who + QStringLiteral(", обрыв ~50%"), golay, golay, 2000, 2000);
        checkResume(who + QStringLiteral(", обрыв ~90%"), golay, golay, 3600, 3600);
        // Несколько сохранений подряд за один проход.
        checkResume(who + QStringLiteral(", много чекпоинтов"), golay, golay, 300, 2100);
    }

    // Коду Грея нужна матрица покрупнее: он идёт чанками по 2^20 масок, и на
    // Голее (4096 масок) весь расчёт укладывается в один чанк — прерывать
    // нечего. I(22) даёт 4.2 млн масок, то есть четыре чанка.
    RunConfig gray;
    gray.matrix    = Reference::identity(22);
    gray.algorithm = Algorithm::GrayCode;
    for (ComputeDevice dev : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
        gray.device = dev;
        const QString who = QStringLiteral("%1 Грей I(22)")
                                .arg(dev == ComputeDevice::Cpu ? QStringLiteral("CPU")
                                                               : QStringLiteral("GPU"));
        checkResume(who + QStringLiteral(", обрыв ~25%"), gray, gray, 1000000, 1000000);
        checkResume(who + QStringLiteral(", обрыв ~50%"), gray, gray, 2000000, 2000000);
        checkResume(who + QStringLiteral(", много чекпоинтов"), gray, gray, 1000000, 3000000);
    }

    // Смена слоя r — самое рискованное место для XOR: обрыв должен попасть
    // на границу между числом единиц в маске.
    RunConfig layer;
    layer.matrix    = Reference::golay24_12();
    layer.algorithm = Algorithm::SimpleXor;
    // C(12,0)+C(12,1)+C(12,2) = 1+12+66 = 79 — конец слоя r=2.
    for (quint64 boundary : { 13ULL, 79ULL, 299ULL }) {
        layer.device = ComputeDevice::Cpu;
        checkResume(QStringLiteral("CPU XOR, обрыв на границе слоя (%1)").arg(boundary),
                    layer, layer, boundary, boundary);
        layer.device = ComputeDevice::Gpu;
        checkResume(QStringLiteral("GPU XOR, обрыв на границе слоя (%1)").arg(boundary),
                    layer, layer, boundary, boundary);
    }

    // Длинный код. При maxRows=3 всего 57 тыс. масок — это один чанк, обрывать
    // нечего. maxRows=5 даёт 13 млн, и чанков становится несколько. GPU-чанк
    // равен блоки*нити*4096, поэтому разбиение здесь намеренно мелкое.
    RunConfig lng = longConfig(ComputeDevice::Cpu);
    checkResume(QStringLiteral("CPU XOR длинный, обрыв"), lng, lng, 3000000, 3000000);
    lng = longConfig(ComputeDevice::Gpu);
    checkResume(QStringLiteral("GPU XOR длинный, обрыв"), lng, lng, 3000000, 3000000);

    // Длинный путь на видеокарте проверяется отдельно и подробнее.
    //
    // Там подготовка стартовых масок идёт в двойной буфер, а синхронизации
    // после каждого чанка больше нет — хост убегает вперёд и успевает
    // поставить в очередь несколько ядер. Чекпоинт обязан отражать реально
    // посчитанное, а не поставленное в очередь: saveGpuCheckpoint для этого
    // сначала дожидается потока. Если бы не дожидался, сохранённый спектр
    // отставал бы от chunkOffset, и возобновление потеряло бы часть слов.
    const RunConfig lgpu = longConfig(ComputeDevice::Gpu);
    for (quint64 stop : { 1500000ULL, 4000000ULL, 7000000ULL, 11000000ULL }) {
        checkResume(QStringLiteral("GPU длинный, обрыв на %1").arg(stop),
                    lgpu, lgpu, stop, stop);
    }
    // Много сохранений за один проход: буфер стартовых масок перекладывается
    // многократно, и каждый чекпоинт попадает в середину этой череды.
    checkResume(QStringLiteral("GPU длинный, много чекпоинтов"),
                lgpu, lgpu, 700000, 9000000);
    // Мелкое разбиение — чанков сильно больше, значит больше и перекладываний.
    RunConfig lfine = lgpu;
    lfine.blocksGpu  = 4;
    lfine.threadsGpu = 32;
    checkResume(QStringLiteral("GPU длинный, мелкие чанки"), lfine, lfine, 900000, 5000000);
}

// Отмена посреди чанка. Нити бросают перебор, не дойдя до конца чанка, и
// такой чанк не имеет права попасть ни в спектр, ни в чекпоинт.
//
// На процессоре в коде Грея и в длинном пути раньше попадал: прерванный
// чанк прибавлялся к спектру, а чекпоинт после него записывал, что чанк
// пройден целиком. Сценарий из жизни — пауза дольше интервала
// автосохранения и потом «Отмена»: сохранение после паузы назревает
// обязательно. «Продолжить» по такой записи давало заниженный спектр без
// единого сообщения.
//
// Здесь пауза ставится до старта, а чекпоинт — после каждого чанка: нити
// встают на паузу в первом же чанке, отмена приходит, пока они стоят.
// Проверяется инвариант записи: слов в сохранённом спектре ровно столько,
// сколько масок пройдено по её смещению.
static void checkCancelMidChunk(const QString& name, const RunConfig& cfg)
{
    if (cfg.device == ComputeDevice::Gpu && !g_gpuAvailable) {
        g_out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
        return;
    }

    clearCheckpoints();

    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());
    worker.setSettings(makeSettings(cfg));
    worker.setCheckpointOpsPolicy(1, 0);
    worker.setGridTuningThreshold(0.0);
    worker.initializeRunState(LoadMode::Reset);
    worker.pause();

    std::thread canceller([&worker]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        worker.cancel();
    });
    worker.computeSpectrum();
    canceller.join();

    AutosaveRecord record;
    const bool saved = testStore().load(cfg.matrix, cfg.algorithm, record);
    clearCheckpoints();

    if (!saved) {
        ++g_passed;
        g_out << "  ok       " << name << QStringLiteral("  (чекпоинта нет)") << Qt::endl;
        return;
    }

    // Пройдено масок по смещению записи: у кода Грея — сплошной номер, у
    // слоёв — все слои до rOffset целиком и chunkOffset масок слоя rOffset.
    quint64 covered = record.state.chunkOffset;
    if (cfg.algorithm != Algorithm::GrayCode)
        for (quint64 r = 0; r < record.state.rOffset; ++r)
            covered += Reference::binom(quint64(cfg.matrix.size()), r);
    quint64 counted = 0;
    for (quint64 v : record.state.spectrum)
        counted += v;

    if (counted == covered) {
        ++g_passed;
        g_out << "  ok       " << name
              << QStringLiteral("  (чекпоинт на %1 масок сходится со спектром)").arg(covered) << Qt::endl;
        return;
    }
    ++g_failed;
    g_out << QStringLiteral("  ПРОВАЛ   ") << name
          << QStringLiteral("  — по чекпоинту пройдено %1 масок, а в его спектре %2 слов")
                 .arg(covered).arg(counted) << Qt::endl;
}

static void testCancelMidChunk()
{
    g_out << Qt::endl << QStringLiteral("Отмена посреди чанка: чекпоинт сходится со спектром") << Qt::endl;

    for (ComputeDevice dev : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
        const QString who = dev == ComputeDevice::Cpu ? QStringLiteral("CPU") : QStringLiteral("GPU");

        // Код Грея: I(22) — четыре чанка по 2^20 масок.
        RunConfig gray;
        gray.matrix    = Reference::identity(22);
        gray.algorithm = Algorithm::GrayCode;
        gray.device    = dev;
        checkCancelMidChunk(who + QStringLiteral(" Грей, пауза и отмена"), gray);

        // Простой XOR, короткий код.
        RunConfig xorShort;
        xorShort.matrix    = Reference::golay24_12();
        xorShort.algorithm = Algorithm::SimpleXor;
        xorShort.device    = dev;
        checkCancelMidChunk(who + QStringLiteral(" XOR Голей, пауза и отмена"), xorShort);

        // Длинный код (k >= 64).
        checkCancelMidChunk(who + QStringLiteral(" XOR длинный, пауза и отмена"), longConfig(dev));
    }
}

// Ключевая проверка: чекпоинт обязан переноситься между разными
// конфигурациями железа. Точка возобновления хранится как абсолютный индекс
// (ранг сочетания либо номер маски Грея), а не как номер чанка, поэтому смена
// числа потоков, блоков и даже устройства не должна ни на что влиять.
static void testCheckpointPortability()
{
    g_out << Qt::endl
          << QStringLiteral("Чекпоинты: перенос между конфигурациями") << Qt::endl;

    RunConfig base;
    base.matrix    = Reference::golay24_12();
    base.algorithm = Algorithm::SimpleXor;

    // Другое число потоков CPU.
    RunConfig other = base;
    other.threadsCpu = 1;
    RunConfig from = base;
    from.threadsCpu = 8;
    checkResume(QStringLiteral("CPU 8 потоков -> CPU 1 поток"), from, other, 1000, 1000);

    // Другое разбиение на GPU.
    from = base;              from.device = ComputeDevice::Gpu;
    from.blocksGpu = 64;      from.threadsGpu = 256;
    other = base;             other.device = ComputeDevice::Gpu;
    other.blocksGpu = 8;      other.threadsGpu = 64;
    checkResume(QStringLiteral("GPU 64x256 -> GPU 8x64"), from, other, 1000, 1000);

    // Смена устройства в обе стороны.
    from = base;  from.device  = ComputeDevice::Cpu;
    other = base; other.device = ComputeDevice::Gpu;
    checkResume(QStringLiteral("CPU -> GPU"), from, other, 1000, 1000);

    from = base;  from.device  = ComputeDevice::Gpu;
    other = base; other.device = ComputeDevice::Cpu;
    checkResume(QStringLiteral("GPU -> CPU"), from, other, 1000, 1000);

    // То же самое на коде Грея — на матрице, которая режется на чанки.
    from = RunConfig();  from.matrix = Reference::identity(22);
    from.algorithm = Algorithm::GrayCode; from.device = ComputeDevice::Cpu;
    other = from;        other.device = ComputeDevice::Gpu;
    checkResume(QStringLiteral("Грей: CPU -> GPU"), from, other, 2000000, 2000000);
    checkResume(QStringLiteral("Грей: GPU -> CPU"), other, from, 2000000, 2000000);

    // Длинный код: смена устройства в обе стороны.
    const RunConfig lngCpu = longConfig(ComputeDevice::Cpu);
    const RunConfig lngGpu = longConfig(ComputeDevice::Gpu);
    checkResume(QStringLiteral("длинный: CPU -> GPU"), lngCpu, lngGpu, 3000000, 3000000);
    checkResume(QStringLiteral("длинный: GPU -> CPU"), lngGpu, lngCpu, 3000000, 3000000);
}

// ------------------------------------------------------ автоподбор сетки

// Сетка, которую выбрал подбор. {0,0} — подбор не сработал или не применим.
static QPair<int, int> tunedGridFor(const RunConfig& cfg, bool verbose = false)
{
    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());
    QPair<int, int> grid(0, 0);

    QObject::connect(&worker, &Worker::gridTuned,
                     [&grid](int blocks, int threads) { grid = qMakePair(blocks, threads); });

    RunConfig tuned = cfg;
    tuned.autoTune = true;
    worker.setSettings(makeSettings(tuned));
    worker.setGridTuningThreshold(0.0);
    worker.setGridTuningVerbose(verbose);
    worker.initializeRunState(LoadMode::Reset);
    worker.computeSpectrum();
    return grid;
}

// Главное свойство подбора: он не имеет права изменить результат. Спектр,
// посчитанный на подобранной сетке, обязан совпасть с посчитанным на
// настройках пользователя — до последней единицы.
static void checkTuned(const QString& name, const RunConfig& cfg)
{
    if (!g_gpuAvailable) {
        g_out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
        return;
    }

    RunConfig plain = cfg;  plain.autoTune = false;
    RunConfig tuned = cfg;  tuned.autoTune = true;

    const Spectrum expected = runWorker(plain);
    const Spectrum actual   = runWorker(tuned);
    const QPair<int, int> grid = tunedGridFor(cfg);

    const QString gridText = grid.first > 0
        ? QStringLiteral("  (выбрано %1 x %2)").arg(grid.first).arg(grid.second)
        : QStringLiteral("  (подбор не сработал)");

    if (grid.first <= 0) {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   ") << name
              << QStringLiteral("  — подбор не сработал, проверять нечего") << Qt::endl;
        return;
    }

    if (actual == expected && !expected.isEmpty()) {
        ++g_passed;
        g_out << "  ok       " << name << gridText << Qt::endl;
        return;
    }

    ++g_failed;
    g_out << QStringLiteral("  ПРОВАЛ   ") << name << gridText << Qt::endl;
    g_out << QStringLiteral("      без подбора: ") << formatSpectrum(expected) << Qt::endl;
    g_out << QStringLiteral("      с подбором:  ") << formatSpectrum(actual)   << Qt::endl;
}

static void testAutoTunedGrid()
{
    g_out << Qt::endl << QStringLiteral("Автоподбор сетки") << Qt::endl;

    RunConfig cfg;
    cfg.device = ComputeDevice::Gpu;

    // Узкий код: по замерам --sweep именно здесь у сетки оставался почти
    // двукратный запас.
    cfg.matrix    = Reference::randomMatrix(40, 50, 3);
    cfg.algorithm = Algorithm::SimpleXor;
    cfg.maxRows   = 6;
    checkTuned(QStringLiteral("короткий XOR, узкий код"), cfg);

    // Широкий код: другое ядро по числу слов в строке.
    cfg.matrix    = Reference::randomMatrix(36, 1500, 7);
    cfg.maxRows   = 5;
    checkTuned(QStringLiteral("короткий XOR, широкий код"), cfg);

    // Голей целиком — на нём же сверяется аналитический спектр ниже.
    cfg.matrix    = Reference::golay24_12();
    cfg.maxRows   = 0;
    checkTuned(QStringLiteral("Голей (24,12), полный перебор"), cfg);

    // Код Грея — ядро другое, и оптимум по нитям у него ведёт себя иначе.
    cfg.matrix    = Reference::identity(20);
    cfg.algorithm = Algorithm::GrayCode;
    cfg.maxRows   = 0;
    checkTuned(QStringLiteral("код Грея"), cfg);

    // Дуальный код считается тем же ядром Грея, но по проверочной матрице.
    cfg.matrix    = Reference::hamming7_4();
    cfg.algorithm = Algorithm::DualCode;
    checkTuned(QStringLiteral("дуальный код"), cfg);

    // Сверка с аналитикой, а не только «сам с собой»: единичная матрица
    // на 20 строках даёт биномиальные коэффициенты.
    RunConfig ident;
    ident.device    = ComputeDevice::Gpu;
    ident.matrix    = Reference::identity(20);
    ident.algorithm = Algorithm::SimpleXor;
    ident.autoTune  = true;
    check(QStringLiteral("подбор: I(20) против биномов"), ident,
          Reference::identityPartialSpectrum(20, 20));

    // Длинный путь. Там сетка задаёт ещё и размер чанка, то есть разбиение
    // расчёта — тем важнее убедиться, что спектр от неё не зависит.
    checkTuned(QStringLiteral("длинный код (k=70)"), longConfig(ComputeDevice::Gpu));

    if (!g_gpuAvailable)
        return;

    // На CPU подбирать нечего.
    RunConfig cpu;
    cpu.matrix    = Reference::golay24_12();
    cpu.algorithm = Algorithm::SimpleXor;
    cpu.device    = ComputeDevice::Cpu;
    const QPair<int, int> cpuGrid = tunedGridFor(cpu);
    if (cpuGrid.first == 0) {
        ++g_passed;
        g_out << "  ok       " << QStringLiteral("CPU: подбор не применяется") << Qt::endl;
    } else {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   CPU: подбор вмешался") << Qt::endl;
    }
}

// Подбор гоняется заново при каждом запуске и может выбрать другую сетку,
// чем в прошлый раз. Значит, чекпоинт обязан переноситься и через него.
static void testAutoTunedCheckpoints()
{
    g_out << Qt::endl << QStringLiteral("Автоподбор: перенос чекпоинтов") << Qt::endl;

    RunConfig base;
    base.matrix    = Reference::golay24_12();
    base.algorithm = Algorithm::SimpleXor;
    base.device    = ComputeDevice::Gpu;

    RunConfig tuned = base;  tuned.autoTune = true;
    RunConfig plain = base;  plain.autoTune = false;

    checkResume(QStringLiteral("подбор -> подбор"),      tuned, tuned, 1000, 1000);
    checkResume(QStringLiteral("без подбора -> подбор"), plain, tuned, 1000, 1000);
    checkResume(QStringLiteral("подбор -> без подбора"), tuned, plain, 1000, 1000);

    // Длинный путь. Подбор меняет там размер чанка (блоки x нити x 4096),
    // то есть всё разбиение расчёта, поэтому задача взята заведомо больше
    // одного чанка: иначе обрыва не случится и проверять будет нечего.
    RunConfig lngPlain;
    lngPlain.matrix     = Reference::identity(70);
    lngPlain.algorithm  = Algorithm::SimpleXor;
    lngPlain.maxRows    = 7;
    lngPlain.device     = ComputeDevice::Gpu;
    lngPlain.threadsCpu = 4;
    RunConfig lngTuned = lngPlain;  lngTuned.autoTune = true;

    checkResume(QStringLiteral("длинный: подбор -> подбор"),
                lngTuned, lngTuned, 200000000, 300000000);
    checkResume(QStringLiteral("длинный: без подбора -> подбор"),
                lngPlain, lngTuned, 200000000, 300000000);
    checkResume(QStringLiteral("длинный: подбор -> без подбора"),
                lngTuned, lngPlain, 200000000, 300000000);

    // Код Грея: маски нумеруются сплошь, точка обрыва — номер маски.
    RunConfig gray;
    gray.matrix    = Reference::identity(22);
    gray.algorithm = Algorithm::GrayCode;
    gray.device    = ComputeDevice::Gpu;
    RunConfig grayTuned = gray;  grayTuned.autoTune = true;
    checkResume(QStringLiteral("Грей: подбор -> подбор"), grayTuned, grayTuned, 2000000, 2000000);
    checkResume(QStringLiteral("Грей: подбор -> без подбора"), grayTuned, gray, 2000000, 2000000);
}

// ------------------------------------------------- хранилище автосохранений

// Копия настроек обязана нести обе матрицы. Самописный конструктор
// копирования их пропускал. Работало это только потому, что fromJson
// возвращает настройки через NRVO: без него конструктор копирования отдал бы
// Worker настройки без матрицы.
static void testSettingsCopy()
{
    g_out << Qt::endl << QStringLiteral("Настройки: копирование") << Qt::endl;

    ComputationSettings s;
    s.matrix        = QStringList{ QStringLiteral("1011"), QStringLiteral("0110") };
    s.matrix2       = QStringList{ QStringLiteral("111") };
    s.algorithm = Algorithm::ProductCode;
    s.leonMemoryMb  = 512;

    const ComputationSettings copied(s);
    ComputationSettings assigned;
    assigned = s;
    const ComputationSettings viaJson = ComputationSettings::fromJson(s.toJson());

    auto same = [&s](const ComputationSettings& x) {
        return x.matrix == s.matrix && x.matrix2 == s.matrix2
            && x.algorithm == s.algorithm && x.leonMemoryMb == s.leonMemoryMb;
    };
    expectStore(QStringLiteral("конструктор копирования переносит матрицы"), same(copied));
    expectStore(QStringLiteral("присваивание переносит матрицы"), same(assigned));
    expectStore(QStringLiteral("toJson -> fromJson переносит матрицы"), same(viaJson));
}

static void testAutosaveStore()
{
    g_out << Qt::endl << QStringLiteral("Хранилище автосохранений") << Qt::endl;

    const QString root = QDir::tempPath() + QStringLiteral("/SpectrumTests-store");
    QDir(root).removeRecursively();
    AutosaveStore store(root);

    const Matrix a = Reference::identity(8);
    Matrix b = Reference::identity(8);
    b[0] = QStringLiteral("11000000");   // тот же размер, другая матрица

    // Имя папки: размер как в интерфейсе плюс хеш. Один размер, разные матрицы —
    // разные папки, иначе вторая затёрла бы matrix.txt первой.
    expectStore(QStringLiteral("имя папки начинается с размера кода"),
                AutosaveStore::folderName(a).startsWith(QStringLiteral("8x8-")));
    expectStore(QStringLiteral("имя папки одно и то же при повторном вызове"),
                AutosaveStore::folderName(a) == AutosaveStore::folderName(a));
    expectStore(QStringLiteral("матрицы одного размера не сталкиваются"),
                AutosaveStore::folderName(a) != AutosaveStore::folderName(b));

    AutosaveRecord record;
    record.algorithm = Algorithm::SimpleXor;
    record.enumType  = EnumerationType::Partial;
    record.maxRows   = 4;
    record.finished  = false;
    record.savedAt   = QDateTime::currentDateTime();
    record.state.rOffset     = 3;
    record.state.chunkOffset = 17;
    record.state.doneOps     = 1234;
    record.state.elapsedSec  = 42;
    record.state.spectrum    = QVector<quint64>{ 1, 0, 5, 9 };

    expectStore(QStringLiteral("запись сохраняется"), store.save(a, record));

    AutosaveRecord back;
    const bool loaded = store.load(a, Algorithm::SimpleXor, back);
    expectStore(QStringLiteral("запись читается обратно"), loaded);
    expectStore(QStringLiteral("состояние не изменилось при записи и чтении"),
                loaded && back.state.rOffset == record.state.rOffset
                       && back.state.chunkOffset == record.state.chunkOffset
                       && back.state.doneOps == record.state.doneOps
                       && back.state.spectrum == record.state.spectrum
                       && back.maxRows == record.maxRows
                       && back.enumType == record.enumType);

    // Счётчики больше 2^53 обязаны пережить запись без потерь. Числом в
    // JSON-тексте Qt 5 выводит их через double: 2^53 + 1 читался обратно как
    // 2^53, а 2^63 после приведения к qint64 становился отрицательным. У кода
    // Грея при k >= 54 так округлялись и точка продолжения, и числа спектра.
    {
        const Matrix c = Reference::identity(9);
        AutosaveRecord big = record;
        big.state.chunkOffset = 1ULL << 63;
        big.state.doneOps     = (1ULL << 53) + 1;
        big.state.spectrum    = QVector<quint64>{ 1, (1ULL << 53) + 1, (1ULL << 63) + 5, ~0ULL };
        AutosaveRecord leon = big;
        leon.algorithm  = Algorithm::RandomInfoSets;
        leon.leonTrials = (1ULL << 53) + 1;
        store.save(c, big);
        store.save(c, leon);

        AutosaveRecord bigBack, leonBack;
        const bool bigLoaded = store.load(c, Algorithm::SimpleXor, bigBack)
                            && store.load(c, Algorithm::RandomInfoSets, leonBack);
        expectStore(QStringLiteral("счётчики больше 2^53 читаются без потерь"),
                    bigLoaded && bigBack.state.chunkOffset == big.state.chunkOffset
                              && bigBack.state.doneOps == big.state.doneOps
                              && bigBack.state.spectrum == big.state.spectrum
                              && leonBack.leonTrials == leon.leonTrials);

        // Записи прежнего формата, с числами вместо строк, читаются как раньше.
        const QString folder = root + QLatin1Char('/') + AutosaveStore::folderName(c);
        QFile old(folder + QStringLiteral("/gray.json"));
        const bool written = old.open(QIODevice::WriteOnly)
            && old.write("{\"version\":1,\"algorithm\":1,\"enumType\":0,\"maxRows\":0,"
                         "\"finished\":false,\"savedAt\":\"2026-01-01T00:00:00\","
                         "\"state\":{\"rOffset\":0,\"chunkOffset\":17,\"doneOps\":1234,"
                         "\"elapsedSec\":42,\"spectrum\":[1,0,5,9]}}") > 0;
        old.close();
        AutosaveRecord oldBack;
        expectStore(QStringLiteral("запись прежнего формата с числами читается"),
                    written && store.load(c, Algorithm::GrayCode, oldBack)
                            && oldBack.state.chunkOffset == 17
                            && oldBack.state.doneOps == 1234
                            && oldBack.state.spectrum == QVector<quint64>{ 1, 0, 5, 9 });
        store.removeFolder(AutosaveStore::folderName(c));
    }

    expectStore(QStringLiteral("чужой алгоритм не подхватывается"),
                !store.load(a, Algorithm::GrayCode, back));
    expectStore(QStringLiteral("чужая матрица не подхватывается"),
                !store.load(b, Algorithm::SimpleXor, back));

    // Матрица лежит одним файлом на папку, а не в каждой записи: на коде
    // (1000,997) это разница между мегабайтом и мегабайтом на каждое
    // сохранение.
    AutosaveRecord gray = record;
    gray.algorithm = Algorithm::GrayCode;
    store.save(a, gray);
    const QDir folder(root + QLatin1Char('/') + AutosaveStore::folderName(a));
    expectStore(QStringLiteral("матрица одна на папку, записей две"),
                folder.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files).size() == 2
                && folder.entryList(QStringList() << QStringLiteral("matrix.txt"), QDir::Files).size() == 1);
    expectStore(QStringLiteral("матрица читается из папки без изменений"),
                store.matrixOf(AutosaveStore::folderName(a)) == a);

    expectStore(QStringLiteral("в списке обе записи"), store.list().size() == 2);

    // Пока в папке есть другие записи, она остаётся.
    store.remove(a, Algorithm::GrayCode);
    expectStore(QStringLiteral("удаление одной записи не трогает соседнюю"),
                store.contains(a, Algorithm::SimpleXor) && !store.contains(a, Algorithm::GrayCode));
    store.remove(a, Algorithm::SimpleXor);
    expectStore(QStringLiteral("с последней записью уходит и папка матрицы"),
                !folder.exists());

    // Ограничение по числу записей: остаются самые свежие.
    for (int i = 0; i < 5; ++i) {
        Matrix m = Reference::identity(8);
        m[0] = QStringLiteral("1000000") + QString::number(i % 2);
        m[1] = QString::number(i) + QStringLiteral("1000000").mid(1);
        AutosaveRecord r = record;
        r.savedAt = QDateTime::currentDateTime().addDays(-i);
        store.save(m, r);
    }
    store.applyRetention(3, 0);
    expectStore(QStringLiteral("лимит по числу записей соблюдается"),
                store.list().size() == 3);

    store.applyRetention(0, 1);
    const QVector<AutosaveEntry> left = store.list();
    bool allFresh = true;
    for (const AutosaveEntry& e : left)
        if (e.record.savedAt.daysTo(QDateTime::currentDateTime()) > 1)
            allFresh = false;
    expectStore(QStringLiteral("лимит по возрасту соблюдается"), allFresh);

    store.removeAll();
    expectStore(QStringLiteral("удаление всего чистит каталог"), store.list().isEmpty());
    QDir(root).removeRecursively();
}

// Годность записи для расчёта с другим maxRows. Слои по числу складываемых
// строк независимы, поэтому вперёд продолжать можно, а назад нельзя.
static void testCanResume()
{
    g_out << Qt::endl << QStringLiteral("Годность автосохранения") << Qt::endl;

    ComputationSettings settings;
    settings.algorithm = Algorithm::SimpleXor;
    settings.maxRows       = 7;

    AutosaveRecord record;
    record.algorithm = Algorithm::SimpleXor;

    record.state.rOffset = 5;
    expectStore(QStringLiteral("оборванный на слое 5 годится для maxRows 7"),
                canResume(record, settings));

    record.state.rOffset = 8;   // досчитано всё до maxRows = 7
    expectStore(QStringLiteral("досчитанный до 7 годится для maxRows 7"),
                canResume(record, settings));

    settings.maxRows = 8;
    expectStore(QStringLiteral("досчитанный до 7 годится для maxRows 8"),
                canResume(record, settings));

    settings.maxRows = 6;
    expectStore(QStringLiteral("досчитанный до 7 НЕ годится для maxRows 6"),
                !canResume(record, settings));

    record.state.rOffset = 5;
    settings.maxRows = 3;
    expectStore(QStringLiteral("ушедший до слоя 5 НЕ годится для maxRows 3"),
                !canResume(record, settings));

    // Недосчитанный слой: его вклад уже в спектре, значит остановиться на
    // предыдущем нельзя — иначе спектр вышел бы завышенным.
    record.state.rOffset     = 8;
    record.state.chunkOffset = 118656860160ULL;
    settings.maxRows = 7;
    expectStore(QStringLiteral("посреди слоя 8 НЕ годится для maxRows 7"),
                !canResume(record, settings));
    settings.maxRows = 8;
    expectStore(QStringLiteral("посреди слоя 8 годится для maxRows 8"),
                canResume(record, settings));
    record.state.chunkOffset = 0;
    settings.maxRows = 7;
    expectStore(QStringLiteral("на границе слоя 8 годится для maxRows 7"),
                canResume(record, settings));

    // У кода Грея слоёв нет, maxRows там ни при чём.
    settings.algorithm = Algorithm::GrayCode;
    record.algorithm = Algorithm::GrayCode;
    record.state.rOffset = 0;
    expectStore(QStringLiteral("код Грея годится независимо от maxRows"),
                canResume(record, settings));
}

// Досчёт: посчитать до maxRows = n, потом попросить n + 1 и сверить с прямым
// расчётом до n + 1. Ради этого запись и не удаляется после успеха.
static void checkExtend(const QString& name, RunConfig cfg, int from, int to)
{
    if (cfg.device == ComputeDevice::Gpu && !g_gpuAvailable) {
        g_out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
        return;
    }

    RunConfig target = cfg;  target.maxRows = to;
    clearCheckpoints();
    const Spectrum direct = runWorker(target);

    clearCheckpoints();
    RunConfig first = cfg;   first.maxRows = from;
    runWorker(first);                       // досчитали до конца, запись осталась

    const qint64 saved = savedDoneOps();
    if (saved <= 0) {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   ") << name
              << QStringLiteral("  — после успешного расчёта записи не осталось,"
                                " досчитывать не с чего") << Qt::endl;
        clearCheckpoints();
        return;
    }

    const Spectrum extended = runWorker(target, LoadMode::FromCheckpoint);
    clearCheckpoints();

    if (extended == direct) {
        ++g_passed;
        g_out << "  ok       " << name
              << QStringLiteral("  (досчитано с ") << saved << QStringLiteral(" оп.)") << Qt::endl;
        return;
    }

    ++g_failed;
    g_out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
    g_out << QStringLiteral("      напрямую: ") << formatSpectrum(direct)   << Qt::endl;
    g_out << QStringLiteral("      досчётом: ") << formatSpectrum(extended) << Qt::endl;
}

static void testExtendMaxRows()
{
    g_out << Qt::endl << QStringLiteral("Досчёт до большего числа строк") << Qt::endl;

    RunConfig cfg;
    cfg.matrix     = Reference::identity(20);
    cfg.algorithm  = Algorithm::SimpleXor;
    cfg.device     = ComputeDevice::Cpu;
    cfg.threadsCpu = 4;
    checkExtend(QStringLiteral("CPU I(20): 5 строк, потом 7"), cfg, 5, 7);

    cfg.device = ComputeDevice::Gpu;
    checkExtend(QStringLiteral("GPU I(20): 5 строк, потом 7"), cfg, 5, 7);

    // Длинный путь: там своё разбиение на чанки и свои слои.
    RunConfig lng;
    lng.matrix     = Reference::identity(70);
    lng.algorithm  = Algorithm::SimpleXor;
    lng.device     = ComputeDevice::Gpu;
    lng.threadsCpu = 4;
    checkExtend(QStringLiteral("GPU I(70): 3 строки, потом 4"), lng, 3, 4);
}

// ------------------------------------------------- подписи оси графика

// Регрессия: приложение падало при попытке убрать график вправо. Сплиттер
// схлопывает виджет в нулевую ширину, деление на неё даёт бесконечность, а
// int от бесконечности — INT_MIN. Цикл построения подписей с отрицательным
// шагом не заканчивается и набивает массивы точек, пока не кончится память.
//
// Здесь проверяется только арифметика шага: ни окна, ни QCustomPlot для
// этого поднимать не нужно.
static void testAxisLabelStep()
{
    g_out << Qt::endl << QStringLiteral("Подписи оси графика") << Qt::endl;

    struct Case { int size; int width; const char* what; };
    const Case cases[] = {
        {   50,    0, "график схлопнут"      },
        { 2049,    0, "график схлопнут"      },
        {    0,  900, "спектр пуст"          },
        {   -1,  900, "спектр пуст"          },
        {   50,   -8, "ширина отрицательная" },
    };

    bool ok = true;
    for (const Case& c : cases) {
        const int step = axisLabelStep(c.size, c.width, 30.0);
        if (step != 0) {
            ok = false;
            g_out << QStringLiteral("      %1: ожидался 0, получено %2")
                         .arg(QString::fromUtf8(c.what)).arg(step) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; g_out << "  ok       " << QStringLiteral("вырожденные размеры дают ноль") << Qt::endl; }
    else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   вырожденные размеры") << Qt::endl; }

    // На любых рабочих размерах шаг обязан лежать в [1, size] — только тогда
    // цикл по подписям заканчивается.
    ok = true;
    for (int size : { 1, 2, 25, 50, 300, 2049 }) {
        for (int width : { 1, 2, 7, 31, 200, 900, 4000, 100000 }) {
            const int step = axisLabelStep(size, width, 30.0);
            if (step < 1 || step > size) {
                ok = false;
                g_out << QStringLiteral("      size=%1 width=%2 -> шаг %3")
                             .arg(size).arg(width).arg(step) << Qt::endl;
            }
        }
    }
    if (ok) { ++g_passed; g_out << "  ok       " << QStringLiteral("шаг всегда в пределах [1, длина спектра]") << Qt::endl; }
    else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   шаг вне пределов") << Qt::endl; }

    // И сам цикл: он обязан завершиться и выдать разумное число подписей.
    ok = true;
    for (int size : { 1, 25, 50, 2049 }) {
        for (int width : { 1, 200, 900, 4000 }) {
            const int step = axisLabelStep(size, width, 30.0);
            int labels = 0;
            for (int i = 0; i < size; i += step) {
                if (++labels > size) break;
            }
            if (labels < 1 || labels > size) {
                ok = false;
                g_out << QStringLiteral("      size=%1 width=%2 -> %3 подписей")
                             .arg(size).arg(width).arg(labels) << Qt::endl;
            }
        }
    }
    if (ok) { ++g_passed; g_out << "  ok       " << QStringLiteral("цикл подписей заканчивается") << Qt::endl; }
    else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   цикл подписей") << Qt::endl; }
}

// Матрица шире MAX_COLS не влезает в фиксированные массивы ядер. Раньше предел
// проверялся только в интерфейсе, и вызов Worker напрямую — как здесь —
// приводил к записи за границу codeword[] прямо на видеокарте, молча.
// Теперь запуск ядра обязан отказаться с сообщением.
static void testOversizedMatrixRejected()
{
    g_out << Qt::endl << QStringLiteral("Защита от матрицы сверх предела") << Qt::endl;

    if (!g_gpuAvailable) {
        g_out << QStringLiteral("  ПРОПУСК  (GPU недоступен)") << Qt::endl;
        return;
    }

    const int tooWide = Constants::MAX_COLS + 1;   // 2049 -> wordsPerRow = 33 > 32
    RunConfig cfg;
    cfg.matrix    = QStringList{ QString(tooWide, QLatin1Char('1')),
                                 QString(tooWide, QLatin1Char('0')) };
    cfg.algorithm = Algorithm::GrayCode;
    cfg.device    = ComputeDevice::Gpu;

    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());
    bool errored = false;
    QObject::connect(&worker, &Worker::errorOccurred,
                     [&errored](const QString&) { errored = true; });

    worker.setSettings(makeSettings(cfg));
    worker.initializeRunState(LoadMode::Reset);
    worker.computeSpectrum();

    if (errored) {
        ++g_passed;
        g_out << QStringLiteral("  ok       матрица шириной ") << tooWide
              << QStringLiteral(" отклонена с ошибкой") << Qt::endl;
    } else {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   матрица шириной ") << tooWide
              << QStringLiteral(" принята — ядро пишет за границу массива") << Qt::endl;
    }

    // Код Грея перебирает 2^k масок в 64-битном слове: больше 63 строк для него
    // недопустимо. Проверялось только в диалоге настроек, а прямой вызов давал
    // сдвиг на 64 и больше.
    for (ComputeDevice dev : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
        RunConfig gray;
        gray.matrix    = Reference::identity(70);
        gray.algorithm = Algorithm::GrayCode;
        gray.device    = dev;

        Worker w;
        bool err = false;
        QObject::connect(&w, &Worker::errorOccurred, [&err](const QString&) { err = true; });
        w.setSettings(makeSettings(gray));
        w.initializeRunState(LoadMode::Reset);
        w.computeSpectrum();

        const QString dn = dev == ComputeDevice::Cpu ? QStringLiteral("CPU")
                                                     : QStringLiteral("GPU");
        if (err) {
            ++g_passed;
            g_out << QStringLiteral("  ok       ") << dn
                  << QStringLiteral(" Грей на 70 строках отклонён") << Qt::endl;
        } else {
            ++g_failed;
            g_out << QStringLiteral("  ПРОВАЛ   ") << dn
                  << QStringLiteral(" Грей на 70 строках принят — сдвиг за разрядность")
                  << Qt::endl;
        }
    }
}

// Один заданный расчёт и ничего больше — чтобы профилировщику было что
// показывать без посторонних запусков ядер.
// Запуск: SpectrumTests.exe --profile ident|rand
static int runSingleForProfiling(const QString& which)
{
    RunConfig cfg;
    cfg.algorithm = Algorithm::SimpleXor;
    cfg.maxRows   = 8;
    cfg.device    = ComputeDevice::Gpu;

    if (which == QStringLiteral("ident")) {
        // Вырожденный случай: строка i единичной матрицы это e_i, поэтому вес
        // кодового слова равен числу единиц в маске. Внутри слоя r он у всех
        // одинаков, и весь блок бьёт атомарными операциями в одну ячейку.
        cfg.matrix = Reference::identity(50);
    } else if (which == QStringLiteral("rand")) {
        // Контроль: те же размеры и то же число комбинаций, но веса размазаны.
        cfg.matrix = Reference::randomMatrix(50, 50, 3);
    } else if (which == QStringLiteral("wide")) {
        // Широкий код: строка занимает 32 слова вместо одного, то есть на
        // каждый изменившийся бит приходится 32 чтения матрицы. Здесь и должно
        // быть видно, из какой памяти её выгоднее читать.
        cfg.matrix  = Reference::randomMatrix(40, 2000, 8);
        cfg.maxRows = 6;
    } else if (which == QStringLiteral("maxshared")) {
        // Предельный случай по разделяемой памяти: максимум столбцов и строк
        // для короткого кода. Если матрицу класть в разделяемую, выходит
        // (2049 + 63*32) * 8 = 32.5 КБ на блок, то есть один блок на
        // мультипроцессор — здесь и проверяется, не съедает ли занятость
        // весь выигрыш от быстрого доступа.
        cfg.matrix  = Reference::randomMatrix(63, 2048, 9);
        cfg.maxRows = 6;
    } else if (which == QStringLiteral("bigwide")) {
        // Короткий путь, но объёмом на десятки секунд: 655 млн комбинаций,
        // строка в 32 словах. Здесь разница видна в секундах, а не в шуме.
        cfg.matrix  = Reference::randomMatrix(50, 2000, 10);
        cfg.maxRows = 9;
    } else if (which == QStringLiteral("biglong")) {
        // Длинный путь (k >= 64) той же ширины. Матрица в 63*2048 бит не
        // влезает в константную память, поэтому лежит в глобальной.
        cfg.matrix  = Reference::randomMatrix(70, 2000, 11);
        cfg.maxRows = 6;
    } else if (which == QStringLiteral("hugelong")) {
        // Матрица 900 x 1600 занимает 180 КБ: не помещается ни в константную
        // память, ни в разделяемую. Единственный путь — глобальная память,
        // здесь и проверяется чтение через __ldg.
        cfg.matrix  = Reference::randomMatrix(900, 1600, 13);
        cfg.maxRows = 3;
    } else if (which == QStringLiteral("graywide")) {
        // Код Грея на широкой матрице: тот же расходящийся доступ к строкам,
        // что и в ядре простого XOR, только маски идут подряд.
        cfg.matrix    = Reference::randomMatrix(27, 2000, 12);
        cfg.algorithm = Algorithm::GrayCode;
        cfg.maxRows   = 27;
    } else {
        g_out << QStringLiteral("ожидалось --profile ident|rand|wide|maxshared|bigwide|biglong|graywide")
              << Qt::endl;
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const Spectrum s = runWorker(cfg);
    const double sec = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - t0).count();

    quint64 total = 0;
    for (auto it = s.constBegin(); it != s.constEnd(); ++it) total += it.value();
    g_out << which << QStringLiteral(": %1 с, слов %2, ненулевых весов %3")
                          .arg(sec, 0, 'f', 2).arg(total).arg(s.size()) << Qt::endl;
    return 0;
}

// ------------------------------------------------- золотой файл спектров

// Тяжёлые прогоны с записью точных спектров в файл.
//
// Нужны, когда правка затрагивает сам перебор: тестовые матрицы маленькие и
// «удобные», на них легко не заметить расхождение, которое вылезет на объёме.
// Порядок действий — выгрузить до правки, выгрузить после, сравнить файлы
// побайтово. Любое расхождение означает, что перебор изменился.
//
// Запуск: SpectrumTests.exe --dump <файл>
static int dumpGolden(const QString& path)
{
    // analytic заполняется там, где спектр известен из теории; пустой означает,
    // что абсолютной проверки распределения для случая нет.
    struct Case { QString name; RunConfig cfg; Spectrum analytic; };
    QVector<Case> cases;

    auto add = [&cases](const QString& name, const QStringList& m, Algorithm alg,
                        int maxRows, ComputeDevice dev, int blocks = 64, int threads = 256,
                        const Spectrum& analytic = Spectrum()) {
        RunConfig c;
        c.matrix     = m;
        c.algorithm  = alg;
        c.maxRows    = maxRows;
        c.device     = dev;
        c.blocksGpu  = blocks;
        c.threadsGpu = threads;
        cases.append({ name, c, analytic });
    };

    const QStringList golay = Reference::golay24_12();
    const QStringList m40   = Reference::randomMatrix(40,  60, 1);
    const QStringList m45   = Reference::randomMatrix(45,  80, 2);
    const QStringList m50   = Reference::randomMatrix(50, 100, 3);
    const QStringList m52   = Reference::randomMatrix(52, 120, 4);
    const QStringList m30   = Reference::randomMatrix(30,  64, 5);
    const QStringList m70   = Reference::randomMatrix(70, 120, 6);
    const QStringList m26   = Reference::randomMatrix(26,  64, 7);

    // Процессорные прогоны берутся меньшего объёма: на CPU перебор идёт на
    // порядок медленнее, а покрытие путей от размера не зависит — важно, чтобы
    // задача резалась на несколько слоёв и несколько чанков.
    for (ComputeDevice d : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
        const bool cpu = (d == ComputeDevice::Cpu);
        const QString dn = cpu ? QStringLiteral("CPU") : QStringLiteral("GPU");

        // Короткий путь, простой XOR — то, что меняется.
        add(dn + " XOR Голей полный",  golay, Algorithm::SimpleXor, 12, d);
        add(QStringLiteral("%1 XOR rnd(40,60) r<=%2").arg(dn).arg(cpu ? 6 : 7),
            m40, Algorithm::SimpleXor, cpu ? 6 : 7, d);
        // Код Грея и дуальный — контроль, они меняться не должны.
        add(QStringLiteral("%1 Грей rnd(%2,64)").arg(dn).arg(cpu ? 26 : 30),
            cpu ? m26 : m30, Algorithm::GrayCode, cpu ? 26 : 30, d);
        add(dn + " дуальный Голей",    golay, Algorithm::DualCode,  12, d);
        // Длинный путь — тоже контроль.
        add(QStringLiteral("%1 XOR rnd(70,120) r<=%2").arg(dn).arg(cpu ? 4 : 5),
            m70, Algorithm::SimpleXor, cpu ? 4 : 5, d);
    }

    // Объёмные прогоны только на видеокарте: на процессоре это часы.
    add("GPU XOR rnd(45,80) r<=9",  m45, Algorithm::SimpleXor,  9, ComputeDevice::Gpu);
    add("GPU XOR rnd(50,100) r<=10", m50, Algorithm::SimpleXor, 10, ComputeDevice::Gpu);
    add("GPU XOR rnd(52,120) r<=11", m52, Algorithm::SimpleXor, 11, ComputeDevice::Gpu);
    // Другое разбиение: результат обязан не зависеть от числа блоков и нитей.
    add("GPU XOR rnd(50,100) r<=10 (8x64)", m50, Algorithm::SimpleXor, 10,
        ComputeDevice::Gpu, 8, 64);

    // Единичные матрицы: I(n) порождает вообще все слова, поэтому спектр равен
    // C(n,w) точно, а при частичном переборе — C(n,w) для w <= maxRows.
    // На случайных матрицах сверять можно только количество слов; здесь
    // проверяется всё распределение целиком, без всякого предыдущего прогона.
    for (ComputeDevice d : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
        const bool cpu = (d == ComputeDevice::Cpu);
        const QString dn = cpu ? QStringLiteral("CPU") : QStringLiteral("GPU");
        const int mr = cpu ? 6 : 7;
        add(QStringLiteral("%1 XOR I(40) r<=%2").arg(dn).arg(mr),
            Reference::identity(40), Algorithm::SimpleXor, mr, d,
            64, 256, Reference::identityPartialSpectrum(40, mr));
        add(dn + " XOR I(20) полный", Reference::identity(20), Algorithm::SimpleXor, 20, d,
            64, 256, Reference::identityPartialSpectrum(20, 20));
        add(dn + " Грей I(24)", Reference::identity(24), Algorithm::GrayCode, 24, d,
            64, 256, Reference::identityPartialSpectrum(24, 24));
    }
    add("GPU XOR I(50) r<=10", Reference::identity(50), Algorithm::SimpleXor, 10,
        ComputeDevice::Gpu, 64, 256, Reference::identityPartialSpectrum(50, 10));

    // Длинный путь (k > 63) с аналитически известным распределением: I(70)
    // порождает все слова, поэтому спектр равен C(70,w) для каждого веса.
    // 144 млн комбинаций — на таком объёме гонка в подготовке стартовых масок
    // проявилась бы перекосом весов, а не только недостачей в сумме.
    add("GPU XOR I(70) r<=6", Reference::identity(70), Algorithm::SimpleXor, 6,
        ComputeDevice::Gpu, 64, 256, Reference::identityPartialSpectrum(70, 6));
    add("CPU XOR I(70) r<=6", Reference::identity(70), Algorithm::SimpleXor, 6,
        ComputeDevice::Cpu, 64, 256, Reference::identityPartialSpectrum(70, 6));
    // То же при мелком разбиении: чанков становится много, значит много и
    // перекладываний буфера стартовых масок.
    add("GPU XOR I(70) r<=6 (8x32)", Reference::identity(70), Algorithm::SimpleXor, 6,
        ComputeDevice::Gpu, 8, 32, Reference::identityPartialSpectrum(70, 6));
    // Длинный путь + матрица в ГЛОБАЛЬНОЙ памяти + округление числа слов —
    // сочетание, которого не было ни в одном случае, и оно скрыло настоящий
    // баг: в запасной ветке цикл шёл до округлённого числа слов, а шаг строки
    // в глобальной памяти настоящий, и лишние слова читались из начала
    // следующей строки.
    //
    // I(900): матрица 900 x 15 слов = 108 КБ, в разделяемую (40 КБ) не влезает;
    // 15 слов округляются до 16. Спектр при этом известен точно — C(900,w).
    add("GPU XOR I(900) r<=3, глобальная память", Reference::identity(900),
        Algorithm::SimpleXor, 3, ComputeDevice::Gpu, 64, 256,
        Reference::identityPartialSpectrum(900, 3));

    // Длинный путь на случайной матрице, 670 млн комбинаций.
    add("GPU XOR rnd(90,300) r<=6", Reference::randomMatrix(90, 300, 14),
        Algorithm::SimpleXor, 6, ComputeDevice::Gpu);
    // Тот же расчёт при мелком разбиении — распределение обязано не измениться.
    add("GPU XOR I(50) r<=10 (8x64)", Reference::identity(50), Algorithm::SimpleXor, 10,
        ComputeDevice::Gpu, 8, 64, Reference::identityPartialSpectrum(50, 10));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        g_out << QStringLiteral("не удалось открыть ") << path << Qt::endl;
        return 1;
    }
    QTextStream fs(&f);
    fs.setCodec("UTF-8");

    for (const Case& c : cases) {
        if (c.cfg.device == ComputeDevice::Gpu && !g_gpuAvailable) {
            g_out << QStringLiteral("  ПРОПУСК  ") << c.name << Qt::endl;
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const Spectrum s = runWorker(c.cfg);
        const double sec = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - t0).count();

        quint64 totalWords = 0;
        for (auto it = s.constBegin(); it != s.constEnd(); ++it) totalWords += it.value();

        fs << c.name << '\n';
        for (auto it = s.constBegin(); it != s.constEnd(); ++it)
            fs << "  " << it.key() << ' ' << it.value() << '\n';
        fs << "  всего " << totalWords << '\n';

        // Абсолютные проверки — не сравнение с прошлым прогоном, а с теорией.
        //
        // 1. Число перебранных слов равно сумме сочетаний и не зависит от
        //    содержимого матрицы: ловит потерянные и посчитанные дважды
        //    комбинации.
        QString verdict;
        const quint64 wantTotal = expectedTotalOps(c.cfg);
        if (totalWords != wantTotal) {
            ++g_failed;
            verdict = QStringLiteral("  ПРОВАЛ: слов %1, а должно быть %2")
                          .arg(totalWords).arg(wantTotal);
        }
        // 2. Там, где спектр известен из теории, сверяем распределение целиком.
        else if (!c.analytic.isEmpty()) {
            if (s == stripZeros(c.analytic)) {
                ++g_passed;
                verdict = QStringLiteral("  == C(n,w)");
            } else {
                ++g_failed;
                verdict = QStringLiteral("  ПРОВАЛ: спектр разошёлся с C(n,w)");
                for (auto it = stripZeros(c.analytic).constBegin();
                     it != stripZeros(c.analytic).constEnd(); ++it)
                    if (s.value(it.key(), 0) != it.value())
                        verdict += QStringLiteral("\n      вес %1: ожидалось %2, получено %3")
                                       .arg(it.key()).arg(it.value())
                                       .arg(s.value(it.key(), 0));
            }
        }
        else {
            ++g_passed;
        }

        g_out << QStringLiteral("  %1  %2 с, слов %3%4")
                     .arg(c.name, -34).arg(sec, 0, 'f', 2).arg(totalWords).arg(verdict)
              << Qt::endl;
        g_out.flush();
    }
    f.close();
    g_out << Qt::endl
          << QStringLiteral("записано в ") << path << Qt::endl
          << QStringLiteral("проверок пройдено ") << g_passed
          << QStringLiteral(", провалено ") << g_failed << Qt::endl;
    return g_failed == 0 ? 0 : 1;
}

// Перебор параметров запуска.
//
// Занятость упирается не в регистры и не в разделяемую память, а в размер
// сетки: на RTX 3070 сорок шесть мультипроцессоров, а по умолчанию блоков
// всего 64, то есть меньше полутора на каждый. Здесь это проверяется замером,
// а не рассуждением.
//
// Запуск: SpectrumTests.exe --sweep <случай>
// Случаи для --sweep и --tune. false — имя не опознано.
static bool sweepCase(const QString& which, RunConfig& base)
{
    base = RunConfig();
    base.device = ComputeDevice::Gpu;

    if (which == QStringLiteral("wide")) {
        base.matrix    = Reference::randomMatrix(50, 2000, 10);
        base.algorithm = Algorithm::SimpleXor;
        base.maxRows   = 10;
    } else if (which == QStringLiteral("narrow")) {
        base.matrix    = Reference::randomMatrix(50, 50, 3);
        base.algorithm = Algorithm::SimpleXor;
        base.maxRows   = 11;
    } else if (which == QStringLiteral("gray")) {
        // Случай намеренно крупный: на прежних 28 строках весь расчёт
        // укладывался в три сотых секунды, и мерить там было нечего.
        base.matrix    = Reference::randomMatrix(35, 1000, 12);
        base.algorithm = Algorithm::GrayCode;
        base.maxRows   = 35;
    } else if (which == QStringLiteral("long")) {
        base.matrix    = Reference::randomMatrix(70, 1000, 11);
        base.algorithm = Algorithm::SimpleXor;
        // Восемь строк вместо шести: на шести весь расчёт занимал 0.15 с,
        // и разница между сетками тонула в накладных расходах.
        base.maxRows   = 8;
    } else {
        return false;
    }
    return true;
}

static bool sweepCase(const QString& which, RunConfig& base);
static bool loadMatrixOrCase(const QString& which, RunConfig& cfg);

// Проба потолка так, как её гоняет приложение по кнопке «Замерить».
//
// Нужна для сверки: --rate меряет частоту по целому расчёту, --probe — по
// полутора секундам с последнего слоя. Числа обязаны сойтись, иначе проба
// показывает пользователю не то, что он получит.
//
// Запуск: SpectrumTests.exe --probe <случай|файл матрицы> [<строк>]
static int probeOnly(const QString& which, int rows)
{
    if (!g_gpuAvailable) {
        g_out << QStringLiteral("GPU недоступен") << Qt::endl;
        return 1;
    }
    clearCheckpoints();

    RunConfig cfg;
    if (!loadMatrixOrCase(which, cfg))
        return 2;
    if (rows > 0)
        cfg.maxRows = rows;

    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());
    worker.setGridTuningThreshold(0.0);

    double measured = -1.0;
    QObject::connect(&worker, &Worker::updateRateMeasured,
                     [&measured](double perSecond) { measured = perSecond; });
    // В приложении пробу останавливает таймер интерфейса. Здесь цикла событий
    // нет, поэтому отдельный поток: он спит столько же и зовёт тот же cancel.
    std::thread stopper;
    QObject::connect(&worker, &Worker::updateRateProbeStarted, [&worker, &stopper]() {
        stopper = std::thread([&worker]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(Constants::PROBE_DURATION_MS));
            worker.cancel();
        });
    });

    worker.setSettings(makeSettings(cfg));
    worker.measureUpdateRate();
    if (stopper.joinable())
        stopper.join();

    g_out << Qt::endl
          << QStringLiteral("матрица %1 x %2, строк в переборе %3, сетка %4 x %5")
                 .arg(cfg.matrix.isEmpty() ? 0 : cfg.matrix.first().size())
                 .arg(cfg.matrix.size()).arg(cfg.maxRows)
                 .arg(cfg.blocksGpu).arg(cfg.threadsGpu) << Qt::endl
          << QStringLiteral("проба намерила %1 отправок в секунду")
                 .arg(measured, 0, 'f', 2) << Qt::endl;
    return 0;
}

// Имя случая из sweepCase или путь к файлу с матрицей.
static bool loadMatrixOrCase(const QString& which, RunConfig& cfg)
{
    if (sweepCase(which, cfg))
        return true;

    // Классические коды по параметрам: «bch:m,представителей[,e]» и
    // «hamming:r[,e]», e — расширить проверкой чётности.
    const QStringList spec = which.section(QLatin1Char(':'), 1).split(QLatin1Char(','));
    if (which.startsWith(QStringLiteral("bch:")) && spec.size() >= 2) {
        cfg = RunConfig();
        cfg.matrix = Bch::build(spec.at(0).toInt(), spec.at(1).toInt(),
                                spec.size() > 2 && spec.at(2) == QStringLiteral("e"), 0).rows;
        return !cfg.matrix.isEmpty();
    }
    if (which.startsWith(QStringLiteral("hamming:")) && !spec.isEmpty()) {
        cfg = RunConfig();
        cfg.matrix = Hamming::build(spec.at(0).toInt(),
                                    spec.size() > 1 && spec.at(1) == QStringLiteral("e"), 0).rows;
        return !cfg.matrix.isEmpty();
    }

    QFile file(which);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        g_out << QStringLiteral("не открыть матрицу: ") << which << Qt::endl;
        return false;
    }
    cfg = RunConfig();
    cfg.device    = ComputeDevice::Gpu;
    cfg.algorithm = Algorithm::SimpleXor;
    const QStringList lines = QString::fromUtf8(file.readAll())
                                  .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString row = line.trimmed();
        if (!row.isEmpty()) cfg.matrix.append(row);
    }
    return !cfg.matrix.isEmpty();
}

// Как часто спектр на самом деле уходит в интерфейс.
//
// Понадобилось, когда на интервале 33 мс спектр стал обновляться реже, чем на
// 100 мс. GUI-поток при этом простаивал на 98 %, то есть дело не в том, что
// интерфейс не успевает рисовать, а в том, что ему нечего рисовать. Здесь окна
// нет вовсе: сигнал ловится напрямую в потоке расчёта, и видно, что отдаёт сам
// воркер.
//
// Запуск: SpectrumTests.exe --rate <случай|файл матрицы> <мс> [<строк>]
static int updateRate(const QString& which, int intervalMs, int rows)
{
    if (!g_gpuAvailable) {
        g_out << QStringLiteral("GPU недоступен") << Qt::endl;
        return 1;
    }
    clearCheckpoints();

    RunConfig cfg;
    if (!loadMatrixOrCase(which, cfg))
        return 2;
    if (rows > 0) cfg.maxRows = rows;

    ComputationSettings settings = makeSettings(cfg);
    settings.intervals.updateSpectrumInterval = intervalMs;

    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());

    // Соединение прямое: обработчик выполняется в потоке расчёта ровно в
    // момент отправки, без очереди событий.
    QVector<double> stamps;
    const auto started = std::chrono::steady_clock::now();
    QObject::connect(&worker, &Worker::spectrumUpdated,
                     [&stamps, started](const SpectrumCounts&) {
        stamps.append(std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started).count());
    });

    worker.setSettings(settings);
    worker.setGridTuningThreshold(0.0);
    worker.initializeRunState(LoadMode::Reset);
    worker.computeSpectrum();

    const double total = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started).count();

    g_out << Qt::endl
          << QStringLiteral("матрица %1 x %2, строк в переборе %3, сетка %4 x %5")
                 .arg(cfg.matrix.isEmpty() ? 0 : cfg.matrix.first().size())
                 .arg(cfg.matrix.size()).arg(cfg.maxRows)
                 .arg(cfg.blocksGpu).arg(cfg.threadsGpu) << Qt::endl
          << QStringLiteral("интервал в настройках: %1 мс").arg(intervalMs) << Qt::endl
          << QStringLiteral("расчёт занял %1 с, отправок спектра %2")
                 .arg(total / 1000.0, 0, 'f', 2).arg(stamps.size()) << Qt::endl;

    if (stamps.size() < 2) {
        g_out << QStringLiteral("отправок слишком мало, увеличьте число строк") << Qt::endl;
        return 0;
    }

    QVector<double> gaps;
    gaps.reserve(stamps.size() - 1);
    for (int i = 1; i < stamps.size(); ++i)
        gaps.append(stamps.at(i) - stamps.at(i - 1));
    std::sort(gaps.begin(), gaps.end());

    // Последовательность пауз как есть: средние прячут структуру, а она тут
    // и есть ответ — видно, идут ли отправки ровно или пачками.
    QStringList shown;
    for (int i = 1; i < stamps.size() && shown.size() < 40; ++i)
        shown.append(QString::number(stamps.at(i) - stamps.at(i - 1), 'f', 1));
    g_out << QStringLiteral("паузы подряд, мс: ") << shown.join(QStringLiteral(" ")) << Qt::endl;

    const double sum = std::accumulate(gaps.begin(), gaps.end(), 0.0);
    g_out << QStringLiteral("пауза между отправками: медиана %1 мс, среднее %2 мс, "
                            "минимум %3, максимум %4")
                 .arg(gaps.at(gaps.size() / 2), 0, 'f', 1)
                 .arg(sum / gaps.size(), 0, 'f', 1)
                 .arg(gaps.first(), 0, 'f', 1)
                 .arg(gaps.last(), 0, 'f', 1) << Qt::endl
          // Делить надо на всё время прогона, а не на сумму пауз: при двух
          // отправках сумма пауз — это одна пауза, и получается бодрое «86 в
          // секунду» вместо честных 0,3.
          << QStringLiteral("получилось %1 отправок в секунду")
                 .arg(1000.0 * stamps.size() / total, 0, 'f', 2) << Qt::endl;
    return 0;
}

// Только подбор: его таблица замеров и сравнение с умолчанием. Полный перебор
// сетки в --sweep занимает минуты, а для правки самого подбора нужен быстрый
// цикл.
//
// Запуск: SpectrumTests.exe --tune <случай>
static int tuneOnly(const QString& which)
{
    RunConfig base;
    if (!sweepCase(which, base)) {
        g_out << QStringLiteral("ожидалось --tune wide|narrow|gray|long") << Qt::endl;
        return 2;
    }
    if (!g_gpuAvailable) {
        g_out << QStringLiteral("GPU недоступен") << Qt::endl;
        return 1;
    }

    g_out << QStringLiteral("Подбор сетки, случай ") << which << Qt::endl;
    g_out.flush();

    const QPair<int, int> chosen = tunedGridFor(base, true);
    fflush(stdout);

    RunConfig plain = base;
    RunConfig tuned = base;  tuned.autoTune = true;

    // Порядок А-Б-А: за минуты перебора карта прогревается и время одной и той
    // же конфигурации уезжает на четверть. Умолчание меряется до и после, и
    // сравнение идёт со средним — иначе дрейф не отличить от эффекта подбора.
    auto timeRun = [](const RunConfig& cfg, Spectrum& outSpectrum) {
        const auto t = std::chrono::steady_clock::now();
        outSpectrum = runWorker(cfg);
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t).count();
    };

    Spectrum sp1, st, sp2;
    const double plain1 = timeRun(plain, sp1);
    const double tunedSec = timeRun(tuned, st);
    const double plain2 = timeRun(plain, sp2);
    const double plainSec = (plain1 + plain2) / 2.0;

    g_out << Qt::endl
          << QStringLiteral("настройки %1 x %2: %3 и %4 с (дрейф %5 %)")
                 .arg(base.blocksGpu).arg(base.threadsGpu)
                 .arg(plain1, 0, 'f', 2).arg(plain2, 0, 'f', 2)
                 .arg(plain1 > 0 ? (plain2 / plain1 - 1.0) * 100.0 : 0.0, 0, 'f', 1) << Qt::endl
          << QStringLiteral("подбор выбрал %1 x %2: %3 с, выигрыш %4x")
                 .arg(chosen.first).arg(chosen.second).arg(tunedSec, 0, 'f', 2)
                 .arg(tunedSec > 0 ? plainSec / tunedSec : 0.0, 0, 'f', 2)
          << ((sp1 == st && sp1 == sp2) ? QString()
                                        : QStringLiteral("   ВНИМАНИЕ: спектр разошёлся"))
          << Qt::endl;
    return 0;
}

static int sweepLaunchParams(const QString& which)
{
    RunConfig base;
    if (!sweepCase(which, base)) {
        g_out << QStringLiteral("ожидалось --sweep wide|narrow|gray|long") << Qt::endl;
        return 2;
    }

    if (!g_gpuAvailable) {
        g_out << QStringLiteral("GPU недоступен") << Qt::endl;
        return 1;
    }

    // Список блоков строится от числа мультипроцессоров этой карты, а не от
    // зашитых чисел: на другой карте они превращаются в бессмыслицу. 64 в
    // списке отдельно — это нынешнее умолчание, с ним и сравниваем.
    int smCount = 0;
    {
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess)
            smCount = prop.multiProcessorCount;
    }
    if (smCount <= 0) smCount = 8;

    QVector<int> blocks;
    for (int perSm : { 1, 2, 3, 4, 6, 8 })
        blocks << perSm * smCount;
    if (!blocks.contains(64)) blocks << 64;
    std::sort(blocks.begin(), blocks.end());
    const QVector<int> threads { 64, 128, 256, 512, 1024 };

    g_out << QStringLiteral("Перебор параметров, случай ") << which
          << QStringLiteral(" (%1 мультипроцессоров)").arg(smCount) << Qt::endl << Qt::endl;
    g_out << QStringLiteral("блоки \ нити");
    for (int t : threads) g_out << QStringLiteral("%1").arg(t, 9);
    g_out << Qt::endl;

    double best = 1e9;
    int bestB = 0, bestT = 0;
    Spectrum reference;

    for (int b : blocks) {
        g_out << QStringLiteral("%1").arg(b, 12);
        for (int t : threads) {
            RunConfig cfg = base;
            cfg.blocksGpu  = b;
            cfg.threadsGpu = t;

            const auto t0 = std::chrono::steady_clock::now();
            const Spectrum s = runWorker(cfg);
            const double sec = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - t0).count();

            // Результат обязан не зависеть от разбиения — сверяем с первым.
            if (reference.isEmpty()) reference = s;
            const bool ok = (s == reference);

            if (ok && sec < best) { best = sec; bestB = b; bestT = t; }
            g_out << QStringLiteral("%1").arg(ok ? QStringLiteral("%1").arg(sec, 0, 'f', 2)
                                                 : QStringLiteral("ПЛОХО"), 9);
            g_out.flush();
        }
        g_out << Qt::endl;
    }

    g_out << Qt::endl
          << QStringLiteral("лучшее: %1 блоков x %2 нитей, %3 с")
                 .arg(bestB).arg(bestT).arg(best, 0, 'f', 2) << Qt::endl;
    // Для сравнения — то, что стоит по умолчанию сейчас.
    RunConfig cur = base; cur.blocksGpu = 64; cur.threadsGpu = 256;
    const auto t0 = std::chrono::steady_clock::now();
    const Spectrum s = runWorker(cur);
    const double sec = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - t0).count();
    g_out << QStringLiteral("сейчас по умолчанию 64 x 256: %1 с, выигрыш %2x")
                 .arg(sec, 0, 'f', 2).arg(best > 0 ? sec / best : 0.0, 0, 'f', 2)
          << (s == reference ? QString()
                             : QStringLiteral("   ВНИМАНИЕ: спектр разошёлся"))
          << Qt::endl;

    // И то же самое с автоподбором — вместе со временем самого подбора.
    // Здесь видно главное: насколько выбранная замером сетка отстаёт от
    // найденной полным перебором и окупается ли подбор вообще.
    RunConfig tuned = base;
    tuned.autoTune = true;
    const auto t1 = std::chrono::steady_clock::now();
    const Spectrum st = runWorker(tuned);
    const double tunedSec = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - t1).count();
    g_out.flush();
    const QPair<int, int> chosen = tunedGridFor(base, true);

    g_out << QStringLiteral("автоподбор выбрал %1 x %2: %3 с, выигрыш %4x, "
                            "до оптимума %5x")
                 .arg(chosen.first).arg(chosen.second)
                 .arg(tunedSec, 0, 'f', 2)
                 .arg(tunedSec > 0 ? sec / tunedSec : 0.0, 0, 'f', 2)
                 .arg(best > 0 ? tunedSec / best : 0.0, 0, 'f', 2)
          << (st == reference ? QString()
                              : QStringLiteral("   ВНИМАНИЕ: спектр разошёлся"))
          << Qt::endl;
    return 0;
}

// ------------------------------------------------------------------- замер

// Замер скорости ядер. Тестовые матрицы намеренно маленькие — на них разницы
// не видно, поэтому для оценки правок в .cu нужен отдельный прогон покрупнее.
// Запуск: SpectrumTests.exe --bench
static void benchmark()
{
    struct Case {
        QString    name;
        RunConfig  cfg;
        quint64    ops;
    };

    QVector<Case> cases;

    // Код Грея: 2^26 масок на каждое устройство.
    for (ComputeDevice dev : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
        RunConfig c;
        c.matrix    = Reference::identity(28);
        c.algorithm = Algorithm::GrayCode;
        c.device    = dev;
        cases.append({ QStringLiteral("%1 Грей I(28)")
                           .arg(dev == ComputeDevice::Cpu ? QStringLiteral("CPU")
                                                          : QStringLiteral("GPU")),
                       c, 1ULL << 28 });
    }

    // Простой XOR по слоям: самый нагруженный режим короткого пути.
    for (ComputeDevice dev : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
        RunConfig c;
        c.matrix    = Reference::identity(40);
        c.algorithm = Algorithm::SimpleXor;
        c.maxRows   = 7;
        c.device    = dev;
        quint64 total = 0;
        for (quint64 r = 0; r <= 7; ++r) total += Reference::binom(40, r);
        cases.append({ QStringLiteral("%1 XOR I(40) maxRows=7")
                           .arg(dev == ComputeDevice::Cpu ? QStringLiteral("CPU")
                                                          : QStringLiteral("GPU")),
                       c, total });
    }

    g_out << QStringLiteral("Замер скорости") << Qt::endl;
    for (const Case& c : cases) {
        if (c.cfg.device == ComputeDevice::Gpu && !g_gpuAvailable) {
            g_out << QStringLiteral("  ПРОПУСК  ") << c.name << Qt::endl;
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const Spectrum s = runWorker(c.cfg);
        const double sec = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - t0).count();

        quint64 got = 0;
        for (auto it = s.constBegin(); it != s.constEnd(); ++it) got += it.value();

        g_out << QStringLiteral("  %1: %2 с, %3 млн масок/с%4")
                     .arg(c.name, -26)
                     .arg(sec, 0, 'f', 2)
                     .arg(sec > 0 ? c.ops / sec / 1e6 : 0.0, 0, 'f', 1)
                     .arg(got == c.ops ? QString()
                                       : QStringLiteral("   ВНИМАНИЕ: обработано %1 из %2")
                                             .arg(got).arg(c.ops))
              << Qt::endl;
    }
}

// Подрезка списка частот обновления по замеренному потолку.
//
// Потолок разнится в разы: спектр уходит только на границе чанка, а длина
// чанка зависит от кода, сетки и пути расчёта. Замерено после кольца копий —
// от 6,9 отправки в секунду на широком коде до 54,6 на матрице 336x96.
// Пункты быстрее потолка в списке блокируются, и вся арифметика этого — здесь.
static void testUpdateIntervals()
{
    g_out << Qt::endl << QStringLiteral("Достижимые интервалы обновления") << Qt::endl;

    // Список из настроек, миллисекунды.
    const QVector<int> list { 100, 250, 500, 1000, 5000, 10000, 30000, 60000 };

    struct Case { int ms; double rate; bool want; const char* what; };
    const Case cases[] = {
        {  100, 10.0, true,  "интервал ровно на потолке достижим"     },
        {  100,  9.9, false, "чуть быстрее потолка - уже нет"         },
        {  100, 54.6, true,  "потолок с запасом"                      },
        { 1000,  6.9, true,  "секунда достижима на худшем из замеров" },
        {  100,  0.0, false, "замера нет - недостижимо ничего"        },
        {  100, -1.0, false, "отрицательный потолок"                  },
        {    0, 10.0, false, "нулевой интервал"                       },
        {   -5, 10.0, false, "отрицательный интервал"                 },
    };

    bool ok = true;
    for (const Case& c : cases) {
        if (intervalReachable(c.ms, c.rate) != c.want) {
            ok = false;
            g_out << QStringLiteral("      %1: %2 мс при потолке %3")
                         .arg(QString::fromUtf8(c.what)).arg(c.ms).arg(c.rate) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; g_out << "  ok       " << QStringLiteral("достижимость интервала") << Qt::endl; }
    else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   достижимость интервала") << Qt::endl; }

    // Секунда обязана оставаться достижимой на любом замере, который вообще
    // что-то поймал: на ней стоит заблокированный список.
    ok = true;
    for (double rate : { 1.0, 2.0, 6.9, 13.7, 54.6 }) {
        if (!intervalReachable(1000, rate)) {
            ok = false;
            g_out << QStringLiteral("      потолок %1 не пускает секунду").arg(rate) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; g_out << "  ok       " << QStringLiteral("секунда достижима на любом пойманном замере") << Qt::endl; }
    else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   секунда недостижима") << Qt::endl; }

    struct Pick { double rate; int want; const char* what; };
    const Pick picks[] = {
        { 54.6,  100, "быстрая конфигурация открывает весь список" },
        {  6.9,  250, "широкий код: 0,1 с не проходит, 0,25 - да"  },
        {  1.5, 1000, "полтора в секунду - только от секунды"      },
        {  0.5, 5000, "полраза в секунду"                          },
        {  0.0,    0, "замера нет - не подходит ничего"            },
        { 1e-9,    0, "потолок ниже самого медленного пункта"      },
    };

    ok = true;
    for (const Pick& p : picks) {
        const int got = fastestAllowed(list, p.rate);
        if (got != p.want) {
            ok = false;
            g_out << QStringLiteral("      %1: ожидалось %2, получено %3")
                         .arg(QString::fromUtf8(p.what)).arg(p.want).arg(got) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; g_out << "  ok       " << QStringLiteral("выбор самого частого допустимого") << Qt::endl; }
    else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   выбор допустимого") << Qt::endl; }

    // Что бы ни вернул fastestAllowed, это обязано быть достижимо: иначе
    // подрезка сама поставила бы пункт, который не работает.
    ok = true;
    for (double rate : { 0.1, 0.9, 1.0, 3.3, 6.9, 20.0, 54.6, 1000.0 }) {
        const int got = fastestAllowed(list, rate);
        if (got != 0 && !intervalReachable(got, rate)) {
            ok = false;
            g_out << QStringLiteral("      потолок %1 -> %2 мс, а это недостижимо")
                         .arg(rate).arg(got) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; g_out << "  ok       " << QStringLiteral("выбранный интервал всегда достижим") << Qt::endl; }
    else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   выбран недостижимый интервал") << Qt::endl; }
}

// Проба потолка не имеет права трогать состояние расчёта: она идёт по тем же
// путям, что и настоящий расчёт, но с последнего слоя и без сохранений. Если
// бы она писала автосохранение, спектр в нём был бы огрызком — только
// последний слой, — и продолжение с него дало бы завышенный ответ.
static void testProbeLeavesNoTrace()
{
    g_out << Qt::endl << QStringLiteral("Замер потолка обновления") << Qt::endl;
    if (!g_gpuAvailable) {
        g_out << QStringLiteral("  пропуск  GPU недоступен") << Qt::endl;
        return;
    }
    RunConfig cfg;
    cfg.device    = ComputeDevice::Gpu;
    cfg.matrix    = Reference::randomMatrix(40, 200, 7);
    cfg.algorithm = Algorithm::SimpleXor;
    cfg.maxRows   = 6;

    // Как этот расчёт выглядит, когда пробы не было.
    clearCheckpoints();
    const Spectrum expected = runWorker(cfg);
    clearCheckpoints();

    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());
    worker.setGridTuningThreshold(0.0);

    int    spectraSent = 0;
    double measured    = -1.0;
    QObject::connect(&worker, &Worker::spectrumUpdated,
                     [&spectraSent](const SpectrumCounts&) { ++spectraSent; });
    QObject::connect(&worker, &Worker::updateRateMeasured,
                     [&measured](double perSecond) { measured = perSecond; });
    // В приложении пробу останавливает таймер интерфейса. Здесь цикла событий
    // нет, поэтому обрываем её сразу по сигналу о старте: замер выйдет грубым,
    // а проверяется тут не он, а следы.
    QObject::connect(&worker, &Worker::updateRateProbeStarted,
                     [&worker]() { worker.cancel(); });

    worker.setSettings(makeSettings(cfg));
    worker.measureUpdateRate();

    if (measured >= 0.0) { ++g_passed; g_out << "  ok       " << QStringLiteral("замер отдал результат") << Qt::endl; }
    else { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   сигнала с результатом не было") << Qt::endl; }

    if (spectraSent == 0) { ++g_passed; g_out << "  ok       " << QStringLiteral("спектр в интерфейс не уходил") << Qt::endl; }
    else { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   проба отправила спектров: %1").arg(spectraSent) << Qt::endl; }

    AutosaveRecord record;
    if (!testStore().load(cfg.matrix, cfg.algorithm, record)) {
        ++g_passed; g_out << "  ok       " << QStringLiteral("автосохранение не тронуто") << Qt::endl;
    } else {
        ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   проба записала автосохранение") << Qt::endl;
    }

    // И главное: после пробы обычный расчёт даёт тот же спектр, что и без неё.
    // Сравнение идёт с прогоном на чистом месте, а не с эталонной формулой:
    // проверяется здесь не правильность расчёта — на неё есть свои тесты, — а
    // то, что проба ничего за собой не оставила.
    const Spectrum after = runWorker(cfg);
    if (!expected.isEmpty() && after == expected) {
        ++g_passed; g_out << "  ok       " << QStringLiteral("расчёт после пробы даёт верный спектр") << Qt::endl;
    } else {
        ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   спектр после пробы разошёлся с эталоном") << Qt::endl;
    }
}

// ------------------------------------------------ Брауэр–Циммерман, прототип

struct BzCase
{
    QString     name;
    QStringList rows;
    int         r;
    int         sets;
};

static QVector<BzCase> bzCases()
{
    return {
        { QStringLiteral("Голей [24,12], как есть"),       Reference::golay24_12(),                              4, 2 },
        { QStringLiteral("случайный [40,16], перемешан"),  Bz::scramble(Bz::systematicRandom(16, 40, 7), 11),   4, 3 },
        { QStringLiteral("случайный [60,20], перемешан"),  Bz::scramble(Bz::systematicRandom(20, 60, 3), 5),    4, 3 },
        { QStringLiteral("случайный [96,24], перемешан"),  Bz::scramble(Bz::systematicRandom(24, 96, 9), 2),    5, 4 },
    };
}

// Печатает низ спектра тремя способами: точный, по Брауэру–Циммерману и так,
// как перебирает программа сейчас — комбинациями строк введённой матрицы.
//
// Запуск: SpectrumTests.exe --bz
static void bzReport()
{
    for (const BzCase& c : bzCases()) {
        const int k = c.rows.size();
        const int n = c.rows.first().length();

        const Reference::Spectrum exact = Reference::bruteForce(c.rows);
        const Bz::Result          bz    = Bz::run(c.rows, c.r, c.sets);
        const Reference::Spectrum naive = Bz::naivePartial(c.rows, c.r);

        QStringList overlaps;
        for (int o : bz.overlaps) overlaps << QString::number(o);

        g_out << Qt::endl
              << QStringLiteral("%1  —  [%2,%3], множеств %4 (перекрытия: %5), до %6 строк")
                     .arg(c.name).arg(n).arg(k).arg(bz.sets).arg(overlaps.join(QStringLiteral(", "))).arg(c.r)
              << Qt::endl
              << QStringLiteral("перебрано %1 слов вместо %2; гарантия: все слова веса < %3")
                     .arg(bz.enumerated).arg(1ULL << k).arg(bz.guaranteedBelow)
              << Qt::endl
              << QStringLiteral("   вес     точно        БЦ    как сейчас") << Qt::endl;

        int shown = 0;
        for (auto it = exact.cbegin(); it != exact.cend() && shown < bz.guaranteedBelow + 4; ++it, ++shown) {
            const int w = it.key();
            const quint64 e = it.value();
            const quint64 b = bz.spectrum.value(w, 0);
            const quint64 v = naive.value(w, 0);
            QString mark;
            if (w < bz.guaranteedBelow)
                mark = b == e ? QStringLiteral("  ✓") : QStringLiteral("  ПРОВАЛ");
            else
                mark = QStringLiteral("  (вне гарантии)");
            if (v != e)
                mark += QStringLiteral("   ← сейчас %1").arg(v < e ? QStringLiteral("недобор") : QStringLiteral("ПЕРЕБОР"));
            g_out << QStringLiteral("   %1  %2  %3  %4%5")
                         .arg(w, 3).arg(e, 10).arg(b, 8).arg(v, 10).arg(mark) << Qt::endl;
        }
    }
    g_out << Qt::endl;
}

// То же на матрице из файла, где точного спектра нет: сравниваются только
// Брауэр–Циммерман и нынешний перебор, и печатается сертифицированный низ.
//
// Запуск: SpectrumTests.exe --bz-file <файл матрицы> <строк> [<множеств>]
static int bzFile(const QString& path, int r, int maxSets)
{
    RunConfig cfg;
    if (!loadMatrixOrCase(path, cfg))
        return 2;
    const int k = cfg.matrix.size();
    const int n = cfg.matrix.first().length();

    g_out << QStringLiteral("[%1,%2], до %3 строк, множеств до %4").arg(n).arg(k).arg(r).arg(maxSets) << Qt::endl;
    g_out.flush();

    auto t = std::chrono::steady_clock::now();
    const Bz::Result bz = Bz::run(cfg.matrix, r, maxSets);
    const double bzSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();

    t = std::chrono::steady_clock::now();
    const Reference::Spectrum naive = Bz::naivePartial(cfg.matrix, r);
    const double naiveSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();

    QStringList overlaps;
    for (int o : bz.overlaps) overlaps << QString::number(o);

    g_out << QStringLiteral("множеств %1 (перекрытия: %2), перебрано %3 слов за %4 с; как сейчас — за %5 с")
                 .arg(bz.sets).arg(overlaps.join(QStringLiteral(", "))).arg(bz.enumerated)
                 .arg(bzSec, 0, 'f', 1).arg(naiveSec, 0, 'f', 1) << Qt::endl
          << QStringLiteral("гарантия: все слова веса < %1 найдены").arg(bz.guaranteedBelow) << Qt::endl;
    if (bz.rejectedOverlap >= 0)
        g_out << QStringLiteral("следующее множество перекрыло бы прежние на %1 столбцов — не взято")
                     .arg(bz.rejectedOverlap) << Qt::endl;
    g_out << QStringLiteral("   вес        БЦ    как сейчас") << Qt::endl;

    int minBz = -1, minNaive = -1;
    for (auto it = bz.spectrum.cbegin(); it != bz.spectrum.cend(); ++it)
        if (it.key() > 0 && minBz < 0) minBz = it.key();
    for (auto it = naive.cbegin(); it != naive.cend(); ++it)
        if (it.key() > 0 && minNaive < 0) minNaive = it.key();

    QSet<int> weights;
    for (auto it = bz.spectrum.cbegin(); it != bz.spectrum.cend(); ++it) weights.insert(it.key());
    for (auto it = naive.cbegin(); it != naive.cend(); ++it) weights.insert(it.key());
    QList<int> sorted = weights.values();
    std::sort(sorted.begin(), sorted.end());

    int shown = 0;
    for (int w : sorted) {
        if (w >= bz.guaranteedBelow + 6 && shown > 12) break;
        const QString mark = w < bz.guaranteedBelow ? QStringLiteral("  точно") : QStringLiteral("  (вне гарантии)");
        g_out << QStringLiteral("   %1  %2  %3%4")
                     .arg(w, 3).arg(bz.spectrum.value(w, 0), 8).arg(naive.value(w, 0), 10).arg(mark) << Qt::endl;
        ++shown;
    }
    g_out << QStringLiteral("минимальный найденный вес: БЦ %1, как сейчас %2").arg(minBz).arg(minNaive) << Qt::endl;
    return 0;
}

// Брауэр–Циммерман в самом расчёте: ниже гарантии спектр обязан совпасть с
// точным побитово, выше — не превышать его. Заодно проверяется правило
// единственности: при весе во всю длину кода перебирается всё, и каждое слово
// должно быть засчитано ровно один раз — спектр совпадает с точным целиком.
static Spectrum checkBzExact(const QString& name, const RunConfig& cfg, const Spectrum& exact)
{
    if (cfg.device == ComputeDevice::Gpu && !g_gpuAvailable) {
        g_out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
        return Spectrum();
    }
    clearCheckpoints();
    const Spectrum actual = runWorker(cfg);
    clearCheckpoints();

    bool ok = g_planExactUpTo >= cfg.bzWeight && !actual.isEmpty();
    QStringList problems;
    if (g_planExactUpTo < cfg.bzWeight)
        problems << QStringLiteral("план обещает точность до %1, просили %2")
                        .arg(g_planExactUpTo).arg(cfg.bzWeight);
    // Показывается только заказанное: до веса cfg.bzWeight — точно, выше
    // веса заказа — ничего, даже если план гарантирует больше.
    const int shownUpTo = std::min(g_planExactUpTo, cfg.bzWeight);
    for (auto it = exact.cbegin(); it != exact.cend(); ++it) {
        const quint64 found = actual.value(it.key(), 0);
        const bool bad = it.key() <= shownUpTo     ? found != it.value()
                       : it.key() <= cfg.bzWeight ? found > it.value()
                                                  : found != 0;
        if (bad) {
            ok = false;
            problems << QStringLiteral("вес %1: точно %2, получено %3")
                            .arg(it.key()).arg(it.value()).arg(found);
        }
    }
    for (auto it = actual.cbegin(); it != actual.cend(); ++it)
        if (!exact.contains(it.key())) {
            ok = false;
            problems << QStringLiteral("лишний вес %1").arg(it.key());
        }

    const QString what = QStringLiteral("%1: множеств %2, до %3 строк, точно до веса %4")
                             .arg(name).arg(g_planSets).arg(g_planRows).arg(g_planExactUpTo);
    if (ok) { ++g_passed; g_out << "  ok       " << what << Qt::endl; }
    else {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   ") << what << Qt::endl;
        for (const QString& line : problems)
            g_out << QStringLiteral("      ") << line << Qt::endl;
    }
    return actual;
}

// Два расчёта по одним множествам обязаны совпасть целиком — и ниже гарантии,
// и выше: найденное множество слов у них одно и то же.
static void expectSame(const QString& name, const Spectrum& a, const Spectrum& b)
{
    if (a.isEmpty() || b.isEmpty())
        return;   // один из расчётов пропущен или провалился — уже отмечено
    if (a == b) { ++g_passed; g_out << "  ok       " << name << Qt::endl; return; }
    ++g_failed;
    g_out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl
          << QStringLiteral("      CPU: ") << formatSpectrum(a) << Qt::endl
          << QStringLiteral("      GPU: ") << formatSpectrum(b) << Qt::endl;
}

// Досчёт до большего веса: запись после расчёта до w1 — начало расчёта до w2.
static void checkExtendBz(const QString& name, RunConfig cfg, int from, int to)
{
    if (cfg.device == ComputeDevice::Gpu && !g_gpuAvailable) {
        g_out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
        return;
    }
    RunConfig target = cfg;  target.bzWeight = to;
    clearCheckpoints();
    const Spectrum direct = runWorker(target);

    clearCheckpoints();
    RunConfig first = cfg;   first.bzWeight = from;
    runWorker(first);

    const qint64 saved = savedDoneOps();
    if (saved <= 0) {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   ") << name
              << QStringLiteral("  — после расчёта записи не осталось") << Qt::endl;
        clearCheckpoints();
        return;
    }
    const Spectrum extended = runWorker(target, LoadMode::FromCheckpoint);
    clearCheckpoints();

    if (extended == direct) {
        ++g_passed;
        g_out << "  ok       " << name
              << QStringLiteral("  (досчитано с ") << saved << QStringLiteral(" оп.)") << Qt::endl;
        return;
    }
    ++g_failed;
    g_out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
    g_out << QStringLiteral("      напрямую: ") << formatSpectrum(direct)   << Qt::endl;
    g_out << QStringLiteral("      досчётом: ") << formatSpectrum(extended) << Qt::endl;
}

static void testBrouwerZimmermannWorker()
{
    g_out << Qt::endl << QStringLiteral("Брауэр–Циммерман в расчёте: CPU и GPU, короткий и длинный пути") << Qt::endl;

    // Короткие коды с точным спектром: гарантия и полный перебор.
    for (const BzCase& c : bzCases()) {
        const Spectrum exact = Reference::bruteForce(c.rows);
        RunConfig cfg;
        cfg.matrix    = c.rows;
        cfg.algorithm = Algorithm::BrouwerZimmermann;
        cfg.bzWeight  = 6;

        cfg.device = ComputeDevice::Cpu;
        const Spectrum cpu = checkBzExact(c.name + QStringLiteral(" CPU, вес 6"), cfg, exact);
        cfg.device = ComputeDevice::Gpu;
        const Spectrum gpu = checkBzExact(c.name + QStringLiteral(" GPU, вес 6"), cfg, exact);
        expectSame(c.name + QStringLiteral(": CPU и GPU совпадают"), cpu, gpu);

        // Вес во всю длину: перебирается всё, спектр обязан совпасть целиком.
        cfg.bzWeight = c.rows.first().length();
        cfg.device   = ComputeDevice::Cpu;
        checkBzExact(c.name + QStringLiteral(" CPU, весь код"), cfg, exact);
    }

    // Длинный путь: k > 63. Точного спектра нет, поэтому CPU против GPU и
    // против прототипа ниже общей гарантии.
    {
        const QStringList rows = Bz::scramble(Bz::systematicRandom(66, 140, 21), 4);
        RunConfig cfg;
        cfg.matrix    = rows;
        cfg.algorithm = Algorithm::BrouwerZimmermann;
        cfg.bzWeight  = 5;

        cfg.device = ComputeDevice::Cpu;
        clearCheckpoints();
        const Spectrum cpu = runWorker(cfg);
        const int exactCpu = g_planExactUpTo, setsCpu = g_planSets, rowsCpu = g_planRows;
        clearCheckpoints();

        Spectrum gpu;
        if (g_gpuAvailable) {
            cfg.device = ComputeDevice::Gpu;
            gpu = runWorker(cfg);
            clearCheckpoints();
        }

        const QString what = QStringLiteral("[140,66] длинный путь: множеств %1, до %2 строк, точно до веса %3")
                                 .arg(setsCpu).arg(rowsCpu).arg(exactCpu);
        if (exactCpu >= cfg.bzWeight && !cpu.isEmpty()) { ++g_passed; g_out << "  ok       " << what << Qt::endl; }
        else { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   ") << what << Qt::endl; }

        if (g_gpuAvailable)
            expectSame(QStringLiteral("[140,66]: CPU и GPU совпадают"), cpu, gpu);
        else
            g_out << QStringLiteral("  ПРОПУСК  [140,66] GPU  (GPU недоступен)") << Qt::endl;

        // Прототип ищет множества сам, и выше гарантии его находки другие.
        // Ниже — обязан совпасть.
        const Bz::Result proto = Bz::run(rows, 2, 3);
        const int common = std::min(proto.guaranteedBelow, exactCpu + 1);
        bool ok = true;
        for (int w = 0; w < common; ++w)
            if (cpu.value(w, 0) != proto.spectrum.value(w, 0)) {
                ok = false;
                g_out << QStringLiteral("      вес %1: расчёт %2, прототип %3")
                             .arg(w).arg(cpu.value(w, 0)).arg(proto.spectrum.value(w, 0)) << Qt::endl;
            }
        const QString vs = QStringLiteral("[140,66]: совпадает с прототипом ниже веса %1").arg(common);
        if (ok) { ++g_passed; g_out << "  ok       " << vs << Qt::endl; }
        else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   ") << vs << Qt::endl; }
    }

    // Возобновление и досчёт: слои с несколькими множествами и сквозной
    // нумерацией внутри слоя.
    g_out << Qt::endl << QStringLiteral("Брауэр–Циммерман: сохранение и досчёт") << Qt::endl;
    {
        // Слои маленькие — каждый слой каждого множества уходит одним чанком,
        // и обрыв приходится ровно на границу множества: 903 операции, стоп
        // после 300 — это середина слоя из двух строк.
        RunConfig cfg;
        cfg.matrix    = Bz::scramble(Bz::systematicRandom(24, 96, 9), 2);
        cfg.algorithm = Algorithm::BrouwerZimmermann;
        cfg.bzWeight  = 8;
        cfg.device    = ComputeDevice::Cpu;
        checkResume(QStringLiteral("CPU [96,24], вес 8, обрыв на границе множества"), cfg, cfg, 100, 300);
        RunConfig gpuCfg = cfg; gpuCfg.device = ComputeDevice::Gpu;
        checkResume(QStringLiteral("GPU [96,24], вес 8, обрыв на границе множества"), gpuCfg, gpuCfg, 100, 300);
        checkResume(QStringLiteral("CPU -> GPU [96,24], перенос записи"), cfg, gpuCfg, 100, 300);
        checkExtendBz(QStringLiteral("CPU [96,24]: вес 5, потом 8"), cfg, 5, 8);
        checkExtendBz(QStringLiteral("GPU [96,24]: вес 5, потом 8"), gpuCfg, 5, 8);

        // Слой из пяти строк на 48 — 1,7 млн комбинаций, больше чанка в
        // миллион: обрыв попадает внутрь множества.
        RunConfig mid;
        mid.matrix    = Bz::scramble(Bz::systematicRandom(48, 96, 33), 6);
        mid.algorithm = Algorithm::BrouwerZimmermann;
        mid.bzWeight  = 11;
        mid.device    = ComputeDevice::Cpu;
        RunConfig midGpu = mid; midGpu.device = ComputeDevice::Gpu;
        checkResume(QStringLiteral("CPU [96,48], вес 11, обрыв внутри множества"), mid, mid, 1000000, 2500000);
        checkResume(QStringLiteral("GPU [96,48], вес 11, обрыв внутри множества"), midGpu, midGpu, 1000000, 2500000);

        // Длинный путь: слой из пяти строк на 66 — 9,7 млн масок на множество,
        // чанк GPU при 8x32 нитях — миллион.
        RunConfig lng;
        lng.matrix     = Bz::scramble(Bz::systematicRandom(66, 140, 21), 4);
        lng.algorithm  = Algorithm::BrouwerZimmermann;
        lng.bzWeight   = 9;
        lng.device     = ComputeDevice::Cpu;
        lng.blocksGpu  = 8;
        lng.threadsGpu = 32;
        RunConfig lngGpu = lng; lngGpu.device = ComputeDevice::Gpu;
        checkResume(QStringLiteral("CPU [140,66] длинный, обрыв внутри множества"), lng, lng, 3000000, 8000000);
        checkResume(QStringLiteral("GPU [140,66] длинный, обрыв внутри множества"), lngGpu, lngGpu, 3000000, 8000000);
        lng.bzWeight = 5;
        checkExtendBz(QStringLiteral("CPU [140,66] длинный: вес 4, потом 5"), lng, 4, 5);
    }
}

// Последний слой Брауэра–Циммермана — только по тем множествам, что нужны
// для гарантии (см. infosets.h). Проверяется трижды: план — против перебора
// всех глубин, спектр расчёта — против полного перебора на каждом весе, где
// слой вышел неполным, и досчёт — с неполного слоя на полный.
static void testBzPartialLayer()
{
    g_out << Qt::endl << QStringLiteral("Брауэр–Циммерман: неполный последний слой") << Qt::endl;

    // 1. План. Наименьшая глубина в порядке (слой, множеств в нём) с
    //    гарантией выше веса; у предыдущей по этому порядку — не выше.
    {
        Mixing::Xorshift64 rng{ 0x5EEDBEEFULL };
        int bad = 0, partial = 0, checked = 0;
        for (int trial = 0; trial < 300; ++trial) {
            const int rows = 4 + int(rng.next() % 40);
            const int cols = rows + 1 + int(rng.next() % (4 * rows));
            const int sets = 1 + int(rng.next() % 6);
            std::vector<int> overlaps(size_t(sets), 0);
            for (int j = 1; j < sets; ++j)
                overlaps[size_t(j)] = std::min(rows, overlaps[size_t(j - 1)] + int(rng.next() % 4));
            for (int weight = 0; weight <= cols; ++weight) {
                const InfoSets::Depth d = InfoSets::depthForWeight(overlaps, weight, rows, cols);
                ++checked;
                if (d.lastLayerSets < sets && d.maxRows < rows)
                    ++partial;
                const int bound = InfoSets::guaranteedBelow(overlaps, d, rows, cols);
                // Предыдущая глубина: на одно множество меньше в последнем слое
                // или, если оно одно, — полный предыдущий слой.
                InfoSets::Depth prev = d;
                if (d.lastLayerSets > 1) prev.lastLayerSets = d.lastLayerSets - 1;
                else                     prev = InfoSets::Depth{ d.maxRows - 1, sets };
                const bool prevOk = d.maxRows == 0 && d.lastLayerSets == 1
                                    ? true
                                    : InfoSets::guaranteedBelow(overlaps, prev, rows, cols) <= weight;
                const bool ok = (bound > weight || d.maxRows >= rows) && prevOk;
                if (!ok && ++bad <= 5)
                    g_out << QStringLiteral("      k=%1 n=%2 вес %3: глубина (%4, %5), гарантия %6")
                                 .arg(rows).arg(cols).arg(weight).arg(d.maxRows).arg(d.lastLayerSets).arg(bound)
                          << Qt::endl;
            }
        }
        const QString what = QStringLiteral("план: %1 весов, из них с неполным слоем %2").arg(checked).arg(partial);
        if (bad == 0 && partial > 0) { ++g_passed; g_out << "  ok       " << what << Qt::endl; }
        else { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   ") << what << Qt::endl; }
    }

    // 2. Спектр. Каждый вес, на котором план расчёта вышел с неполным
    //    последним слоем, сверяется с полным перебором.
    struct Case { QString name; QStringList rows; };
    const QVector<Case> cases = {
        { QStringLiteral("Голей [24,12]"),      Reference::golay24_12() },
        { QStringLiteral("случайный [40,16]"),  Bz::scramble(Bz::systematicRandom(16, 40, 7), 11) },
        { QStringLiteral("случайный [60,20]"),  Bz::scramble(Bz::systematicRandom(20, 60, 3), 5) },
        { QStringLiteral("случайный [70,14]"),  Bz::scramble(Bz::systematicRandom(14, 70, 17), 3) },
        { QStringLiteral("случайный [96,24]"),  Bz::scramble(Bz::systematicRandom(24, 96, 9), 2) },
        { QStringLiteral("случайный [33,18]"),  Bz::scramble(Bz::systematicRandom(18, 33, 23), 8) },
    };
    int partialRuns = 0;
    for (const Case& c : cases) {
        const Spectrum exact = Reference::bruteForce(c.rows);
        const int rows = c.rows.size();
        const int cols = c.rows.first().length();
        int words = 0;
        const std::vector<quint64> packed = InfoSets::packRows(c.rows, words);
        const int fit = std::max(1, Constants::MAX_CONST_WORDS / std::max(1, rows * words));
        const std::vector<InfoSets::InfoSet> found =
            InfoSets::find(packed.data(), rows, cols, words, std::min(Constants::MAX_INFO_SETS, fit));
        std::vector<int> overlaps;
        for (const InfoSets::InfoSet& set : found) overlaps.push_back(set.overlap);

        for (int weight = 1; weight <= cols / 2; ++weight) {
            std::vector<int> used = overlaps;
            used.resize(size_t(InfoSets::setsForWeight(overlaps, weight, rows, cols)));
            const InfoSets::Depth d = InfoSets::depthForWeight(used, weight, rows, cols);
            if (d.lastLayerSets >= int(used.size()) || d.maxRows >= rows)
                continue;
            ++partialRuns;
            RunConfig cfg;
            cfg.matrix    = c.rows;
            cfg.algorithm = Algorithm::BrouwerZimmermann;
            cfg.bzWeight  = weight;
            cfg.device    = ComputeDevice::Cpu;
            const QString name = QStringLiteral("%1, вес %2: слой %3 по %4 из %5 множеств")
                                     .arg(c.name).arg(weight).arg(d.maxRows).arg(d.lastLayerSets).arg(used.size());
            const Spectrum cpu = checkBzExact(name + QStringLiteral(", CPU"), cfg, exact);
            cfg.device = ComputeDevice::Gpu;
            const Spectrum gpu = checkBzExact(name + QStringLiteral(", GPU"), cfg, exact);
            if (g_gpuAvailable)
                expectSame(name + QStringLiteral(": CPU и GPU совпадают"), cpu, gpu);
        }
    }
    {
        const QString what = QStringLiteral("прогонов с неполным слоем: %1").arg(partialRuns);
        if (partialRuns >= 10) { ++g_passed; g_out << "  ok       " << what << Qt::endl; }
        else { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   ") << what << Qt::endl; }
    }

    // 3. Досчёт и обрыв. С неполного слоя — на тот же слой по большему числу
    //    множеств и на следующий слой; обрыв посреди неполного слоя.
    {
        RunConfig cfg;
        cfg.matrix    = Bz::scramble(Bz::systematicRandom(24, 96, 9), 2);
        cfg.algorithm = Algorithm::BrouwerZimmermann;
        cfg.device    = ComputeDevice::Cpu;
        RunConfig gpuCfg = cfg; gpuCfg.device = ComputeDevice::Gpu;

        // Веса, на которых слой выходит неполным, и следующий за каждым.
        int words = 0;
        const std::vector<quint64> packed = InfoSets::packRows(cfg.matrix, words);
        const std::vector<InfoSets::InfoSet> found = InfoSets::find(packed.data(), 24, 96, words, 8);
        std::vector<int> overlaps;
        for (const InfoSets::InfoSet& set : found) overlaps.push_back(set.overlap);
        int from = -1;
        for (int w = 4; w < 20 && from < 0; ++w) {
            std::vector<int> used = overlaps;
            used.resize(size_t(InfoSets::setsForWeight(overlaps, w, 24, 96)));
            std::vector<int> usedNext = overlaps;
            usedNext.resize(size_t(InfoSets::setsForWeight(overlaps, w + 1, 24, 96)));
            const InfoSets::Depth d = InfoSets::depthForWeight(used, w, 24, 96);
            // Досчёт возможен только по тем же множествам.
            if (used.size() == usedNext.size() && d.lastLayerSets < int(used.size()))
                from = w;
        }
        if (from < 0) {
            ++g_failed;
            g_out << QStringLiteral("  ПРОВАЛ   нет веса с неполным слоем для проверки досчёта") << Qt::endl;
        }
        else {
            checkExtendBz(QStringLiteral("CPU [96,24]: вес %1 (неполный слой), потом %2").arg(from).arg(from + 1),
                          cfg, from, from + 1);
            checkExtendBz(QStringLiteral("CPU [96,24]: вес %1, потом %2").arg(from).arg(from + 4), cfg, from, from + 4);
            checkExtendBz(QStringLiteral("GPU [96,24]: вес %1, потом %2").arg(from).arg(from + 1), gpuCfg, from, from + 1);
        }
    }
    {
        // Обрыв внутри неполного слоя: слой должен быть больше чанка, а чанк
        // CPU — миллион комбинаций. У [96,48] слой из пяти строк — 1,7 млн.
        RunConfig cfg;
        cfg.matrix    = Bz::scramble(Bz::systematicRandom(48, 96, 33), 6);
        cfg.algorithm = Algorithm::BrouwerZimmermann;
        cfg.device    = ComputeDevice::Cpu;

        int words = 0;
        const std::vector<quint64> packed = InfoSets::packRows(cfg.matrix, words);
        const int fit = std::max(1, Constants::MAX_CONST_WORDS / std::max(1, 48 * words));
        const std::vector<InfoSets::InfoSet> found =
            InfoSets::find(packed.data(), 48, 96, words, std::min(Constants::MAX_INFO_SETS, fit));
        std::vector<int> overlaps;
        for (const InfoSets::InfoSet& set : found) overlaps.push_back(set.overlap);
        for (int w = 6; w < 16; ++w) {
            std::vector<int> used = overlaps;
            used.resize(size_t(InfoSets::setsForWeight(overlaps, w, 48, 96)));
            const InfoSets::Depth d = InfoSets::depthForWeight(used, w, 48, 96);
            const quint64 top = Reference::binom(48, quint64(d.maxRows));
            if (d.lastLayerSets >= int(used.size()) || top * quint64(d.lastLayerSets) < 3000000)
                continue;
            cfg.bzWeight = w;
            const quint64 total = expectedTotalOps(cfg);
            const quint64 stop  = total - top * quint64(d.lastLayerSets) / 2;
            checkResume(QStringLiteral("CPU [96,48], вес %1: обрыв в неполном слое (%2 из %3 множеств)")
                            .arg(w).arg(d.lastLayerSets).arg(used.size()),
                        cfg, cfg, 1000000, stop);
            RunConfig gpuCfg = cfg; gpuCfg.device = ComputeDevice::Gpu;
            checkResume(QStringLiteral("GPU [96,48], вес %1: обрыв в неполном слое").arg(w),
                        gpuCfg, gpuCfg, 1000000, stop);
            break;
        }
    }
}

// Расчёт по Брауэру–Циммерману на матрице из файла: план, время, спектр.
//
// Запуск: SpectrumTests.exe --bz-run <файл матрицы> <вес> [cpu|gpu]
static int bzRun(const QString& path, int weight, const QString& device)
{
    RunConfig cfg;
    if (!loadMatrixOrCase(path, cfg))
        return 2;
    cfg.algorithm = Algorithm::BrouwerZimmermann;
    cfg.bzWeight  = weight;
    cfg.device    = device == QStringLiteral("cpu") ? ComputeDevice::Cpu : ComputeDevice::Gpu;
    cfg.threadsCpu = omp_get_num_procs();
    cfg.autoTune   = cfg.device == ComputeDevice::Gpu;

    const int k = cfg.matrix.size();
    const int n = cfg.matrix.first().length();
    g_out << QStringLiteral("[%1,%2], точно до веса %3, %4")
                 .arg(n).arg(k).arg(weight)
                 .arg(cfg.device == ComputeDevice::Cpu ? QStringLiteral("CPU") : QStringLiteral("GPU")) << Qt::endl;
    g_out.flush();

    clearCheckpoints();
    const auto t = std::chrono::steady_clock::now();
    const Spectrum spectrum = runWorker(cfg);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    clearCheckpoints();

    g_out << QStringLiteral("множеств %1, до %2 строк, точно до веса %3; %4 с")
                 .arg(g_planSets).arg(g_planRows).arg(g_planExactUpTo).arg(sec, 0, 'f', 1) << Qt::endl;
    int shown = 0;
    for (auto it = spectrum.cbegin(); it != spectrum.cend() && shown < 16; ++it, ++shown)
        g_out << QStringLiteral("   %1  %2%3").arg(it.key(), 3).arg(it.value(), 12)
                     .arg(it.key() <= g_planExactUpTo ? QStringLiteral("  точно") : QStringLiteral("  (неполно)"))
              << Qt::endl;
    return 0;
}

// ------------------------------------------------ случайный поиск (Леон)

static void expectLeon(const QString& name, bool ok, const QString& detail = QString())
{
    if (ok) { ++g_passed; g_out << "  ok       " << name << Qt::endl; }
    else {
        ++g_failed;
        g_out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
        if (!detail.isEmpty()) g_out << QStringLiteral("      ") << detail << Qt::endl;
    }
}

// Модель поимки: вероятности в пределах, монотонны по глубине и по весу,
// план выбирает глубину от одной до четырёх строк.
static void testLeonModel()
{
    g_out << Qt::endl << QStringLiteral("Случайный поиск: модель поимки") << Qt::endl;

    const int n = 336, k = 96;
    bool inRange = true, byRows = true, byWeight = true;
    for (int w = 1; w <= n; w += 5) {
        double prev = 0.0;
        for (int r = 1; r <= 4; ++r) {
            const double p = Leon::catchProbability(n, k, w, r);
            if (p < 0.0 || p > 1.0) inRange = false;
            if (p + 1e-12 < prev) byRows = false;
            prev = p;
        }
    }
    for (int w = 12; w < n; w += 3)
        if (Leon::catchProbability(n, k, w + 3, 3) > Leon::catchProbability(n, k, w, 3) + 1e-12)
            byWeight = false;
    expectLeon(QStringLiteral("вероятности в [0, 1]"), inRange);
    expectLeon(QStringLiteral("глубже перебор — выше поимка"), byRows);
    expectLeon(QStringLiteral("тяжелее слово — ниже поимка"), byWeight);

    // Слово веса 24 на [336,96]: около 5 % на трёх строках — как в таблице.
    const double p24 = Leon::catchProbability(n, k, 24, 3);
    expectLeon(QStringLiteral("вес 24, три строки: %1").arg(p24, 0, 'f', 4),
               p24 > 0.045 && p24 < 0.056);

    // С учётом цены Гаусса выгодны две строки: одна — слишком много попыток,
    // три — слишком дорогая каждая. Без профиля ключей окна не бывает.
    const Leon::Plan plan = Leon::plan(n, k, 42, 1e-9);
    expectLeon(QStringLiteral("план для веса 42: %1 строк, %2 попыток, без окна")
                   .arg(plan.rows).arg(plan.trials),
               plan.rows == 2 && plan.window == 0 && plan.trials > 100000 && plan.trials < 1000000);

    // Профиль ключей окна Штерна–Дюмера. У случайной [1000,500] ключи
    // равномерны: пар столько, сколько даёт L₁·L₂/2^l, и на такой длине
    // план берёт окно — по модели оно втрое и более дешевле перебора. У
    // разреженной (по пять единиц в строке) многие строки на окне нулевые,
    // пар на порядки больше. На видеокарте окна нет.
    {
        // Не Reference::randomMatrix: та берёт младший бит xorshift, а он —
        // линейная рекуррента порядка 64, и у проверочной части такой
        // «случайной» матрицы ранг не выше 64 — ключи скошены.
        std::mt19937_64 rng(11);
        QStringList dense;
        for (int r = 0; r < 500; ++r) {
            QString row(1000, QLatin1Char('0'));
            for (int c = 0; c < 1000; ++c)
                if (rng() & 1ULL) row[c] = QLatin1Char('1');
            dense << row;
        }
        const Leon::SternProfile dp = Leon::sternProfile(dense);
        const double uniform2 = Leon::sternListSize(250, 2) * Leon::sternListSize(250, 2) / std::ldexp(1.0, 20);
        expectLeon(QStringLiteral("профиль случайной [1000,500]: окно %1, пар при p=2 l=20 %2 (у равномерных ключей %3)")
                       .arg(dp.window).arg(dp.pairs[2][20], 0, 'g', 4).arg(uniform2, 0, 'g', 4),
                   dp.window == Leon::MAX_STERN_WINDOW && dp.pairs[2][20] > 0.5 * uniform2 && dp.pairs[2][20] < 2.0 * uniform2);
        const Leon::Plan wide = Leon::plan(1000, 500, 40, 1e-9, false, &dp);
        const Leon::Plan widePlain = Leon::plan(1000, 500, 40, 1e-9, false, &dp, Leon::WindowPolicy::none());
        expectLeon(QStringLiteral("план [1000,500] для веса 40: p=%1 l=%2, %3 попыток по %4 слов (без окна p=%5, %6 попыток по %7)")
                       .arg(wide.rows).arg(wide.window).arg(wide.trials).arg(wide.costPerTrial, 0, 'g', 3)
                       .arg(widePlain.rows).arg(widePlain.trials).arg(widePlain.costPerTrial, 0, 'g', 3),
                   wide.window > 0 && wide.rows == 2
                       && 3.0 * double(wide.trials) * wide.costPerTrial <= double(widePlain.trials) * widePlain.costPerTrial);
        expectLeon(QStringLiteral("план для GPU без окна"), Leon::plan(1000, 500, 40, 1e-9, true, &dp).window == 0);

        rng.seed(5);
        QStringList sparse;
        for (int r = 0; r < 500; ++r) {
            QString row(1000, QLatin1Char('0'));
            for (int i = 0; i < 5; ++i) row[int(rng() % 1000)] = QLatin1Char('1');
            sparse << row;
        }
        const Leon::SternProfile sp = Leon::sternProfile(sparse);
        const double uniform1 = Leon::sternListSize(250, 1) * Leon::sternListSize(250, 1) / std::ldexp(1.0, 20);
        expectLeon(QStringLiteral("профиль разреженной [1000,500]: пар при p=1 l=20 %1 (у равномерных ключей %2)")
                       .arg(sp.pairs[1][20], 0, 'g', 4).arg(uniform1, 0, 'g', 4),
                   sp.window == Leon::MAX_STERN_WINDOW && sp.pairs[1][20] > 20.0 * uniform1);
    }

    // Таблица: слово помнится один раз, поимки считаются, оценка Чао по f1/f2.
    Leon::WordTable table(1, 8);
    const quint64 a = 0x0FULL, b = 0xF0ULL, c = 0x33ULL;
    bool fresh = table.add(&a, 4) && table.add(&b, 4) && table.add(&c, 4);
    bool repeat = !table.add(&a, 4) && !table.add(&b, 4) && !table.add(&b, 4);
    const std::vector<double> unseen = table.unseenByWeight();
    // a поймано дважды, b трижды, c один раз: f1 = 1, f2 = 1 -> 0.5
    expectLeon(QStringLiteral("таблица слов: %1 слов, Чао %2").arg(table.size()).arg(unseen[4]),
               fresh && repeat && table.size() == 3 && std::abs(unseen[4] - 0.5) < 1e-9);

    // Таблица по частям даёт то же, что и цельная: пачкой и по одному.
    {
        Leon::WordTable      whole(1, 8);
        Leon::ShardedWordTable parts(1, 8, 4);
        std::vector<quint64> batch;
        for (quint64 i = 1; i <= 2000; ++i) {
            const quint64 w = (i % 200) + 1;   // повторы: 200 разных слов, вес не выше 8
            batch.push_back(w);
        }
        for (quint64 w : batch) {
            int weight = 0; quint64 v = w; while (v) { weight += int(v & 1); v >>= 1; }
            whole.add(&w, weight);
            if (w % 2 == 0) parts.add(&w, weight);
        }
        std::vector<quint64> odd;
        for (quint64 w : batch) if (w % 2 == 1) odd.push_back(w);
        parts.addBatch(odd.data(), odd.size());
        const bool same = whole.size() == parts.size()
                       && whole.countByWeight() == parts.countByWeight()
                       && whole.unseenByWeight() == parts.unseenByWeight();
        expectLeon(QStringLiteral("таблица по частям: %1 слов, как цельная").arg(parts.size()), same);
    }
}

// Поиск на кодах с известным спектром: до заданного веса он обязан совпасть
// с точным. Вероятность несовпадения задана степенью 10^-12 — тест
// детерминирован по затравке, так что либо проходит всегда, либо никогда.
static void testLeonWorker()
{
    g_out << Qt::endl << QStringLiteral("Случайный поиск в расчёте") << Qt::endl;

    struct Case { QString name; QStringList rows; int weight; };
    const QVector<Case> cases = {
        { QStringLiteral("Голей [24,12]"),           Reference::golay24_12(),                             12 },
        { QStringLiteral("случайный [40,16]"),       Bz::scramble(Bz::systematicRandom(16, 40, 7), 11),  12 },
        { QStringLiteral("случайный [60,20]"),       Bz::scramble(Bz::systematicRandom(20, 60, 3), 5),   15 },
        { QStringLiteral("случайный [96,24]"),       Bz::scramble(Bz::systematicRandom(24, 96, 9), 2),   20 },
    };
    auto checkDevice = [&](const Case& c, const Spectrum& exact, ComputeDevice device) {
        const QString who = device == ComputeDevice::Cpu ? QStringLiteral("CPU") : QStringLiteral("GPU");
        if (device == ComputeDevice::Gpu && !g_gpuAvailable) {
            g_out << QStringLiteral("  ПРОПУСК  ") << c.name << QStringLiteral(" GPU  (GPU недоступен)") << Qt::endl;
            return;
        }
        RunConfig cfg;
        cfg.matrix     = c.rows;
        cfg.algorithm  = Algorithm::RandomInfoSets;
        cfg.leonWeight = c.weight;
        cfg.device     = device;
        clearCheckpoints();
        const Spectrum found = runWorker(cfg);
        const quint64 trials = g_searchTotal, done = g_searchDone;
        const double  miss   = g_searchMiss;
        const Spectrum again = runWorker(cfg);
        clearCheckpoints();

        bool ok = !found.isEmpty() && done == trials;
        QStringList problems;
        for (auto it = exact.cbegin(); it != exact.cend(); ++it) {
            if (it.key() > c.weight) {
                if (found.contains(it.key())) { ok = false; problems << QStringLiteral("вес %1 тяжелее предела, но найден").arg(it.key()); }
                continue;
            }
            if (found.value(it.key(), 0) != it.value()) {
                ok = false;
                problems << QStringLiteral("вес %1: точно %2, найдено %3").arg(it.key()).arg(it.value()).arg(found.value(it.key(), 0));
            }
        }
        for (auto it = found.cbegin(); it != found.cend(); ++it)
            if (found.value(it.key()) > exact.value(it.key(), 0)) { ok = false; problems << QStringLiteral("вес %1: лишние слова").arg(it.key()); }
        if (again != found) { ok = false; problems << QStringLiteral("повтор дал другой спектр"); }

        expectLeon(QStringLiteral("%1 %2, до веса %3: попыток %4, пропуск ~%5")
                       .arg(c.name).arg(who).arg(c.weight).arg(trials).arg(miss, 0, 'g', 2),
                   ok, problems.join(QStringLiteral("; ")));
    };

    for (const Case& c : cases) {
        const Spectrum exact = Reference::bruteForce(c.rows);
        checkDevice(c, exact, ComputeDevice::Cpu);
        checkDevice(c, exact, ComputeDevice::Gpu);
    }

    // Длинный код: точного спектра нет, сверяемся с Брауэром–Циммерманом,
    // который до веса 9 точен.
    {
        const QStringList rows = Bz::scramble(Bz::systematicRandom(66, 140, 21), 4);
        RunConfig bz;
        bz.matrix    = rows;
        bz.algorithm = Algorithm::BrouwerZimmermann;
        bz.bzWeight  = 8;
        bz.device    = ComputeDevice::Cpu;
        clearCheckpoints();
        const Spectrum exact = runWorker(bz);
        const int exactUpTo = g_planExactUpTo;

        for (ComputeDevice device : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
            const bool gpu = device == ComputeDevice::Gpu;
            if (gpu && !g_gpuAvailable) {
                g_out << QStringLiteral("  ПРОПУСК  [140,66] длинный GPU  (GPU недоступен)") << Qt::endl;
                continue;
            }
            RunConfig leon;
            leon.matrix     = rows;
            leon.algorithm  = Algorithm::RandomInfoSets;
            leon.leonWeight = 8;
            leon.device     = device;
            clearCheckpoints();
            const Spectrum found = runWorker(leon);
            clearCheckpoints();

            bool ok = exactUpTo >= 8 && !found.isEmpty();
            QStringList problems;
            for (int w = 1; w <= 8; ++w)
                if (found.value(w, 0) != exact.value(w, 0)) {
                    ok = false;
                    problems << QStringLiteral("вес %1: БЦ %2, поиск %3").arg(w).arg(exact.value(w, 0)).arg(found.value(w, 0));
                }
            const Leon::Plan plan = Leon::plan(140, 66, 8, 1e-12, gpu);
            expectLeon(QStringLiteral("[140,66] длинный %1, до веса 8: попыток %2, %3 строк за попытку")
                           .arg(gpu ? QStringLiteral("GPU") : QStringLiteral("CPU"))
                           .arg(g_searchTotal).arg(plan.rows), ok, problems.join(QStringLiteral("; ")));
        }
    }

    // Плотный код: слов до веса 9 почти семь миллионов, первая пачка
    // переполняет выходной буфер. С таблицей виденных слов на видеокарте
    // отброшенная пачка теряла слова (они уже сидели в таблице и при
    // повторе не выкладывались): Макс получил 6 слов веса 6 вместо 18270,
    // и от запуска к запуску по-разному. Эталон — дуальный перебор.
    if (g_gpuAvailable) {
        const QStringList rows = Bz::systematicRandom(51, 63, 63);
        RunConfig dual;
        dual.matrix    = rows;
        dual.algorithm = Algorithm::DualCode;
        dual.device    = ComputeDevice::Cpu;
        clearCheckpoints();
        const Spectrum exact = runWorker(dual);

        RunConfig leon;
        leon.matrix     = rows;
        leon.algorithm  = Algorithm::RandomInfoSets;
        leon.leonWeight = 9;
        leon.device     = ComputeDevice::Gpu;
        clearCheckpoints();
        const Spectrum found = runWorker(leon);
        const Spectrum again = runWorker(leon);
        clearCheckpoints();

        bool ok = !found.isEmpty() && !exact.isEmpty();
        QStringList problems;
        for (int w = 1; w <= 9; ++w)
            if (found.value(w, 0) != exact.value(w, 0)) {
                ok = false;
                problems << QStringLiteral("вес %1: точно %2, найдено %3").arg(w).arg(exact.value(w, 0)).arg(found.value(w, 0));
            }
        if (again != found) { ok = false; problems << QStringLiteral("повтор дал другой спектр"); }
        expectLeon(QStringLiteral("[63,51] плотный GPU, до веса 9: буфер переполняется, слова целы"),
                   ok, problems.join(QStringLiteral("; ")));
    }

    // Подбор сетки включён, устройство GPU: подбор обязан промолчать (у
    // поиска своё ядро), а итог — попасть в запись автосохранения. Раньше
    // подбор лез в пустую таблицу биномов, а финал затирал спектр нулями из
    // d_spectrum, который поиск не трогает.
    if (g_gpuAvailable) {
        RunConfig cfg;
        cfg.matrix     = Reference::golay24_12();
        cfg.algorithm  = Algorithm::RandomInfoSets;
        cfg.leonWeight = 12;
        cfg.device     = ComputeDevice::Gpu;
        cfg.autoTune   = true;
        clearCheckpoints();
        const Spectrum found = runWorker(cfg);
        const QVector<AutosaveEntry> entries = testStore().list();
        Spectrum saved;
        if (!entries.isEmpty())
            for (int w = 0; w < entries.first().record.state.spectrum.size(); ++w)
                if (entries.first().record.state.spectrum.at(w) != 0)
                    saved[w] = entries.first().record.state.spectrum.at(w);
        clearCheckpoints();
        Spectrum expected;
        const Spectrum analytic = Reference::analyticGolay24_12();
        for (auto it = analytic.cbegin(); it != analytic.cend(); ++it)
            if (it.key() <= cfg.leonWeight)
                expected[it.key()] = it.value();
        expectLeon(QStringLiteral("GPU с подбором сетки: спектр найден и записан"),
                   !found.isEmpty() && found == expected
                       && !entries.isEmpty() && stripZeros(saved) == found,
                   QStringLiteral("найдено: %1\n      записано: %2")
                       .arg(formatSpectrum(found), formatSpectrum(saved)));
    }

    // Одинаковые номера попыток дают одинаковые множества на обоих
    // устройствах: при одной глубине перебора спектры обязаны совпасть
    // побитово — не «оба точны», а именно совпасть, включая веса выше
    // предела точности. Три матрицы: в разделяемой памяти, в глобальной
    // из-за длины строки (11 слов) и в глобальной из-за числа строк.
    if (g_gpuAvailable) {
        // Разреженный код: единичная матрица плюс несколько единиц в
        // проверочной части. У случайного кода такой длины лёгких слов нет
        // вовсе, а тяжёлых — миллиарды; здесь слова веса до 8 есть, их
        // сотни, и они ловятся.
        auto sparseCode = [](int rows, int cols, int onesPerRow, quint64 seed) {
            quint64 state = seed | 1ULL;
            auto next = [&state]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
            QStringList out;
            for (int r = 0; r < rows; ++r) {
                QString row(cols, QLatin1Char('0'));
                row[r] = QLatin1Char('1');
                for (int i = 0; i < onesPerRow; ++i)
                    row[rows + int(next() % quint64(cols - rows))] = QLatin1Char('1');
                out << row;
            }
            return Bz::scramble(out, seed + 1);
        };
        struct Twin { QString name; QStringList rows; int weight; };
        const QVector<Twin> twins = {
            { QStringLiteral("[90,30] в разделяемой"),      Bz::scramble(Bz::systematicRandom(30, 90, 17), 3), 30 },
            { QStringLiteral("[700,40] строка в 11 слов"),  sparseCode(40, 700, 5, 23),                         12 },
            { QStringLiteral("[600,300] строка в 10 слов"), sparseCode(300, 600, 5, 29),                         8 },
        };
        // Окно Штерна–Дюмера есть только у процессора — для сравнения слово
        // в слово оно выключается.
        for (const Twin& t : twins) {
            const int n = t.rows.first().length(), k = t.rows.size();
            const int tier = leonSharedTier(k, n, (n + 63) / 64, 0);
            // Глубина у устройств может разойтись из-за разной цены Гаусса;
            // сравнивать имеет смысл только при одинаковой.
            const Leon::Plan cpuPlan = Leon::plan(n, k, t.weight, 1e-12, false);
            const Leon::Plan gpuPlan = Leon::plan(n, k, t.weight, 1e-12, true);
            if (cpuPlan.rows != gpuPlan.rows) {
                g_out << QStringLiteral("  ПРОПУСК  %1 CPU и GPU слово в слово: разная глубина (%2 и %3)")
                             .arg(t.name).arg(cpuPlan.rows).arg(gpuPlan.rows) << Qt::endl;
                continue;
            }
            RunConfig cfg;
            cfg.matrix     = t.rows;
            cfg.algorithm  = Algorithm::RandomInfoSets;
            cfg.leonWeight = t.weight;
            cfg.device     = ComputeDevice::Cpu;
            cfg.window     = Leon::WindowPolicy::none();
            clearCheckpoints();
            const Spectrum cpu = runWorker(cfg);
            const quint64 cpuTrials = g_searchDone;
            cfg.device = ComputeDevice::Gpu;
            clearCheckpoints();
            const Spectrum gpu = runWorker(cfg);
            const quint64 gpuTrials = g_searchDone;
            clearCheckpoints();
            expectLeon(QStringLiteral("%1 (%2) CPU и GPU слово в слово: попыток %3 и %4, весов %5")
                           .arg(t.name)
                           .arg(tier == 0 ? QStringLiteral("global") : tier == 1 ? QStringLiteral("shared") : QStringLiteral("shared+"))
                           .arg(cpuTrials).arg(gpuTrials).arg(cpu.size()),
                       !cpu.isEmpty() && cpu == gpu && cpuTrials == gpuTrials,
                       QStringLiteral("CPU: %1\n      GPU: %2").arg(formatSpectrum(cpu), formatSpectrum(gpu)));
        }
    }

    // Окно Штерна–Дюмера на процессоре. Произведение двух кодов Хэмминга
    // [31,26] — [961,676], d = 9: слов веса 9 ровно A₃² = 155² = 24025,
    // легче нет; план берёт окно, спектр с окном совпадает с точным и со
    // спектром без окна, а времени уходит меньше. У разреженной [1000,500]
    // (по пять единиц в строке) ключи скошены и лёгких слов тьма — окно
    // план обязан отвергнуть (см. cutover в Leon::plan).
    {
        auto kron = [](const QStringList& a, const QStringList& b) {
            QStringList out;
            for (const QString& ra : a)
                for (const QString& rb : b) {
                    QString row;
                    row.reserve(ra.size() * rb.size());
                    for (const QChar& x : ra)
                        row += x == QLatin1Char('1') ? rb : QString(rb.size(), QLatin1Char('0'));
                    out << row;
                }
            return out;
        };
        const QStringList ham31 = Hamming::build(5, false, 0).rows;
        std::mt19937_64 rng(5);
        QStringList sparse;
        for (int r = 0; r < 500; ++r) {
            QString row(1000, QLatin1Char('0'));
            for (int i = 0; i < 5; ++i) row[int(rng() % 1000)] = QLatin1Char('1');
            sparse << row;
        }
        struct WinCase { QString name; QStringList rows; int weight; Spectrum exact; bool wantWindow; };
        const QVector<WinCase> cases = {
            { QStringLiteral("Хэмминг [31,26]²"), kron(ham31, ham31), 9, Spectrum{ { 9, 24025 } }, true },
            { QStringLiteral("разреженная [1000,500]"), sparse, 12, Spectrum(), false },
        };
        for (const WinCase& c : cases) {
            const int n = c.rows.first().length(), k = c.rows.size();
            const Leon::SternProfile profile = Leon::sternProfile(c.rows);
            const Leon::Plan withWindow = Leon::plan(n, k, c.weight, 1e-12, false, &profile);
            const Leon::Plan plain = Leon::plan(n, k, c.weight, 1e-12, false, &profile,
                                                Leon::WindowPolicy::none());

            RunConfig cfg;
            cfg.matrix     = c.rows;
            cfg.algorithm  = Algorithm::RandomInfoSets;
            cfg.leonWeight = c.weight;
            cfg.device     = ComputeDevice::Cpu;
            cfg.threadsCpu = omp_get_num_procs();
            clearCheckpoints();
            auto t0 = std::chrono::steady_clock::now();
            const Spectrum found = runWorker(cfg);
            const double withSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const quint64 done = g_searchDone;
            // Без окна — тот же код, тот же предел: спектры обязаны совпасть.
            RunConfig plainCfg = cfg;
            plainCfg.window = Leon::WindowPolicy::none();
            clearCheckpoints();
            t0 = std::chrono::steady_clock::now();
            const Spectrum plainFound = runWorker(plainCfg);
            const double plainSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            clearCheckpoints();

            bool ok = !found.isEmpty() && found == plainFound;
            QStringList problems;
            if (found != plainFound)
                problems << QStringLiteral("с окном и без окна спектры разошлись");
            for (int w = 1; w <= c.weight && !c.exact.isEmpty(); ++w) {
                if (found.value(w, 0) != c.exact.value(w, 0)) {
                    ok = false;
                    problems << QStringLiteral("вес %1: точно %2, найдено %3").arg(w).arg(c.exact.value(w, 0)).arg(found.value(w, 0));
                }
            }
            if (c.wantWindow && (withWindow.window == 0 || withSeconds > plainSeconds)) {
                ok = false;
                problems << QStringLiteral("окно должно быть выбрано и выиграть по времени");
            }
            if (!c.wantWindow && withWindow.window != 0) {
                ok = false;
                problems << QStringLiteral("окно на скошенных ключах должно быть отвергнуто");
            }
            expectLeon(QStringLiteral("%1, окно Штерна–Дюмера: план p=%2 l=%3, попыток %4, %5 с (без окна p=%6, попыток %7, %8 с), сделано %9")
                           .arg(c.name).arg(withWindow.rows).arg(withWindow.window).arg(withWindow.trials)
                           .arg(withSeconds, 0, 'f', 1).arg(plain.rows).arg(plain.trials).arg(plainSeconds, 0, 'f', 1).arg(done),
                       ok, problems.join(QStringLiteral("; ")));
        }
    }
}

// Случайный поиск на матрице из файла.
//
// Запуск: SpectrumTests.exe --leon-run <файл матрицы> <вес> [<степень пропуска>] [cpu|gpu]
// Циклическая симметрия (cyclic.h): находится ли она у циклических кодов и
// только у них, совпадает ли представитель у всех сдвигов слова, и верна ли
// оценка вероятности поймать орбиту — против прямого подсчёта по попыткам.
static void testCyclicSymmetry()
{
    g_out << Qt::endl << QStringLiteral("Циклическая симметрия кода") << Qt::endl;

    auto symmetryOf = [](const QStringList& rows) {
        int words = 0;
        const std::vector<quint64> packed = InfoSets::packRows(rows, words);
        return Cyclic::find(packed.data(), rows.size(), rows.first().length(), words);
    };
    struct Expect { QString name; QStringList rows; int start; int length; };
    const QVector<Expect> expect = {
        { QStringLiteral("БЧХ [31,16]"),             Bch::build(5, 3, false, 0).rows, 0, 31 },
        { QStringLiteral("БЧХ [63,36]"),             Bch::build(6, 5, false, 0).rows, 0, 63 },
        { QStringLiteral("расширенный БЧХ [64,36]"), Bch::build(6, 5, true, 0).rows,  0, 63 },
        { QStringLiteral("укороченный БЧХ [60,33]"), Bch::build(6, 5, false, 3).rows, 0, 0 },
        { QStringLiteral("Хэмминг [15,11]"),         Hamming::build(4, false, 0).rows, 0, 15 },
        { QStringLiteral("расширенный Хэмминг [16,11]"), Hamming::build(4, true, 0).rows, 0, 15 },
        { QStringLiteral("случайный [40,16]"),       Bz::scramble(Bz::systematicRandom(16, 40, 7), 11), 0, 0 },
    };
    for (const Expect& e : expect) {
        const Cyclic::Symmetry sym = symmetryOf(e.rows);
        const bool ok = sym.length == e.length && (e.length == 0 || sym.start == e.start);
        const QString what = QStringLiteral("%1: сдвиг [%2, %3)").arg(e.name).arg(sym.start).arg(sym.start + sym.length);
        if (ok) { ++g_passed; g_out << "  ok       " << what << Qt::endl; }
        else {
            ++g_failed;
            g_out << QStringLiteral("  ПРОВАЛ   ") << what
                  << QStringLiteral("  (ожидался [%1, %2))").arg(e.start).arg(e.start + e.length) << Qt::endl;
        }
    }

    // Представитель и размер орбиты: у всех сдвигов слова представитель
    // один, размер — число различных сдвигов. Слова — случайные и с периодом.
    {
        Mixing::Xorshift64 rng{ 0xC1C1EULL };
        int bad = 0, checked = 0;
        for (int trial = 0; trial < 400; ++trial) {
            const int n = 5 + int(rng.next() % 140);
            const Cyclic::Symmetry sym{ int(rng.next() % 2), n - int(rng.next() % 2) };
            if (sym.start + sym.length > n) continue;
            const int words = (n + 63) / 64;
            std::vector<quint64> word(size_t(words), 0ULL);
            // Каждое третье слово — периодическое: узор повторяется с шагом p.
            const int p = trial % 3 == 0 ? std::max(1, sym.length / std::max(1, 1 + int(rng.next() % 6))) : sym.length;
            const bool periodic = p < sym.length && sym.length % p == 0;
            for (int c = 0; c < n; ++c) {
                bool bit = (rng.next() & 3) == 0;
                if (periodic && c >= sym.start && c < sym.start + sym.length && c - sym.start >= p) {
                    const int src = sym.start + (c - sym.start) % p;
                    bit = (word[size_t(src >> 6)] >> (src & 63)) & 1ULL;
                }
                if (bit) word[size_t(c >> 6)] |= 1ULL << (c & 63);
            }
            std::vector<quint64> canon(static_cast<size_t>(words)), turned(canon), other(canon);
            const int orbit = Cyclic::canonical(word.data(), words, sym, canon.data());
            std::set<std::vector<quint64>> distinct;
            for (int t = 0; t < sym.length; ++t) {
                Cyclic::rotate(word.data(), words, sym, t, turned.data());
                distinct.insert(turned);
                const int o = Cyclic::canonical(turned.data(), words, sym, other.data());
                ++checked;
                if (other != canon || o != orbit) ++bad;
            }
            if (int(distinct.size()) != orbit) ++bad;
        }
        const QString what = QStringLiteral("представитель орбиты: %1 сдвигов").arg(checked);
        if (bad == 0 && checked > 1000) { ++g_passed; g_out << "  ok       " << what << Qt::endl; }
        else { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   ") << what << QStringLiteral(", расхождений %1").arg(bad) << Qt::endl; }
    }

    // Совместная вероятность: при полном совпадении носителей — не меньше
    // одиночной, при пересечении — не больше; оценка орбиты не меньше
    // одиночной вероятности.
    {
        bool ok = true;
        QStringList problems;
        for (int window : { 0, 6 }) {
            const int n = 63, k = 36, w = 11, rows = 2;
            const double single = Leon::catchProbabilityFor(n, k, w, rows, window);
            const double same   = Leon::jointCatchProbability(n, k, w, w, rows, window);
            if (same < single * (1.0 - 1e-9)) { ok = false; problems << QStringLiteral("окно %1: P(a=w) < P").arg(window); }
            for (int a = 0; a < w; ++a)
                if (Leon::jointCatchProbability(n, k, w, a, rows, window) > same * (1.0 + 1e-9)) {
                    ok = false; problems << QStringLiteral("окно %1: P(a=%2) > P(a=w)").arg(window).arg(a);
                }
            const double orbit = Leon::orbitCatchProbability(n, k, w, rows, window, Cyclic::Symmetry{ 0, 63 });
            if (orbit < single) { ok = false; problems << QStringLiteral("окно %1: орбита < слова").arg(window); }
        }
        expectLeon(QStringLiteral("совместная вероятность поимки и оценка орбиты"), ok, problems.join(QStringLiteral("; ")));

        // Справка: во сколько раз оценка орбиты выше вероятности одного слова
        // (во столько же раз меньше попыток) на кодах длиной до 1023.
        struct Gain { int n, k, w; };
        for (const Gain& c : { Gain{ 127, 92, 16 }, Gain{ 255, 131, 45 }, Gain{ 1023, 513, 80 } }) {
            const double single = Leon::catchProbability(c.n, c.k, c.w, 2);
            const double orbit  = Leon::orbitCatchProbability(c.n, c.k, c.w, 2, 0, Cyclic::Symmetry{ 0, c.n });
            g_out << QStringLiteral("           [%1,%2] до веса %3, 2 строки: орбита ловится в %4 раза вероятнее слова")
                         .arg(c.n).arg(c.k).arg(c.w).arg(orbit / single, 0, 'f', 1) << Qt::endl;
        }
    }

    // Оценка орбиты против прямого счёта: орбита слова минимального веса
    // БЧХ [31,16] и попытки Ли–Брикелла с одной строкой — какая доля попыток
    // ловит хоть одно её слово. Оценка — нижняя граница по модели случайного
    // множества, поэтому замер обязан быть не ниже её (с поправкой на шум).
    {
        const QStringList rows = Bch::build(5, 3, false, 0).rows;
        const int k = rows.size(), n = rows.first().length();
        int words = 0;
        const std::vector<quint64> packed = InfoSets::packRows(rows, words);
        const Cyclic::Symmetry sym = Cyclic::find(packed.data(), k, n, words);
        // Слово минимального веса — полным перебором.
        std::vector<quint64> target;
        int d = n + 1;
        for (quint64 mask = 1; mask < (1ULL << k); ++mask) {
            std::vector<quint64> word(size_t(words), 0ULL);
            for (int i = 0; i < k; ++i)
                if ((mask >> i) & 1ULL)
                    for (int w = 0; w < words; ++w) word[size_t(w)] ^= packed[size_t(i) * words + w];
            int weight = 0;
            for (quint64 x : word) weight += Isd::popcount64(x);
            if (weight < d) { d = weight; target = word; }
        }
        std::vector<quint64> targetCanon(static_cast<size_t>(words));
        Cyclic::canonical(target.data(), words, sym, targetCanon.data());

        const int rowsPerTrial = 1;
        const quint64 trials = 60000;
        quint64 caughtOrbit = 0, caughtWord = 0;
        std::vector<quint64> canon(static_cast<size_t>(words));
        for (quint64 t = 0; t < trials; ++t) {
            bool orbitHit = false, wordHit = false;
            Leon::trial(packed.data(), k, n, words, rowsPerTrial, d, t, [&](const quint64* word, int weight) {
                if (weight != d) return;
                if (std::equal(word, word + words, target.data())) wordHit = true;
                Cyclic::canonical(word, words, sym, canon.data());
                if (canon == targetCanon) orbitHit = true;
            });
            caughtOrbit += orbitHit ? 1 : 0;
            caughtWord  += wordHit ? 1 : 0;
        }
        const double bound   = Leon::orbitCatchProbability(n, k, d, rowsPerTrial, 0, sym);
        const double single  = Leon::catchProbability(n, k, d, rowsPerTrial);
        const double measured = double(caughtOrbit) / double(trials);
        // Шум замера — три сигмы биномиального счёта.
        const double sigma = std::sqrt(bound * (1.0 - bound) / double(trials));
        const bool ok = measured >= bound - 3.0 * sigma;
        expectLeon(QStringLiteral("БЧХ [31,16], вес %1: орбиту ловит %2 попыток, оценка снизу %3, "
                                  "одно слово — %4 (модель %5)")
                       .arg(d).arg(measured, 0, 'g', 3).arg(bound, 0, 'g', 3)
                       .arg(double(caughtWord) / double(trials), 0, 'g', 3).arg(single, 0, 'g', 3),
                   ok, QStringLiteral("замер ниже оценки"));
    }
}

// Случайный поиск у циклических кодов: по орбитам спектр тот же, что
// точный, а попыток меньше, чем по отдельным словам.
static void testLeonCyclic()
{
    g_out << Qt::endl << QStringLiteral("Случайный поиск по орбитам сдвигов") << Qt::endl;

    struct Case { QString name; QStringList rows; int weight; };
    const QVector<Case> cases = {
        { QStringLiteral("БЧХ [31,16]"),                 Bch::build(5, 3, false, 0).rows, 12 },
        { QStringLiteral("БЧХ [63,45]"),                 Bch::build(6, 3, false, 0).rows, 11 },
        { QStringLiteral("расширенный БЧХ [64,45]"),     Bch::build(6, 3, true, 0).rows,  12 },
        { QStringLiteral("БЧХ [63,36]"),                 Bch::build(6, 5, false, 0).rows, 15 },
        { QStringLiteral("Хэмминг [63,57]"),             Hamming::build(6, false, 0).rows, 5 },
        { QStringLiteral("расширенный Хэмминг [64,57]"), Hamming::build(6, true, 0).rows,  6 },
    };
    for (const Case& c : cases) {
        const int k = c.rows.size(), n = c.rows.first().length();
        // Эталон — полный перебор или дуальный код, смотря что короче.
        RunConfig exactCfg;
        exactCfg.matrix    = c.rows;
        exactCfg.algorithm = k <= n - k ? Algorithm::GrayCode : Algorithm::DualCode;
        exactCfg.device    = ComputeDevice::Cpu;
        exactCfg.threadsCpu = std::max(4, omp_get_num_procs());
        clearCheckpoints();
        const Spectrum exact = runWorker(exactCfg);

        for (ComputeDevice device : { ComputeDevice::Cpu, ComputeDevice::Gpu }) {
            const bool gpu = device == ComputeDevice::Gpu;
            const QString who = gpu ? QStringLiteral("GPU") : QStringLiteral("CPU");
            if (gpu && !g_gpuAvailable) {
                g_out << QStringLiteral("  ПРОПУСК  ") << c.name << QStringLiteral(" GPU  (GPU недоступен)") << Qt::endl;
                continue;
            }
            RunConfig cfg;
            cfg.matrix     = c.rows;
            cfg.algorithm  = Algorithm::RandomInfoSets;
            cfg.leonWeight = c.weight;
            cfg.leonMissExponent = 9;
            cfg.device     = device;
            cfg.threadsCpu = std::max(4, omp_get_num_procs());
            cfg.window     = Leon::WindowPolicy::none();

            cfg.cyclicSearch = false;
            clearCheckpoints();
            const auto t0 = std::chrono::steady_clock::now();
            const Spectrum plain = runWorker(cfg);
            const double plainSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const quint64 plainTrials = g_searchTotal;

            cfg.cyclicSearch = true;
            clearCheckpoints();
            const auto t1 = std::chrono::steady_clock::now();
            const Spectrum orbits = runWorker(cfg);
            const double orbitSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
            const quint64 orbitTrials = g_searchTotal;
            clearCheckpoints();

            // Сравнивается работа, а не число попыток: с орбитами поимка
            // вероятнее, и план берёт перебор помельче — попыток бывает и
            // больше, зато каждая в разы дешевле.
            int words = 0;
            const std::vector<quint64> packed = InfoSets::packRows(c.rows, words);
            const Cyclic::Symmetry sym = Cyclic::find(packed.data(), k, n, words);
            const double miss = std::pow(10.0, -cfg.leonMissExponent);
            const Leon::Plan planPlain = Leon::plan(n, k, c.weight, miss, gpu, nullptr, cfg.window);
            const Leon::Plan planOrbit = Leon::plan(n, k, c.weight, miss, gpu, nullptr, cfg.window, sym);
            const double plainWork = double(plainTrials) * planPlain.costPerTrial;
            const double orbitWork = double(orbitTrials) * planOrbit.costPerTrial;
            bool ok = !orbits.isEmpty() && sym.active() && orbitWork < plainWork;
            QStringList problems;
            if (!sym.active()) problems << QStringLiteral("симметрия не найдена");
            if (orbitWork >= plainWork) problems << QStringLiteral("работы не меньше");
            for (int w = 1; w <= c.weight; ++w) {
                if (orbits.value(w, 0) != exact.value(w, 0)) {
                    ok = false;
                    problems << QStringLiteral("вес %1: точно %2, по орбитам %3").arg(w).arg(exact.value(w, 0)).arg(orbits.value(w, 0));
                }
                if (plain.value(w, 0) != exact.value(w, 0)) {
                    ok = false;
                    problems << QStringLiteral("вес %1: точно %2, по словам %3").arg(w).arg(exact.value(w, 0)).arg(plain.value(w, 0));
                }
            }
            expectLeon(QStringLiteral("%1 %2 до веса %3: работы в %4 раза меньше (попыток %5 по %6 строк "
                                      "против %7 по %8), %9 с против %10 с")
                           .arg(c.name).arg(who).arg(c.weight)
                           .arg(plainWork / std::max(1.0, orbitWork), 0, 'f', 1)
                           .arg(orbitTrials).arg(planOrbit.rows).arg(plainTrials).arg(planPlain.rows)
                           .arg(orbitSec, 0, 'f', 2).arg(plainSec, 0, 'f', 2),
                       ok, problems.join(QStringLiteral("; ")));
        }
    }

    // Список слов наружу — все слова каждой орбиты: код произведения строит
    // из них наборы. Сверка с полным перебором компоненты.
    {
        const QStringList rows = Bch::build(5, 2, false, 0).rows;   // [31,21], d = 5
        Worker worker;
        RunConfig cfg;
        cfg.matrix     = rows;
        cfg.algorithm  = Algorithm::RandomInfoSets;
        cfg.leonWeight = 8;
        cfg.device     = ComputeDevice::Cpu;
        worker.setSettings(makeSettings(cfg));
        worker.setAutosaveRoot(autosaveRoot());
        worker.setKeepFoundWords(true);
        clearCheckpoints();
        worker.computeSpectrum();
        clearCheckpoints();
        const Product::Component brute = Product::bruteForce(rows, 8);
        std::set<std::vector<quint64>> listed, expected;
        const int words = brute.wordsPerRow;
        for (size_t i = 0; i < worker.foundWeights().size(); ++i)
            listed.insert(std::vector<quint64>(worker.foundWords().begin() + ptrdiff_t(i * words),
                                               worker.foundWords().begin() + ptrdiff_t((i + 1) * words)));
        for (quint64 i = 0; i < brute.wordCount(); ++i)
            expected.insert(std::vector<quint64>(brute.word(size_t(i)), brute.word(size_t(i)) + words));
        expectLeon(QStringLiteral("БЧХ [31,21]: слова до веса 8 наружу — все сдвиги (%1 слов, перебором %2)")
                       .arg(listed.size()).arg(expected.size()),
                   listed == expected && worker.foundWeights().size() == listed.size(),
                   QStringLiteral("списки разные"));
    }
}

// Точный спектр через дуальный код (Мак-Вильямс), для сверки поиска на
// кодах, где прямой перебор невозможен, а дуальный — да.
//
// Запуск: SpectrumTests.exe --dual-run <файл|bch:m,r|hamming:r> [до веса] [cpu|gpu]
static int dualRun(const QString& path, int upTo, const QString& device)
{
    RunConfig cfg;
    if (!loadMatrixOrCase(path, cfg))
        return 2;
    cfg.algorithm  = Algorithm::DualCode;
    cfg.device     = device == QStringLiteral("gpu") ? ComputeDevice::Gpu : ComputeDevice::Cpu;
    cfg.threadsCpu = omp_get_num_procs();
    const int k = cfg.matrix.size(), n = cfg.matrix.first().length();
    g_out << QStringLiteral("[%1,%2], дуальный перебор 2^%3").arg(n).arg(k).arg(n - k) << Qt::endl;
    g_out.flush();
    clearCheckpoints();
    const auto t = std::chrono::steady_clock::now();
    const Spectrum spectrum = runWorker(cfg);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    clearCheckpoints();
    g_out << QStringLiteral("%1 с").arg(sec, 0, 'f', 1) << Qt::endl;
    for (auto it = spectrum.cbegin(); it != spectrum.cend() && (upTo <= 0 || it.key() <= upTo); ++it)
        g_out << QStringLiteral("   %1  %2").arg(it.key(), 3).arg(it.value(), 12) << Qt::endl;
    return 0;
}

static int leonRun(const QString& path, int weight, int missExponent, const QString& device)
{
    RunConfig cfg;
    if (!loadMatrixOrCase(path, cfg))
        return 2;
    cfg.algorithm        = Algorithm::RandomInfoSets;
    cfg.leonWeight       = weight;
    cfg.leonMissExponent = missExponent;
    cfg.device           = device.startsWith(QStringLiteral("gpu")) ? ComputeDevice::Gpu : ComputeDevice::Cpu;
    cfg.threadsCpu       = omp_get_num_procs();
    // «cpu-plain» — процессор без окна Штерна–Дюмера, для сравнения;
    // «gpu-window» — видеокарта с окном; «-words» в конце — без поиска по
    // орбитам у циклического кода.
    cfg.window.cpu    = !device.startsWith(QStringLiteral("cpu-plain"));
    cfg.window.gpu    = device.startsWith(QStringLiteral("gpu-window"));
    cfg.cyclicSearch  = !device.endsWith(QStringLiteral("-words"));

    const int k = cfg.matrix.size();
    const int n = cfg.matrix.first().length();
    int words = 0;
    const std::vector<quint64> packed = InfoSets::packRows(cfg.matrix, words);
    const Cyclic::Symmetry symmetry = cfg.cyclicSearch ? Cyclic::find(packed.data(), k, n, words)
                                                       : Cyclic::Symmetry();
    if (symmetry.active())
        g_out << QStringLiteral("циклический код: сдвиг столбцов [%1, %2), поиск по орбитам")
                     .arg(symmetry.start).arg(symmetry.start + symmetry.length) << Qt::endl;
    const Leon::SternProfile profile = Leon::sternProfile(cfg.matrix);
    const Leon::Plan plan = Leon::plan(n, k, weight, std::pow(10.0, -missExponent),
                                       cfg.device == ComputeDevice::Gpu, &profile, cfg.window, symmetry);
    g_out << QStringLiteral("[%1,%2], все слова до веса %3, пропуск 10^-%4, %8: %5 строк за попытку%9, попыток %6, слов %7")
                 .arg(n).arg(k).arg(weight).arg(missExponent)
                 .arg(plan.rows).arg(plan.trials).arg(double(plan.trials) * plan.wordsPerTrial, 0, 'g', 3)
                 .arg(cfg.device == ComputeDevice::Gpu ? QStringLiteral("GPU") : QStringLiteral("CPU"))
                 .arg(plan.window > 0 ? QStringLiteral(", окно %1").arg(plan.window) : QString()) << Qt::endl;
    g_out.flush();

    clearCheckpoints();
    const auto t = std::chrono::steady_clock::now();
    const Spectrum spectrum = runWorker(cfg);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    clearCheckpoints();

    g_out << QStringLiteral("попыток %1 из %2, %3 с; вероятность пропуска по модели ~%4")
                 .arg(g_searchDone).arg(g_searchTotal).arg(sec, 0, 'f', 1).arg(g_searchMiss, 0, 'g', 2) << Qt::endl;
    for (auto it = spectrum.cbegin(); it != spectrum.cend(); ++it) {
        const float unseen = it.key() < g_searchUnseen.size() ? g_searchUnseen.at(it.key()) : 0.0f;
        g_out << QStringLiteral("   %1  %2%3").arg(it.key(), 3).arg(it.value(), 12)
                     .arg(unseen >= 0.5f ? QStringLiteral("  (ещё ~%1 не найдено)").arg(qRound64(double(unseen))) : QString())
              << Qt::endl;
    }
    return 0;
}

// ------------------------------------------------ коды произведения

// Порождающая матрица C1 ⊗ C2: строка (i, j) — произведение строки i из G1
// на строку j из G2, позиция (x, y) идёт под номером x·n2 + y.
static QStringList kronecker(const QStringList& g1, const QStringList& g2)
{
    const int n1 = g1.first().length(), n2 = g2.first().length();
    QStringList out;
    for (const QString& a : g1)
        for (const QString& b : g2) {
            QString row(n1 * n2, QLatin1Char('0'));
            for (int x = 0; x < n1; ++x)
                if (a.at(x) == QLatin1Char('1'))
                    for (int y = 0; y < n2; ++y)
                        if (b.at(y) == QLatin1Char('1'))
                            row[x * n2 + y] = QLatin1Char('1');
            out << row;
        }
    return out;
}

// Спектр произведения по компонентам: ранги 1..maxRank, до веса maxWeight.
static Spectrum productSpectrum(const QStringList& g1, const QStringList& g2,
                                int maxRank, quint64 maxWeight, int& exactUpTo)
{
    const Product::Component c1 = Product::bruteForce(g1, g1.first().length());
    const Product::Component c2 = Product::bruteForce(g2, g2.first().length());
    std::vector<quint64> total = Product::rankOne(c1, c2, maxWeight);
    for (int r = 2; r <= maxRank; ++r) {
        Product::ProfileMap p1, p2;
        const int limit1 = int(maxWeight / quint64(c2.d));
        const int limit2 = int(maxWeight / quint64(c1.d));
        if (!Product::profiles(c1, r, limit1, 1ULL << 40, p1, false)
            || !Product::profiles(c2, r, limit2, 1ULL << 40, p2, true))
            break;
        const std::vector<quint64> part = Product::rankR(p1, p2, r, maxWeight);
        for (size_t w = 0; w < total.size(); ++w) total[w] += part[w];
    }
    exactUpTo = int(std::min<quint64>(maxWeight, Product::rankWeightBound(c1.d, c2.d, maxRank + 1) - 1));
    Spectrum spec;
    spec[0] = 1;
    for (size_t w = 1; w < total.size(); ++w)
        if (total[w] > 0) spec[int(w)] = total[w];
    return spec;
}

// БЧХ-конструктор: минимальные многочлены против справочной таблицы,
// порождающий многочлен из примера Макса, матрица [63,51] из его библиотеки.
static void testBchCode()
{
    g_out << Qt::endl << QStringLiteral("БЧХ-коды") << Qt::endl;

    // Справочные значения (восьмеричные) для представителей классов.
    struct Known { int m; int exponent; const char* octal; };
    const Known known[] = {
        // В печатной таблице после первого совпадения классов номер строки —
        // просто порядковый: у m=5 строка «11» — это m_15, у m=6 строка «23» — m_31.
        { 2, 1, "7" },   { 3, 3, "15" },  { 4, 5, "7" },   { 4, 7, "31" },   { 5, 15, "51" },
        { 6, 9, "15" },  { 6, 31, "141" }, { 7, 9, "277" }, { 7, 19, "313" }, { 7, 63, "221" },
        { 8, 1, "435" }, { 8, 13, "453" }, { 9, 55, "1275" }, { 10, 33, "75" }, { 10, 57, "3121" },
    };
    bool tableOk = true;
    QStringList wrong;
    for (const Known& kn : known) {
        const auto table = Bch::minimalPolynomials(kn.m);
        const auto it = std::find_if(table.begin(), table.end(),
                                     [&](const Bch::MinimalPolynomial& p) { return p.exponent == kn.exponent; });
        const QString got = it == table.end() ? QStringLiteral("нет") : it->octal;
        if (got != QLatin1String(kn.octal)) { tableOk = false; wrong << QStringLiteral("m=%1 i=%2: %3, ждали %4").arg(kn.m).arg(kn.exponent).arg(got).arg(kn.octal); }
    }
    expectLeon(QStringLiteral("минимальные многочлены совпадают со справочником"), tableOk, wrong.join(QStringLiteral("; ")));

    // Число классов (без нулевого): m=7 — 18, m=10 — 106.
    expectLeon(QStringLiteral("представителей классов: m=7 — %1, m=10 — %2")
                   .arg(Bch::minimalPolynomials(7).size()).arg(Bch::minimalPolynomials(10).size()),
               Bch::minimalPolynomials(7).size() == 18 && Bch::minimalPolynomials(10).size() == 106);

    // Пример: 211·217·235·367·277.
    const Bch::Code c127 = Bch::build(7, 5, false, 0);
    expectLeon(QStringLiteral("[127,92]: g = 211·217·235·367·277, δ = 11"),
               c127.n == 127 && c127.k == 92 && c127.designedDistance == 11 && c127.corrects == 5
                   && c127.generator == QStringLiteral("110010100111011000000010010011010111")
                   && c127.rows.size() == 92 && c127.rows.first().length() == 127,
               c127.generator);

    // Матрица [63,51] — как в библиотеке Макса (I | P, x^{n-k+i} mod g от младшей степени).
    const Bch::Code c63 = Bch::build(6, 2, false, 0);
    const QStringList expect63 = {
        QStringLiteral("100000000000000000000000000000000000000000000000000100111001010"),
        QStringLiteral("010000000000000000000000000000000000000000000000000010011100101"),
        QStringLiteral("001000000000000000000000000000000000000000000000000101110111000"),
    };
    expectLeon(QStringLiteral("[63,51]: первые строки как у сохранённой матрицы"),
               c63.n == 63 && c63.k == 51 && c63.designedDistance == 5
                   && c63.rows.mid(0, 3) == expect63,
               c63.rows.value(0));

    // Каждая строка — кодовое слово: делится на g. Проверка через спектр:
    // у [15,7,5] (m=4, два класса) точный спектр A_5 = 18, A_6 = 30.
    const Bch::Code c15 = Bch::build(4, 2, false, 0);
    const Spectrum s15 = Reference::bruteForce(c15.rows);
    expectLeon(QStringLiteral("[15,7,5]: A_5 = %1, A_6 = %2").arg(s15.value(5)).arg(s15.value(6)),
               c15.k == 7 && s15.value(5) == 18 && s15.value(6) == 30 && s15.value(4, 0) == 0);

    // Расширение: [16,7,6]; укорочение на 2: [13,5]; и то и другое: [14,5].
    const Bch::Code ext = Bch::build(4, 2, true, 0);
    const Bch::Code sh  = Bch::build(4, 2, false, 2);
    const Bch::Code both = Bch::build(4, 2, true, 2);
    const Spectrum sExt = Reference::bruteForce(ext.rows);
    expectLeon(QStringLiteral("расширение и укорочение: [16,7,6] (A_5 = 0, A_6 = %1), [13,5], [14,5]").arg(sExt.value(6)),
               ext.n == 16 && ext.k == 7 && ext.designedDistance == 6 && sExt.value(5, 0) == 0 && sExt.value(6) == 48
                   && sh.n == 13 && sh.k == 5 && both.n == 14 && both.k == 5
                   && sh.rows.size() == 5 && sh.rows.first().length() == 13);

    // Конструктивное расстояние учитывает пропуск: m=7, 8 классов (до 15) —
    // корни α^1…α^18, δ = 19; m=4, 3 класса — [15,5,7]; 4 класса — [15,1,15].
    const Bch::Code c8 = Bch::describe(7, 8, false, 0);
    const Bch::Code r3 = Bch::describe(4, 3, false, 0);
    const Bch::Code r4 = Bch::describe(4, 4, false, 0);
    expectLeon(QStringLiteral("δ по границе БЧХ: m=7/8 классов — [127,%1,δ=%2]; m=4 — [15,%3,%4], [15,%5,%6]")
                   .arg(c8.k).arg(c8.designedDistance).arg(r3.k).arg(r3.designedDistance).arg(r4.k).arg(r4.designedDistance),
               c8.k == 71 && c8.designedDistance == 19 && r3.k == 5 && r3.designedDistance == 7
                   && r4.k == 1 && r4.designedDistance == 15);
}

static void testHammingCode()
{
    g_out << Qt::endl << QStringLiteral("Коды Хэмминга") << Qt::endl;
    const Hamming::Code h7  = Hamming::build(3, false, 0);
    const Hamming::Code h8  = Hamming::build(3, true, 0);
    const Hamming::Code h15 = Hamming::build(4, false, 0);
    const Hamming::Code h12 = Hamming::build(4, false, 3);
    const Spectrum s7 = Reference::bruteForce(h7.rows), s8 = Reference::bruteForce(h8.rows),
                   s15 = Reference::bruteForce(h15.rows), s12 = Reference::bruteForce(h12.rows);
    expectLeon(QStringLiteral("[7,4,3]: A_3 = %1, A_4 = %2, A_7 = %3").arg(s7.value(3)).arg(s7.value(4)).arg(s7.value(7)),
               h7.n == 7 && h7.k == 4 && s7.value(3) == 7 && s7.value(4) == 7 && s7.value(7) == 1
                   && h7.rows == QStringList{ QStringLiteral("1000101"), QStringLiteral("0100111"),
                                              QStringLiteral("0010110"), QStringLiteral("0001011") });
    expectLeon(QStringLiteral("[8,4,4] расширенный: A_4 = %1, A_8 = %2").arg(s8.value(4)).arg(s8.value(8)),
               h8.n == 8 && h8.d == 4 && s8.value(3, 0) == 0 && s8.value(4) == 14 && s8.value(8) == 1);
    expectLeon(QStringLiteral("[15,11,3]: A_3 = %1, A_4 = %2").arg(s15.value(3)).arg(s15.value(4)),
               h15.n == 15 && h15.k == 11 && s15.value(3) == 35 && s15.value(4) == 105);
    expectLeon(QStringLiteral("[12,8] укороченный на 3: d = %1").arg(s12.isEmpty() ? 0 : s12.firstKey() == 0 ? (s12.size() > 1 ? (s12.constBegin() + 1).key() : 0) : s12.firstKey()),
               h12.n == 12 && h12.k == 8 && h12.rows.size() == 8 && h12.rows.first().length() == 12
                   && s12.value(1, 0) == 0 && s12.value(2, 0) == 0 && s12.value(3, 0) > 0);
    const QStringList parity = Parity::build(4);
    const Spectrum sp = Reference::bruteForce(parity);
    expectLeon(QStringLiteral("чётность [5,4,2]: A_2 = %1, A_4 = %2").arg(sp.value(2)).arg(sp.value(4)),
               parity.size() == 4 && parity.first() == QStringLiteral("10001") && parity.last() == QStringLiteral("00011")
                   && sp.value(2) == 10 && sp.value(4) == 5 && sp.value(1, 0) == 0 && sp.value(3, 0) == 0);
    expectLeon(QStringLiteral("r = 11: [2047,2036], расширенный [2048,2036]"),
               Hamming::describe(11, false, 0).n == 2047 && Hamming::describe(11, true, 0).n == 2048
                   && Hamming::describe(11, false, 0).k == 2036);
}

static void testProductCode()
{
    g_out << Qt::endl << QStringLiteral("Коды произведения: низ спектра по компонентам") << Qt::endl;

    struct Case { QString name; QStringList g1, g2; };
    const QVector<Case> cases = {
        { QStringLiteral("eHamming(8,4) x eHamming(8,4) = [64,16]"), Reference::extHamming8_4(), Reference::extHamming8_4() },
        { QStringLiteral("Hamming(7,4) x Hamming(7,4) = [49,16]"),   Reference::hamming7_4(),    Reference::hamming7_4() },
        { QStringLiteral("Hamming(7,4) x eHamming(8,4) = [56,16]"),  Reference::hamming7_4(),    Reference::extHamming8_4() },
        { QStringLiteral("Hamming(7,4) x rnd[12,5] = [84,20]"),      Reference::hamming7_4(),    Bz::systematicRandom(5, 12, 3) },
    };
    for (const Case& c : cases) {
        const QStringList product = kronecker(c.g1, c.g2);
        const Spectrum exact = Reference::bruteForce(product);
        const int n = product.first().length();
        const int maxRank = std::min(c.g1.size(), c.g2.size());

        // Все ранги — весь спектр целиком: слов ранга выше min(k1,k2) нет.
        int exactUpTo = 0;
        const Spectrum full = productSpectrum(c.g1, c.g2, maxRank, quint64(n), exactUpTo);
        bool ok = full == exact;
        QStringList problems;
        if (!ok)
            for (auto it = exact.cbegin(); it != exact.cend(); ++it)
                if (full.value(it.key(), 0) != it.value())
                    problems << QStringLiteral("вес %1: точно %2, по рангам %3")
                                    .arg(it.key()).arg(it.value()).arg(full.value(it.key(), 0));
        expectLeon(c.name + QStringLiteral(", все ранги (до %1) — весь спектр").arg(maxRank), ok,
                   problems.join(QStringLiteral("; ")));

        // Ранги до rank: ниже границы ранга rank+1 — точно, выше — не больше точного.
        for (int rank = 1; rank < maxRank; ++rank) {
            const Spectrum part = productSpectrum(c.g1, c.g2, rank, quint64(n), exactUpTo);
            bool okR = true;
            QStringList probs;
            for (auto it = exact.cbegin(); it != exact.cend(); ++it) {
                const quint64 got = part.value(it.key(), 0);
                if (it.key() <= exactUpTo ? got != it.value() : got > it.value()) {
                    okR = false;
                    probs << QStringLiteral("вес %1: точно %2, ранги<=%3 дают %4")
                                 .arg(it.key()).arg(it.value()).arg(rank).arg(got);
                }
            }
            for (auto it = part.cbegin(); it != part.cend(); ++it)
                if (!exact.contains(it.key())) { okR = false; probs << QStringLiteral("лишний вес %1").arg(it.key()); }
            expectLeon(c.name + QStringLiteral(", ранги до %1: точно до веса %2").arg(rank).arg(exactUpTo),
                       okR, probs.join(QStringLiteral("; ")));
        }
    }

    // Тот же расчёт через Worker: компоненты маленькие — полный перебор,
    // все ранги; итог совпадает с полным перебором произведения до
    // заявленной точности и попадает в запись автосохранения.
    {
        RunConfig cfg;
        cfg.matrix      = Reference::extHamming8_4();
        cfg.matrix2     = Reference::extHamming8_4();
        cfg.algorithm   = Algorithm::ProductCode;
        cfg.productRank = 4;
        cfg.device      = ComputeDevice::Cpu;
        clearCheckpoints();
        const Spectrum got   = runWorker(cfg);
        const Spectrum exact = Reference::bruteForce(kronecker(cfg.matrix, cfg.matrix2));
        const QVector<AutosaveEntry> entries = testStore().list();
        clearCheckpoints();

        bool ok = g_productExactUpTo >= 24 && !got.isEmpty();
        QStringList problems;
        for (auto it = exact.cbegin(); it != exact.cend(); ++it) {
            const quint64 g = got.value(it.key(), 0);
            if (it.key() <= g_productExactUpTo ? g != it.value() : g != 0) {
                ok = false;
                problems << QStringLiteral("вес %1: точно %2, получено %3").arg(it.key()).arg(it.value()).arg(g);
            }
        }
        if (entries.isEmpty() || entries.first().record.algorithm != ComputationSettings::ProductCode
            || entries.first().record.productExactUpTo != g_productExactUpTo)
            { ok = false; problems << QStringLiteral("запись автосохранения не та"); }
        expectLeon(QStringLiteral("Worker: eHamming(8,4)^2, ранги до 4 — %1").arg(g_productText),
                   ok, problems.join(QStringLiteral("; ")));
    }

    // Большая компонента: порог полного перебора занижен, чтобы компоненты
    // считались вложенным Брауэром–Циммерманом; тогда доступен только ранг 1,
    // и спектр точен до границы Толхёйзена.
    {
        RunConfig cfg;
        cfg.matrix      = Reference::golay24_12();
        cfg.matrix2     = Reference::hamming7_4();
        cfg.algorithm   = Algorithm::ProductCode;
        cfg.productRank = 1;
        cfg.device      = ComputeDevice::Cpu;
        cfg.productBruteForceMaxK = 3;
        clearCheckpoints();
        const Spectrum got = runWorker(cfg);
        clearCheckpoints();

        // Ранг 1 по точным спектрам компонент — независимая сверка.
        const Spectrum sg = Reference::analyticGolay24_12();
        const Spectrum sh = Reference::analyticHamming7_4();
        Spectrum expected;
        expected[0] = 1;
        for (auto a = sg.cbegin(); a != sg.cend(); ++a)
            for (auto b = sh.cbegin(); b != sh.cend(); ++b)
                if (a.key() > 0 && b.key() > 0 && a.key() * b.key() <= g_productExactUpTo)
                    expected[a.key() * b.key()] += a.value() * b.value();

        const int tolhuizen = int(Product::rankWeightBound(8, 3, 2)) - 1;   // 24 + max(8*2, 3*4) - 1 = 39
        expectLeon(QStringLiteral("Worker: Голей x Хэмминг, большие компоненты — %1").arg(g_productText),
                   !got.isEmpty() && got == expected && g_productExactUpTo == tolhuizen,
                   QStringLiteral("ожидалось до %1: %2; получено: %3")
                       .arg(tolhuizen).arg(formatSpectrum(expected), formatSpectrum(got)));
    }

    // Большие компоненты при ранге >= 2 — списки слов случайным поиском.
    // Голей x Хэмминг: у одной компоненты слова собраны Леоном, у другой —
    // перебором (порог 6: Хэмминг с k = 4 ниже, Голей с k = 12 выше);
    // итог обязан совпасть с расчётом по перебранным целиком компонентам.
    {
        RunConfig cfg;
        cfg.matrix      = Reference::golay24_12();
        cfg.matrix2     = Reference::hamming7_4();
        cfg.algorithm   = Algorithm::ProductCode;
        cfg.productRank = 2;
        cfg.device      = ComputeDevice::Cpu;
        clearCheckpoints();
        const Spectrum exact = runWorker(cfg);
        const int exactUpTo = g_productExactUpTo;

        RunConfig leonCfg = cfg;
        leonCfg.productBruteForceMaxK = 6;
        clearCheckpoints();
        const Spectrum viaLeon = runWorker(leonCfg);
        const QVector<AutosaveEntry> entries = testStore().list();
        clearCheckpoints();

        // Записей несколько: вложенный поиск пишет и свою, по компоненте.
        int recordedMiss = -1;
        for (const AutosaveEntry& e : entries)
            if (e.record.algorithm == ComputationSettings::ProductCode)
                recordedMiss = e.record.productMissExponent;
        expectLeon(QStringLiteral("Worker: Голей (Леон) x Хэмминг, ранги до 2 — %1").arg(g_productText),
                   !exact.isEmpty() && viaLeon == exact && g_productExactUpTo == exactUpTo
                       && recordedMiss == 12,
                   QStringLiteral("перебором: %1\n      Леоном: %2").arg(formatSpectrum(exact), formatSpectrum(viaLeon)));
    }

    // Граница Толхёйзена для ранга 2 и порядок GL.
    expectLeon(QStringLiteral("|GL(2,2)| = 6, |GL(3,2)| = 168, |GL(4,2)| = 20160"),
               Product::generalLinearOrder(2) == 6 && Product::generalLinearOrder(3) == 168
                   && Product::generalLinearOrder(4) == 20160);
    expectLeon(QStringLiteral("граница ранга 2 для d = 4, 4: 24; для 3, 3: 15"),
               Product::rankWeightBound(4, 4, 2) == 24 && Product::rankWeightBound(3, 3, 2) == 15);
}

// Код произведения по двум матрицам из файлов.
//
// Запуск: SpectrumTests.exe --product-run <файл 1> <файл 2> [<вес>] [<ранг>] [cpu|gpu]
static int productRun(const QString& path1, const QString& path2, int weight, int rank, const QString& device)
{
    RunConfig c1, c2;
    if (!loadMatrixOrCase(path1, c1) || !loadMatrixOrCase(path2, c2))
        return 2;
    RunConfig cfg;
    cfg.matrix        = c1.matrix;
    cfg.matrix2       = c2.matrix;
    cfg.algorithm     = Algorithm::ProductCode;
    cfg.productWeight = weight;
    cfg.productRank   = rank;
    cfg.device        = device == QStringLiteral("gpu") ? ComputeDevice::Gpu : ComputeDevice::Cpu;
    cfg.threadsCpu    = omp_get_num_procs();
    cfg.autoTune      = cfg.device == ComputeDevice::Gpu;

    g_out << QStringLiteral("[%1,%2] x [%3,%4], вес %5, ранги до %6")
                 .arg(c1.matrix.first().length()).arg(c1.matrix.size())
                 .arg(c2.matrix.first().length()).arg(c2.matrix.size())
                 .arg(weight).arg(rank) << Qt::endl;
    g_out.flush();

    clearCheckpoints();
    const auto t = std::chrono::steady_clock::now();
    const Spectrum spectrum = runWorker(cfg);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    clearCheckpoints();

    g_out << g_productText << QStringLiteral("; %1 с").arg(sec, 0, 'f', 1) << Qt::endl;
    for (auto it = spectrum.cbegin(); it != spectrum.cend(); ++it)
        g_out << QStringLiteral("   %1  %2").arg(it.key(), 6).arg(it.value(), 16) << Qt::endl;
    return 0;
}

// Гарантия алгоритма: ниже границы совпадение с точным спектром обязано быть
// побитовым, выше — БЦ не имеет права насчитать больше, чем есть.
static void testBrouwerZimmermann()
{
    g_out << Qt::endl << QStringLiteral("Брауэр–Циммерман: низ спектра с гарантией") << Qt::endl;

    for (const BzCase& c : bzCases()) {
        const Reference::Spectrum exact = Reference::bruteForce(c.rows);
        const Bz::Result          bz    = Bz::run(c.rows, c.r, c.sets);

        bool ok = true;
        for (auto it = exact.cbegin(); it != exact.cend(); ++it) {
            const quint64 found = bz.spectrum.value(it.key(), 0);
            if (it.key() < bz.guaranteedBelow ? found != it.value() : found > it.value()) {
                ok = false;
                g_out << QStringLiteral("      вес %1: точно %2, БЦ %3 (граница %4)")
                             .arg(it.key()).arg(it.value()).arg(found).arg(bz.guaranteedBelow) << Qt::endl;
            }
        }
        for (auto it = bz.spectrum.cbegin(); it != bz.spectrum.cend(); ++it)
            if (!exact.contains(it.key())) { ok = false; g_out << QStringLiteral("      лишний вес %1").arg(it.key()) << Qt::endl; }

        const QString what = QStringLiteral("%1: веса < %2 точны").arg(c.name).arg(bz.guaranteedBelow);
        if (ok) { ++g_passed; g_out << "  ok       " << what << Qt::endl; }
        else    { ++g_failed; g_out << QStringLiteral("  ПРОВАЛ   ") << what << Qt::endl; }
    }
}

// -------------------------------------------------------------------- main

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

#ifdef Q_OS_WIN
    // Без этого русский вывод превращается в мусор: консоль по умолчанию в CP866.
    SetConsoleOutputCP(CP_UTF8);
#endif
    g_out.setCodec("UTF-8");

    // Отдельное имя приложения: чекпоинты тестов не должны попадать в ветку
    // реестра, которой пользуется сама программа.
    QCoreApplication::setOrganizationName(QStringLiteral("Alpas"));
    QCoreApplication::setApplicationName(QStringLiteral("SpectrumTests"));

    int deviceCount = 0;
    g_gpuAvailable = (cudaGetDeviceCount(&deviceCount) == cudaSuccess) && deviceCount > 0;
    g_out << QStringLiteral("GPU: ")
          << (g_gpuAvailable ? QStringLiteral("доступен")
                             : QStringLiteral("не найден, GPU-тесты пропускаются"))
          << Qt::endl;

    const QStringList args = app.arguments();

    const int bzAt = args.indexOf(QStringLiteral("--bz-file"));
    if (bzAt >= 0 && bzAt + 2 < args.size()) {
        const int sets = bzAt + 3 < args.size() ? args.at(bzAt + 3).toInt() : 8;
        const int rc = bzFile(args.at(bzAt + 1), args.at(bzAt + 2).toInt(), sets > 0 ? sets : 8);
        g_out.flush();
        return rc;
    }

    if (args.contains(QStringLiteral("--bz"))) {
        bzReport();
        g_out.flush();
        return 0;
    }

    const int productAt = args.indexOf(QStringLiteral("--product-run"));
    if (productAt >= 0 && productAt + 2 < args.size()) {
        const int weight = productAt + 3 < args.size() ? args.at(productAt + 3).toInt() : 0;
        const int rank   = productAt + 4 < args.size() ? args.at(productAt + 4).toInt() : 2;
        const QString device = productAt + 5 < args.size() ? args.at(productAt + 5).toLower() : QStringLiteral("cpu");
        const int rc = productRun(args.at(productAt + 1), args.at(productAt + 2), weight, rank > 0 ? rank : 2, device);
        g_out.flush();
        return rc;
    }

    // --stern-bench <файл> <вес> [p] — цена попытки с окном и без, один поток,
    // и профиль ключей: сколько пар списков совпадает против равномерных
    // L₁·L₂/2^l.
    if (args.contains(QStringLiteral("--stern-bench"))) {
        const int at = args.indexOf(QStringLiteral("--stern-bench"));
        RunConfig cfg;
        if (at + 2 >= args.size() || !loadMatrixOrCase(args.at(at + 1), cfg)) return 2;
        const int maxWeight = args.at(at + 2).toInt();
        const int maxP = at + 3 < args.size() ? args.at(at + 3).toInt() : 2;
        int words = 0;
        const std::vector<quint64> packed = InfoSets::packRows(cfg.matrix, words);
        const int k = cfg.matrix.size(), n = cfg.matrix.first().length();
        auto timeIt = [&](auto&& body, int reps) {
            const auto t = std::chrono::steady_clock::now();
            for (int i = 0; i < reps; ++i) body(quint64(i));
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count() / reps * 1e6;
        };
        quint64 sink = 0;
        auto visit = [&](const quint64*, int) { ++sink; };
        g_out << QStringLiteral("[%1,%2], вес %3, один поток, мкс на попытку:").arg(n).arg(k).arg(maxWeight) << Qt::endl;
        {
            std::vector<int> order;
            InfoSets::InfoSet set;
            int ok = 0;
            const double us = timeIt([&](quint64 t) {
                Leon::shuffledColumns(n, t, order);
                ok += InfoSets::systematize(packed.data(), k, n, words, order, nullptr, set) ? 1 : 0;
            }, 100);
            g_out << QStringLiteral("  систематизация: %1 мкс (удачных %2 из 100)").arg(us, 0, 'f', 1).arg(ok) << Qt::endl;
        }
        for (int p = 1; p <= maxP; ++p) {
            const double us = timeIt([&](quint64 t) { Leon::trial(packed.data(), k, n, words, p, maxWeight, t, visit); }, p == 3 ? 10 : 100);
            g_out << QStringLiteral("  перебор p=%1: %2 мкс (слов %3)").arg(p).arg(us, 0, 'f', 1).arg(Leon::wordsPerTrial(k, p), 0, 'g', 4) << Qt::endl;
        }
        const Leon::SternProfile profile = Leon::sternProfile(cfg.matrix);
        for (int p = 1; p <= std::min(maxP, 2); ++p)
            for (int l : { 8, 12, 16, 20 }) {
                if (l > profile.window) continue;
                const double us = timeIt([&](quint64 t) { Leon::trialStern(packed.data(), k, n, words, p, l, maxWeight, t, visit); }, p == 2 ? 20 : 100);
                const double list    = Leon::sternListSize(k / 2, p) + Leon::sternListSize(k - k / 2, p);
                const double uniform = Leon::sternListSize(k / 2, p) * Leon::sternListSize(k - k / 2, p) / std::ldexp(1.0, l);
                g_out << QStringLiteral("  окно p=%1 l=%2: %3 мкс (список %4, пар %5, у равномерных ключей %6)")
                             .arg(p).arg(l, 2).arg(us, 8, 'f', 1).arg(list, 0, 'g', 4)
                             .arg(profile.pairs[p][l], 0, 'g', 4).arg(uniform, 0, 'g', 4) << Qt::endl;
            }
        const Leon::Plan plan = Leon::plan(n, k, maxWeight, 1e-6, false, &profile);
        const Leon::Plan plain = Leon::plan(n, k, maxWeight, 1e-6, false, nullptr, Leon::WindowPolicy::none());
        g_out << QStringLiteral("  план (пропуск 10^-6): p=%1 l=%2, попыток %3, цена %4 слов; без окна p=%5, попыток %6, цена %7 слов")
                     .arg(plan.rows).arg(plan.window).arg(plan.trials).arg(double(plan.trials) * plan.costPerTrial, 0, 'g', 3)
                     .arg(plain.rows).arg(plain.trials).arg(double(plain.trials) * plain.costPerTrial, 0, 'g', 3) << Qt::endl;
        g_out << QStringLiteral("  (sink %1)").arg(sink) << Qt::endl;
        g_out.flush();
        return 0;
    }

    // --gpu-gauss-bench <файл> [попыток] [p] — цена попытки ядра Леона на
    // видеокарте: p = 0 — один Гаусс (перебора нет), p = 1, 2 — с перебором;
    // слов не выкладывается (maxWeight = 0). Печатает ярус памяти и мкс на
    // попытку по пропускной способности.
    if (args.contains(QStringLiteral("--gpu-gauss-bench"))) {
        const int at = args.indexOf(QStringLiteral("--gpu-gauss-bench"));
        RunConfig cfg;
        if (at + 1 >= args.size() || !loadMatrixOrCase(args.at(at + 1), cfg)) return 2;
        const int trials = at + 2 < args.size() ? args.at(at + 2).toInt() : 2048;
        const int maxP   = at + 3 < args.size() ? args.at(at + 3).toInt() : 2;
        int words = 0;
        const std::vector<quint64> packed = InfoSets::packRows(cfg.matrix, words);
        const int k = cfg.matrix.size(), n = cfg.matrix.first().length();
        const int tier = leonSharedTier(k, n, words, 0);
        DeviceBuffer<quint64> d_mat, d_out, d_scratch;
        DeviceBuffer<unsigned> d_count;
        d_mat.allocate(packed.size());
        CUDA_CALL(cudaMemcpy(d_mat.get(), packed.data(), packed.size() * sizeof(quint64), cudaMemcpyHostToDevice));
        d_out.allocate(size_t(1024) * words);
        d_count.allocate(1);
        const size_t scratchWords = leonScratchWords(k, n, words, 2, 0, 0);
        if (scratchWords > 0) d_scratch.allocate(scratchWords * size_t(trials));
        g_out << QStringLiteral("[%1,%2], ярус памяти %3 (%4), попыток %5:")
                     .arg(n).arg(k).arg(tier)
                     .arg(tier == 0 ? QStringLiteral("глобальная") : tier == 1 ? QStringLiteral("разделяемая") : QStringLiteral("большая разделяемая"))
                     .arg(trials) << Qt::endl;
        for (int p = 0; p <= maxP; ++p) {
            LeonLaunch launch;
            launch.matrix = d_mat.get(); launch.rows = k; launch.cols = n; launch.wordsPerRow = words;
            launch.rowsPerTrial = p; launch.maxWeight = 0; launch.firstTrial = 0; launch.trials = trials;
            launch.outWords = d_out.get(); launch.outCount = d_count.get(); launch.capacity = 1024;
            launch.scratch = d_scratch.get();
            d_count.fillZero();
            launchLeonTrials(launch, LEON_THREADS, nullptr);   // прогрев
            CUDA_CALL(cudaDeviceSynchronize());
            const auto t0 = std::chrono::steady_clock::now();
            launchLeonTrials(launch, LEON_THREADS, nullptr);
            CUDA_CALL(cudaDeviceSynchronize());
            const double us = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / trials * 1e6;
            g_out << QStringLiteral("  p=%1: %2 мкс на попытку").arg(p).arg(us, 0, 'f', 1) << Qt::endl;
#ifdef LEON_PROFILE
            // Такты по фазам у нити 0 блока 0 — их пишет ядро, собранное с
            // LEON_PROFILE (см. leonkernel.cu); без него в буфере пусто.
            if (p == 0) {
                std::vector<quint64> prof(7, 0);
                CUDA_CALL(cudaMemcpy(prof.data(), d_out.get(), 7 * sizeof(quint64), cudaMemcpyDeviceToHost));
                const double cols = double(std::max<quint64>(1, prof[6]));
                if (getenv("LEON_GAUSS") && std::string(getenv("LEON_GAUSS")) != "col")
                    g_out << QStringLiteral("    тактов на группу (нить 0): ключи %1, поиск опор %2, фаза B %3, исключение %4; групп %5")
                                   .arg(prof[0] / cols, 0, 'f', 0).arg(prof[1] / cols, 0, 'f', 0).arg(prof[2] / cols, 0, 'f', 0)
                                   .arg(prof[3] / cols, 0, 'f', 0).arg(prof[6]) << Qt::endl;
                else
                    g_out << QStringLiteral("    тактов на столбец (нить 0): поиск %1 (из них загрузки %6), барьер %2, обмен+барьер %3, исключение %4, барьер %5")
                                   .arg(prof[0] / cols, 0, 'f', 0).arg(prof[1] / cols, 0, 'f', 0).arg(prof[2] / cols, 0, 'f', 0)
                                   .arg(prof[3] / cols, 0, 'f', 0).arg(prof[4] / cols, 0, 'f', 0).arg(prof[5] / cols, 0, 'f', 0) << Qt::endl;
            }
#endif
        }
        g_out.flush();
        return 0;
    }

    // --table-bench [слов в пачке] [различных] — скорость ShardedWordTable::addBatch
    if (args.contains(QStringLiteral("--table-bench"))) {
        const int at = args.indexOf(QStringLiteral("--table-bench"));
        const size_t perBatch = at + 1 < args.size() ? size_t(args.at(at + 1).toULongLong()) : 250000;
        const size_t distinct = at + 2 < args.size() ? size_t(args.at(at + 2).toULongLong()) : 1800000;
        const int words = 6;
        std::mt19937_64 rng(7);
        std::vector<quint64> pool(distinct * words);
        for (quint64& w : pool) w = rng() & rng() & rng();   // ~1/8 единиц, вес ~40
        std::vector<quint64> batch(perBatch * words);
        Leon::ShardedWordTable table(words, 336, std::max(1, omp_get_max_threads()));
        double total = 0.0;
        const int batches = 100;
        for (int b = 0; b < batches; ++b) {
            for (size_t i = 0; i < perBatch; ++i) {
                const size_t src = size_t(rng() % distinct);
                std::copy(pool.begin() + long(src * words), pool.begin() + long((src + 1) * words),
                          batch.begin() + long(i * words));
            }
            const auto t = std::chrono::steady_clock::now();
            table.addBatch(batch.data(), perBatch);
            total += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
        }
        g_out << QStringLiteral("потоков %1, пачка %2 слов: %3 мс на пачку, %4 млн слов/с, в таблице %5")
                     .arg(omp_get_max_threads()).arg(perBatch).arg(total / batches * 1e3, 0, 'f', 2)
                     .arg(perBatch * batches / total / 1e6, 0, 'f', 1).arg(table.size()) << Qt::endl;
        g_out.flush();
        return 0;
    }

    // --isd <файл|random:n,k[,seed]> <вес> [степень пропуска] [прогонов] [p Штерна] [l Штерна]
    const int isdAt = args.indexOf(QStringLiteral("--isd"));
    if (isdAt >= 0 && isdAt + 2 < args.size()) {
        const QString which = args.at(isdAt + 1);
        Isd::Code code;
        if (which.startsWith(QStringLiteral("random:"))) {
            const QStringList parts = which.mid(7).split(QLatin1Char(','));
            const int n = parts.value(0).toInt(), k = parts.value(1).toInt();
            const quint64 seed = parts.size() > 2 ? parts.at(2).toULongLong() : 1ULL;
            if (n <= 0 || k <= 0 || k >= n) { g_out << QStringLiteral("random:n,k — нужно 0 < k < n") << Qt::endl; return 2; }
            code = Isd::randomCode(n, k, seed);
        } else {
            RunConfig cfg;
            if (!loadMatrixOrCase(which, cfg)) return 2;
            code = Isd::fromRows(cfg.matrix);
        }
        const int maxWeight = args.at(isdAt + 2).toInt();
        const int missE     = isdAt + 3 < args.size() ? args.at(isdAt + 3).toInt() : 9;
        const int runs      = isdAt + 4 < args.size() ? args.at(isdAt + 4).toInt() : 3;
        const int sternP    = isdAt + 5 < args.size() ? args.at(isdAt + 5).toInt() : 0;
        const int sternL    = isdAt + 6 < args.size() ? args.at(isdAt + 6).toInt() : 0;
        const int rc = Isd::compare(g_out, code, maxWeight, missE > 0 ? missE : 9, runs > 0 ? runs : 3, sternP, sternL);
        g_out.flush();
        return rc;
    }

    const int dualAt = args.indexOf(QStringLiteral("--dual-run"));
    if (dualAt >= 0 && dualAt + 1 < args.size()) {
        const int upTo = dualAt + 2 < args.size() ? args.at(dualAt + 2).toInt() : 0;
        const QString device = dualAt + 3 < args.size() ? args.at(dualAt + 3).toLower() : QStringLiteral("cpu");
        const int rc = dualRun(args.at(dualAt + 1), upTo, device);
        g_out.flush();
        return rc;
    }

    const int leonAt = args.indexOf(QStringLiteral("--leon-run"));
    if (leonAt >= 0 && leonAt + 2 < args.size()) {
        const int missExp = leonAt + 3 < args.size() ? args.at(leonAt + 3).toInt() : 9;
        const QString device = leonAt + 4 < args.size() ? args.at(leonAt + 4).toLower() : QStringLiteral("cpu");
        const int rc = leonRun(args.at(leonAt + 1), args.at(leonAt + 2).toInt(), missExp > 0 ? missExp : 9, device);
        g_out.flush();
        return rc;
    }

    const int bzRunAt = args.indexOf(QStringLiteral("--bz-run"));
    if (bzRunAt >= 0 && bzRunAt + 2 < args.size()) {
        const QString device = bzRunAt + 3 < args.size() ? args.at(bzRunAt + 3).toLower() : QStringLiteral("gpu");
        const int rc = bzRun(args.at(bzRunAt + 1), args.at(bzRunAt + 2).toInt(), device);
        g_out.flush();
        return rc;
    }

    if (args.contains(QStringLiteral("--bench"))) {
        benchmark();
        g_out.flush();
        return 0;
    }

    const int sweepAt = args.indexOf(QStringLiteral("--sweep"));
    if (sweepAt >= 0 && sweepAt + 1 < args.size()) {
        const int rc = sweepLaunchParams(args.at(sweepAt + 1));
        g_out.flush();
        return rc;
    }

    // --probe <случай|файл матрицы> [<строк>]
    const int probeAt = args.indexOf(QStringLiteral("--probe"));
    if (probeAt >= 0 && probeAt + 1 < args.size()) {
        const int rows = probeAt + 2 < args.size() ? args.at(probeAt + 2).toInt() : 0;
        const int rc = probeOnly(args.at(probeAt + 1), rows);
        g_out.flush();
        return rc;
    }

    // --rate <случай|файл матрицы> <мс> [<строк>]
    const int rateAt = args.indexOf(QStringLiteral("--rate"));
    if (rateAt >= 0 && rateAt + 2 < args.size()) {
        const int rows = rateAt + 3 < args.size() ? args.at(rateAt + 3).toInt() : 0;
        const int rc = updateRate(args.at(rateAt + 1), args.at(rateAt + 2).toInt(), rows);
        g_out.flush();
        return rc;
    }

    const int tuneAt = args.indexOf(QStringLiteral("--tune"));
    if (tuneAt >= 0 && tuneAt + 1 < args.size()) {
        const int rc = tuneOnly(args.at(tuneAt + 1));
        g_out.flush();
        return rc;
    }

    const int profAt = args.indexOf(QStringLiteral("--profile"));
    if (profAt >= 0 && profAt + 1 < args.size()) {
        const int rc = runSingleForProfiling(args.at(profAt + 1));
        g_out.flush();
        return rc;
    }

    const int dumpAt = args.indexOf(QStringLiteral("--dump"));
    if (dumpAt >= 0 && dumpAt + 1 < args.size()) {
        const int rc = dumpGolden(args.at(dumpAt + 1));
        g_out.flush();
        return rc;
    }

    testShortCode(QStringLiteral("Хэмминг (7,4)"),
                  Reference::hamming7_4(), Reference::analyticHamming7_4());
    testShortCode(QStringLiteral("Расширенный Хэмминг (8,4)"),
                  Reference::extHamming8_4(), Reference::analyticExtHamming8_4());
    testShortCode(QStringLiteral("Голей (24,12)"),
                  Reference::golay24_12(), Reference::analyticGolay24_12());

    testPartialShort(Reference::golay24_12(), 4);

    // Ядро коротких кодов выбирается по числу слов в строке из заготовленного
    // набора и округляется вверх, а лишние слова дозаполняются нулями. Во всех
    // случаях выше число слов попадало в набор точно (1, 2, 5, 32), и путь с
    // округлением не проверялся ни разу.
    //
    // 700 бит это 11 слов, округляется до 12; 550 бит это 9 слов, до 10.
    // Матрицы намеренно узкие по строкам, чтобы работал наивный перебор.
    testShortCodeAgainstBruteForce(QStringLiteral("rnd(20,700), 11 слов -> 12"),
                                   Reference::randomMatrix(20, 700, 15));
    testShortCodeAgainstBruteForce(QStringLiteral("rnd(20,550), 9 слов -> 10"),
                                   Reference::randomMatrix(20, 550, 16));

    testLongCode(70, 3);
    testLongCode(64, 2);

    testDualCode(QStringLiteral("Хэмминг (7,4)"), Reference::hamming7_4());
    testDualCode(QStringLiteral("rnd(16,40)"), Reference::randomMatrix(16, 40, 24));
    testMacWilliams();
    testSpectrumCounts();

    testAxisLabelStep();
    testUpdateIntervals();
    testProbeLeavesNoTrace();
    testBrouwerZimmermann();
    testBrouwerZimmermannWorker();
    testBzPartialLayer();
    testLeonModel();
    testLeonWorker();
    testCyclicSymmetry();
    testLeonCyclic();
    testBchCode();
    testHammingCode();
    testProductCode();

    testSettingsCopy();
    testAutosaveStore();
    testCanResume();
    testExtendMaxRows();

    testAutoTunedGrid();

    testCheckpoints();
    testCheckpointPortability();
    testCancelMidChunk();
    testAutoTunedCheckpoints();
    testOversizedMatrixRejected();

    g_out << Qt::endl
          << QStringLiteral("итого: пройдено ") << g_passed << QStringLiteral(", провалено ") << g_failed << Qt::endl;
    return g_failed == 0 ? 0 : 1;
}
