// Харнесс проверки расчёта спектра.
//
// Гоняет Worker по всем вычислительным путям (CPU/GPU x Gray/XOR x short/long
// + дуальный код) и сверяет результат с эталоном. Требование к спектру
// абсолютное: расхождение даже на единицу — провал.
//
// Спектр снимается штатным сигналом updateSpectrumPTE: он несёт точный
// десятичный текст "вес - количество" и, в отличие от updateSpectrumPlot
// (float), не теряет разрядность на больших значениях.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QTextStream>

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <thread>

// worker.h тянет gmpxx.h, где есть std::numeric_limits<...>::min(). Его нужно
// разобрать до windows.h, иначе макросы min/max из windows.h ломают тело класса.
#include "worker.h"
#include "autosavestore.h"
#include "reference.h"
#include "ui/axisticks.h"
#include "ui/updateintervals.h"
#include "bz.h"
#include "leonkernel.cuh"
#include "productcode.h"

#ifdef Q_OS_WIN
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

using Reference::Spectrum;
using Algorithm     = ComputationSettings::Algorithm;
using ComputeDevice = ComputationSettings::ComputeDevice;
using EnumerationType = ComputationSettings::EnumerationType;

static QTextStream out(stdout);
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

static Spectrum parseSpectrumText(const SpectrumText& lines)
{
    Spectrum s;
    for (const QString& line : lines) {
        const QStringList parts = line.split(QStringLiteral(" - "));
        if (parts.size() != 2) continue;
        s[parts[0].toInt()] = parts[1].toULongLong();
    }
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
    ComputeDevice device   = ComputeDevice::CPU;
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
};

static ComputationSettings makeSettings(const RunConfig& cfg)
{
    ComputationSettings s;
    s.matrix        = cfg.matrix;
    s.algorithmType = cfg.algorithm;
    s.enumType      = ComputationSettings::Full;
    s.maxRows       = cfg.maxRows > 0 ? cfg.maxRows : cfg.matrix.size();
    s.bzWeight      = cfg.bzWeight > 0 ? cfg.bzWeight : 8;
    s.leonWeight    = cfg.leonWeight > 0 ? cfg.leonWeight : 24;
    s.leonMissExponent = cfg.leonMissExponent;
    s.matrix2       = cfg.matrix2;
    s.productWeight = cfg.productWeight;
    s.productRank   = cfg.productRank;
    s.productAlgorithm = int(cfg.productAlgorithm);
    s.compDev       = cfg.device;
    s.compDevSet.threadsCpu = cfg.threadsCpu;
    s.compDevSet.blocksGpu  = cfg.blocksGpu;
    s.compDevSet.threadsGpu = cfg.threadsGpu;
    s.autoTuneGrid          = cfg.autoTune;
    // Интервалы задраны так, чтобы за время теста чекпоинт не сработал:
    // сохранение состояния проверяется отдельными тестами.
    s.timeIntSet.saveSpectrumInterval   = 100000;
    s.timeIntSet.updateSpectrumInterval = 100000;
    return s;
}

// Автосохранения на время прогона складываются в свой каталог: боевой лежит
// в AppData пользователя, и топтать его тестами нельзя.
static QString autosaveRoot()
{
    static const QString dir =
        QDir::tempPath() + QStringLiteral("/SpectrumTests-autosave");
    return dir;
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

    QObject::connect(&worker, &Worker::updateSpectrumPTE,
                     [&captured](const SpectrumText& s) { captured = parseSpectrumText(s); });
    QObject::connect(&worker, &Worker::errorOccurred,
                     [&](const QString& m) { errored = true; errorMessage = m; });
    g_planSets = g_planRows = g_planExactUpTo = -1;
    QObject::connect(&worker, &Worker::planReady,
                     [](int sets, int rows, int exactUpTo) {
                         g_planSets = sets; g_planRows = rows; g_planExactUpTo = exactUpTo;
                     });
    g_searchDone = g_searchTotal = 0; g_searchMiss = -1.0; g_searchUnseen.clear();
    g_productText.clear(); g_productExactUpTo = -1;
    QObject::connect(&worker, &Worker::productPlan,
                     [](const QString& text, int exactUpTo) {
                         g_productText = text;
                         if (exactUpTo >= 0) g_productExactUpTo = exactUpTo;
                     });
    QObject::connect(&worker, &Worker::searchEstimate,
                     [](int, quint64 done, quint64 total, double miss, SpectrumFloat unseen) {
                         g_searchDone = done; g_searchTotal = total;
                         g_searchMiss = miss; g_searchUnseen = unseen;
                     });

    worker.setSettings(makeSettings(cfg).toJson());
    worker.setCheckpointOpsPolicy(checkpointEveryOps, stopAfterOps);
    // Тестовые матрицы мелкие, и в боевом режиме подбор на них не запустился
    // бы вовсе — тесты про подбор стали бы пустыми.
    worker.setGridTuningThreshold(0.0);
    worker.initializeRunState(loadMode);
    worker.computeSpectrum();

    if (errored) {
        out << QStringLiteral("      ошибка от Worker: ") << errorMessage << Qt::endl;
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
        maxRows = quint64(InfoSets::rowsForWeight(overlaps, weight, rows, cols));
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
    if (cfg.device == ComputeDevice::GPU && !g_gpuAvailable) {
        out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
        return;
    }

    const Spectrum actual = runWorker(cfg);
    const Spectrum want   = stripZeros(expected);

    if (actual == want) {
        ++g_passed;
        out << "  ok       " << name << Qt::endl;
        return;
    }

    ++g_failed;
    out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
    out << QStringLiteral("      ожидалось: ") << formatSpectrum(want)   << Qt::endl;
    out << QStringLiteral("      получено:  ") << formatSpectrum(actual) << Qt::endl;

    // Показываем расхождения поимённо — «отклонение даже на единицу» должно
    // быть видно сразу.
    QList<int> weights = want.keys();
    for (int w : actual.keys())
        if (!weights.contains(w)) weights << w;
    std::sort(weights.begin(), weights.end());
    for (int w : weights) {
        const quint64 e = want.value(w, 0), a = actual.value(w, 0);
        if (e != a)
            out << QStringLiteral("      вес ") << w << QStringLiteral(": ожидалось ") << e << QStringLiteral(", получено ") << a
                << QStringLiteral(" (разница ") << (qint64(a) - qint64(e)) << ")" << Qt::endl;
    }
}

// ------------------------------------------------------------------ сценарии

// Все пути, доступные для короткого кода, должны дать один и тот же спектр.
static void testShortCode(const QString& label, const QStringList& matrix,
                          const Spectrum& analytic)
{
    out << Qt::endl << label << " (k=" << matrix.size()
        << ", n=" << matrix.first().length() << ")" << Qt::endl;

    const Spectrum brute = Reference::bruteForce(matrix);

    // Сначала убеждаемся, что наивный перебор согласуется с аналитикой —
    // иначе эталону нельзя доверять.
    if (stripZeros(brute) != stripZeros(analytic)) {
        ++g_failed;
        out << QStringLiteral("  ПРОВАЛ   эталон: перебор разошёлся с аналитическим спектром")
            << Qt::endl
            << QStringLiteral("      аналитика: ") << formatSpectrum(stripZeros(analytic)) << Qt::endl
            << QStringLiteral("      перебор:   ") << formatSpectrum(stripZeros(brute))    << Qt::endl;
        return;
    }
    out << QStringLiteral("  ok       эталон: перебор == аналитический спектр") << Qt::endl;
    ++g_passed;

    RunConfig cfg; cfg.matrix = matrix;

    cfg.algorithm = Algorithm::SimpleXor; cfg.device = ComputeDevice::CPU;
    check("CPU  XOR  (короткий)", cfg, brute);
    cfg.device = ComputeDevice::GPU;
    check("GPU  XOR  (короткий)", cfg, brute);

    cfg.algorithm = Algorithm::GrayCode;  cfg.device = ComputeDevice::CPU;
    check("CPU  Грей (короткий)", cfg, brute);
    cfg.device = ComputeDevice::GPU;
    check("GPU  Грей (короткий)", cfg, brute);
}

// Сверка всех четырёх коротких путей с наивным перебором. Отдельно от
// testShortCode, где ещё требуется аналитический спектр: у произвольной матрицы
// его нет, но перебор остаётся абсолютным эталоном.
static void testShortCodeAgainstBruteForce(const QString& label, const QStringList& matrix)
{
    out << Qt::endl << label << QStringLiteral(" (k=") << matrix.size()
        << QStringLiteral(", n=") << matrix.first().length() << QStringLiteral(")") << Qt::endl;

    const Spectrum brute = Reference::bruteForce(matrix);

    RunConfig cfg; cfg.matrix = matrix;
    cfg.algorithm = Algorithm::SimpleXor; cfg.device = ComputeDevice::CPU;
    check("CPU  XOR ", cfg, brute);
    cfg.device = ComputeDevice::GPU;
    check("GPU  XOR ", cfg, brute);
    cfg.algorithm = Algorithm::GrayCode;  cfg.device = ComputeDevice::CPU;
    check("CPU  Грей", cfg, brute);
    cfg.device = ComputeDevice::GPU;
    check("GPU  Грей", cfg, brute);
}

// Длинный код: полный перебор невозможен, эталон — C(n,w) на единичной матрице.
static void testLongCode(int n, int maxRows)
{
    out << Qt::endl << QStringLiteral("Единичная матрица I(") << n << "), maxRows=" << maxRows
        << QStringLiteral(" — длинный путь") << Qt::endl;

    const Spectrum expected = Reference::identityPartialSpectrum(n, maxRows);

    RunConfig cfg;
    cfg.matrix    = Reference::identity(n);
    cfg.algorithm = Algorithm::SimpleXor;
    cfg.maxRows   = maxRows;

    cfg.device = ComputeDevice::CPU;
    check("CPU  XOR  (длинный)", cfg, expected);
    cfg.device = ComputeDevice::GPU;
    check("GPU  XOR  (длинный)", cfg, expected);
}

// Частичный перебор на коротком коде — сверяем с ограниченным перебором.
static void testPartialShort(const QStringList& matrix, int maxRows)
{
    out << Qt::endl << QStringLiteral("Частичный перебор, maxRows=") << maxRows << Qt::endl;

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

    cfg.device = ComputeDevice::CPU;
    check("CPU  XOR  частичный", cfg, expected);
    cfg.device = ComputeDevice::GPU;
    check("GPU  XOR  частичный", cfg, expected);
}

// Дуальный код: считается спектр проверочной матрицы, затем восстанавливается
// исходный через тождества Мак-Вильямс. Результат обязан совпасть с перебором.
static void testDualCode(const QString& label, const QStringList& matrix)
{
    out << Qt::endl << label << QStringLiteral(" — через дуальный код") << Qt::endl;

    const Spectrum brute = Reference::bruteForce(matrix);

    RunConfig cfg;
    cfg.matrix    = matrix;
    cfg.algorithm = Algorithm::DualCode;

    cfg.device = ComputeDevice::CPU;
    check("CPU  дуальный", cfg, brute);
    cfg.device = ComputeDevice::GPU;
    check("GPU  дуальный", cfg, brute);
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
    const bool needsGpu = cfg.device == ComputeDevice::GPU
                       || resumeCfg.device == ComputeDevice::GPU;
    if (needsGpu && !g_gpuAvailable) {
        out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
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
        out << QStringLiteral("  ПРОВАЛ   ") << name
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
        out << "  ok       " << name
            << QStringLiteral("  (обрыв на ") << saved << QStringLiteral(" оп.)") << Qt::endl;
        return;
    }

    ++g_failed;
    out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
    out << QStringLiteral("      целиком:      ") << formatSpectrum(whole)   << Qt::endl;
    out << QStringLiteral("      с прерыванием:") << formatSpectrum(resumed) << Qt::endl;

    QList<int> weights = whole.keys();
    for (int w : resumed.keys())
        if (!weights.contains(w)) weights << w;
    std::sort(weights.begin(), weights.end());
    for (int w : weights) {
        const quint64 e = whole.value(w, 0), a = resumed.value(w, 0);
        if (e != a)
            out << QStringLiteral("      вес ") << w << QStringLiteral(": целиком ") << e
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
    out << Qt::endl << QStringLiteral("Чекпоинты: прерывание и возобновление") << Qt::endl;

    // Голей (24,12): 4096 комбинаций, прерывания в разных точках.
    RunConfig golay;
    golay.matrix = Reference::golay24_12();

    golay.algorithm = Algorithm::SimpleXor;
    for (ComputeDevice dev : { ComputeDevice::CPU, ComputeDevice::GPU }) {
        golay.device = dev;
        const QString who = QStringLiteral("%1 XOR Голей")
                                .arg(dev == ComputeDevice::CPU ? QStringLiteral("CPU")
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
    for (ComputeDevice dev : { ComputeDevice::CPU, ComputeDevice::GPU }) {
        gray.device = dev;
        const QString who = QStringLiteral("%1 Грей I(22)")
                                .arg(dev == ComputeDevice::CPU ? QStringLiteral("CPU")
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
        layer.device = ComputeDevice::CPU;
        checkResume(QStringLiteral("CPU XOR, обрыв на границе слоя (%1)").arg(boundary),
                    layer, layer, boundary, boundary);
        layer.device = ComputeDevice::GPU;
        checkResume(QStringLiteral("GPU XOR, обрыв на границе слоя (%1)").arg(boundary),
                    layer, layer, boundary, boundary);
    }

    // Длинный код. При maxRows=3 всего 57 тыс. масок — это один чанк, обрывать
    // нечего. maxRows=5 даёт 13 млн, и чанков становится несколько. GPU-чанк
    // равен блоки*нити*4096, поэтому разбиение здесь намеренно мелкое.
    RunConfig lng = longConfig(ComputeDevice::CPU);
    checkResume(QStringLiteral("CPU XOR длинный, обрыв"), lng, lng, 3000000, 3000000);
    lng = longConfig(ComputeDevice::GPU);
    checkResume(QStringLiteral("GPU XOR длинный, обрыв"), lng, lng, 3000000, 3000000);

    // Длинный путь на видеокарте проверяется отдельно и подробнее.
    //
    // Там подготовка стартовых масок идёт в двойной буфер, а синхронизации
    // после каждого чанка больше нет — хост убегает вперёд и успевает
    // поставить в очередь несколько ядер. Чекпоинт обязан отражать реально
    // посчитанное, а не поставленное в очередь: saveGpuCheckpoint для этого
    // сначала дожидается потока. Если бы не дожидался, сохранённый спектр
    // отставал бы от chunkOffset, и возобновление потеряло бы часть слов.
    const RunConfig lgpu = longConfig(ComputeDevice::GPU);
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

// Ключевая проверка: чекпоинт обязан переноситься между разными
// конфигурациями железа. Точка возобновления хранится как абсолютный индекс
// (ранг сочетания либо номер маски Грея), а не как номер чанка, поэтому смена
// числа потоков, блоков и даже устройства не должна ни на что влиять.
static void testCheckpointPortability()
{
    out << Qt::endl
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
    from = base;              from.device = ComputeDevice::GPU;
    from.blocksGpu = 64;      from.threadsGpu = 256;
    other = base;             other.device = ComputeDevice::GPU;
    other.blocksGpu = 8;      other.threadsGpu = 64;
    checkResume(QStringLiteral("GPU 64x256 -> GPU 8x64"), from, other, 1000, 1000);

    // Смена устройства в обе стороны.
    from = base;  from.device  = ComputeDevice::CPU;
    other = base; other.device = ComputeDevice::GPU;
    checkResume(QStringLiteral("CPU -> GPU"), from, other, 1000, 1000);

    from = base;  from.device  = ComputeDevice::GPU;
    other = base; other.device = ComputeDevice::CPU;
    checkResume(QStringLiteral("GPU -> CPU"), from, other, 1000, 1000);

    // То же самое на коде Грея — на матрице, которая режется на чанки.
    from = RunConfig();  from.matrix = Reference::identity(22);
    from.algorithm = Algorithm::GrayCode; from.device = ComputeDevice::CPU;
    other = from;        other.device = ComputeDevice::GPU;
    checkResume(QStringLiteral("Грей: CPU -> GPU"), from, other, 2000000, 2000000);
    checkResume(QStringLiteral("Грей: GPU -> CPU"), other, from, 2000000, 2000000);

    // Длинный код: смена устройства в обе стороны.
    const RunConfig lngCpu = longConfig(ComputeDevice::CPU);
    const RunConfig lngGpu = longConfig(ComputeDevice::GPU);
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
    worker.setSettings(makeSettings(tuned).toJson());
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
        out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
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
        out << QStringLiteral("  ПРОВАЛ   ") << name
            << QStringLiteral("  — подбор не сработал, проверять нечего") << Qt::endl;
        return;
    }

    if (actual == expected && !expected.isEmpty()) {
        ++g_passed;
        out << "  ok       " << name << gridText << Qt::endl;
        return;
    }

    ++g_failed;
    out << QStringLiteral("  ПРОВАЛ   ") << name << gridText << Qt::endl;
    out << QStringLiteral("      без подбора: ") << formatSpectrum(expected) << Qt::endl;
    out << QStringLiteral("      с подбором:  ") << formatSpectrum(actual)   << Qt::endl;
}

static void testAutoTunedGrid()
{
    out << Qt::endl << QStringLiteral("Автоподбор сетки") << Qt::endl;

    RunConfig cfg;
    cfg.device = ComputeDevice::GPU;

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
    ident.device    = ComputeDevice::GPU;
    ident.matrix    = Reference::identity(20);
    ident.algorithm = Algorithm::SimpleXor;
    ident.autoTune  = true;
    check(QStringLiteral("подбор: I(20) против биномов"), ident,
          Reference::identityPartialSpectrum(20, 20));

    // Длинный путь. Там сетка задаёт ещё и размер чанка, то есть разбиение
    // расчёта — тем важнее убедиться, что спектр от неё не зависит.
    checkTuned(QStringLiteral("длинный код (k=70)"), longConfig(ComputeDevice::GPU));

    if (!g_gpuAvailable)
        return;

    // На CPU подбирать нечего.
    RunConfig cpu;
    cpu.matrix    = Reference::golay24_12();
    cpu.algorithm = Algorithm::SimpleXor;
    cpu.device    = ComputeDevice::CPU;
    const QPair<int, int> cpuGrid = tunedGridFor(cpu);
    if (cpuGrid.first == 0) {
        ++g_passed;
        out << "  ok       " << QStringLiteral("CPU: подбор не применяется") << Qt::endl;
    } else {
        ++g_failed;
        out << QStringLiteral("  ПРОВАЛ   CPU: подбор вмешался") << Qt::endl;
    }
}

// Подбор гоняется заново при каждом запуске и может выбрать другую сетку,
// чем в прошлый раз. Значит, чекпоинт обязан переноситься и через него.
static void testAutoTunedCheckpoints()
{
    out << Qt::endl << QStringLiteral("Автоподбор: перенос чекпоинтов") << Qt::endl;

    RunConfig base;
    base.matrix    = Reference::golay24_12();
    base.algorithm = Algorithm::SimpleXor;
    base.device    = ComputeDevice::GPU;

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
    lngPlain.device     = ComputeDevice::GPU;
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
    gray.device    = ComputeDevice::GPU;
    RunConfig grayTuned = gray;  grayTuned.autoTune = true;
    checkResume(QStringLiteral("Грей: подбор -> подбор"), grayTuned, grayTuned, 2000000, 2000000);
    checkResume(QStringLiteral("Грей: подбор -> без подбора"), grayTuned, gray, 2000000, 2000000);
}

// ------------------------------------------------- хранилище автосохранений

static void expectStore(const QString& name, bool condition)
{
    if (condition) { ++g_passed; out << "  ok       " << name << Qt::endl; }
    else           { ++g_failed; out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl; }
}

static void testAutosaveStore()
{
    out << Qt::endl << QStringLiteral("Хранилище автосохранений") << Qt::endl;

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
    out << Qt::endl << QStringLiteral("Годность автосохранения") << Qt::endl;

    ComputationSettings settings;
    settings.algorithmType = Algorithm::SimpleXor;
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
    settings.algorithmType = Algorithm::GrayCode;
    record.algorithm = Algorithm::GrayCode;
    record.state.rOffset = 0;
    expectStore(QStringLiteral("код Грея годится независимо от maxRows"),
                canResume(record, settings));
}

// Досчёт: посчитать до maxRows = n, потом попросить n + 1 и сверить с прямым
// расчётом до n + 1. Ради этого запись и не удаляется после успеха.
static void checkExtend(const QString& name, RunConfig cfg, int from, int to)
{
    if (cfg.device == ComputeDevice::GPU && !g_gpuAvailable) {
        out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
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
        out << QStringLiteral("  ПРОВАЛ   ") << name
            << QStringLiteral("  — после успешного расчёта записи не осталось,"
                              " досчитывать не с чего") << Qt::endl;
        clearCheckpoints();
        return;
    }

    const Spectrum extended = runWorker(target, LoadMode::FromCheckpoint);
    clearCheckpoints();

    if (extended == direct) {
        ++g_passed;
        out << "  ok       " << name
            << QStringLiteral("  (досчитано с ") << saved << QStringLiteral(" оп.)") << Qt::endl;
        return;
    }

    ++g_failed;
    out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
    out << QStringLiteral("      напрямую: ") << formatSpectrum(direct)   << Qt::endl;
    out << QStringLiteral("      досчётом: ") << formatSpectrum(extended) << Qt::endl;
}

static void testExtendMaxRows()
{
    out << Qt::endl << QStringLiteral("Досчёт до большего числа строк") << Qt::endl;

    RunConfig cfg;
    cfg.matrix     = Reference::identity(20);
    cfg.algorithm  = Algorithm::SimpleXor;
    cfg.device     = ComputeDevice::CPU;
    cfg.threadsCpu = 4;
    checkExtend(QStringLiteral("CPU I(20): 5 строк, потом 7"), cfg, 5, 7);

    cfg.device = ComputeDevice::GPU;
    checkExtend(QStringLiteral("GPU I(20): 5 строк, потом 7"), cfg, 5, 7);

    // Длинный путь: там своё разбиение на чанки и свои слои.
    RunConfig lng;
    lng.matrix     = Reference::identity(70);
    lng.algorithm  = Algorithm::SimpleXor;
    lng.device     = ComputeDevice::GPU;
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
    out << Qt::endl << QStringLiteral("Подписи оси графика") << Qt::endl;

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
            out << QStringLiteral("      %1: ожидался 0, получено %2")
                       .arg(QString::fromUtf8(c.what)).arg(step) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; out << "  ok       " << QStringLiteral("вырожденные размеры дают ноль") << Qt::endl; }
    else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   вырожденные размеры") << Qt::endl; }

    // На любых рабочих размерах шаг обязан лежать в [1, size] — только тогда
    // цикл по подписям заканчивается.
    ok = true;
    for (int size : { 1, 2, 25, 50, 300, 2049 }) {
        for (int width : { 1, 2, 7, 31, 200, 900, 4000, 100000 }) {
            const int step = axisLabelStep(size, width, 30.0);
            if (step < 1 || step > size) {
                ok = false;
                out << QStringLiteral("      size=%1 width=%2 -> шаг %3")
                           .arg(size).arg(width).arg(step) << Qt::endl;
            }
        }
    }
    if (ok) { ++g_passed; out << "  ok       " << QStringLiteral("шаг всегда в пределах [1, длина спектра]") << Qt::endl; }
    else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   шаг вне пределов") << Qt::endl; }

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
                out << QStringLiteral("      size=%1 width=%2 -> %3 подписей")
                           .arg(size).arg(width).arg(labels) << Qt::endl;
            }
        }
    }
    if (ok) { ++g_passed; out << "  ok       " << QStringLiteral("цикл подписей заканчивается") << Qt::endl; }
    else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   цикл подписей") << Qt::endl; }
}

// Матрица шире MAX_COLS не влезает в фиксированные массивы ядер. Раньше предел
// проверялся только в интерфейсе, и вызов Worker напрямую — как здесь —
// приводил к записи за границу codeword[] прямо на видеокарте, молча.
// Теперь запуск ядра обязан отказаться с сообщением.
static void testOversizedMatrixRejected()
{
    out << Qt::endl << QStringLiteral("Защита от матрицы сверх предела") << Qt::endl;

    if (!g_gpuAvailable) {
        out << QStringLiteral("  ПРОПУСК  (GPU недоступен)") << Qt::endl;
        return;
    }

    const int tooWide = Constants::MAX_COLS + 1;   // 2049 -> wordsPerRow = 33 > 32
    RunConfig cfg;
    cfg.matrix    = QStringList{ QString(tooWide, QLatin1Char('1')),
                                 QString(tooWide, QLatin1Char('0')) };
    cfg.algorithm = Algorithm::GrayCode;
    cfg.device    = ComputeDevice::GPU;

    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());
    bool errored = false;
    QObject::connect(&worker, &Worker::errorOccurred,
                     [&errored](const QString&) { errored = true; });

    worker.setSettings(makeSettings(cfg).toJson());
    worker.initializeRunState(LoadMode::Reset);
    worker.computeSpectrum();

    if (errored) {
        ++g_passed;
        out << QStringLiteral("  ok       матрица шириной ") << tooWide
            << QStringLiteral(" отклонена с ошибкой") << Qt::endl;
    } else {
        ++g_failed;
        out << QStringLiteral("  ПРОВАЛ   матрица шириной ") << tooWide
            << QStringLiteral(" принята — ядро пишет за границу массива") << Qt::endl;
    }

    // Код Грея перебирает 2^k масок в 64-битном слове: больше 63 строк для него
    // недопустимо. Проверялось только в диалоге настроек, а прямой вызов давал
    // сдвиг на 64 и больше.
    for (ComputeDevice dev : { ComputeDevice::CPU, ComputeDevice::GPU }) {
        RunConfig gray;
        gray.matrix    = Reference::identity(70);
        gray.algorithm = Algorithm::GrayCode;
        gray.device    = dev;

        Worker w;
        bool err = false;
        QObject::connect(&w, &Worker::errorOccurred, [&err](const QString&) { err = true; });
        w.setSettings(makeSettings(gray).toJson());
        w.initializeRunState(LoadMode::Reset);
        w.computeSpectrum();

        const QString dn = dev == ComputeDevice::CPU ? QStringLiteral("CPU")
                                                     : QStringLiteral("GPU");
        if (err) {
            ++g_passed;
            out << QStringLiteral("  ok       ") << dn
                << QStringLiteral(" Грей на 70 строках отклонён") << Qt::endl;
        } else {
            ++g_failed;
            out << QStringLiteral("  ПРОВАЛ   ") << dn
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
    cfg.device    = ComputeDevice::GPU;

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
        out << QStringLiteral("ожидалось --profile ident|rand|wide|maxshared|bigwide|biglong|graywide")
            << Qt::endl;
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const Spectrum s = runWorker(cfg);
    const double sec = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - t0).count();

    quint64 total = 0;
    for (auto it = s.constBegin(); it != s.constEnd(); ++it) total += it.value();
    out << which << QStringLiteral(": %1 с, слов %2, ненулевых весов %3")
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
    for (ComputeDevice d : { ComputeDevice::CPU, ComputeDevice::GPU }) {
        const bool cpu = (d == ComputeDevice::CPU);
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
    add("GPU XOR rnd(45,80) r<=9",  m45, Algorithm::SimpleXor,  9, ComputeDevice::GPU);
    add("GPU XOR rnd(50,100) r<=10", m50, Algorithm::SimpleXor, 10, ComputeDevice::GPU);
    add("GPU XOR rnd(52,120) r<=11", m52, Algorithm::SimpleXor, 11, ComputeDevice::GPU);
    // Другое разбиение: результат обязан не зависеть от числа блоков и нитей.
    add("GPU XOR rnd(50,100) r<=10 (8x64)", m50, Algorithm::SimpleXor, 10,
        ComputeDevice::GPU, 8, 64);

    // Единичные матрицы: I(n) порождает вообще все слова, поэтому спектр равен
    // C(n,w) точно, а при частичном переборе — C(n,w) для w <= maxRows.
    // На случайных матрицах сверять можно только количество слов; здесь
    // проверяется всё распределение целиком, без всякого предыдущего прогона.
    for (ComputeDevice d : { ComputeDevice::CPU, ComputeDevice::GPU }) {
        const bool cpu = (d == ComputeDevice::CPU);
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
        ComputeDevice::GPU, 64, 256, Reference::identityPartialSpectrum(50, 10));

    // Длинный путь (k > 63) с аналитически известным распределением: I(70)
    // порождает все слова, поэтому спектр равен C(70,w) для каждого веса.
    // 144 млн комбинаций — на таком объёме гонка в подготовке стартовых масок
    // проявилась бы перекосом весов, а не только недостачей в сумме.
    add("GPU XOR I(70) r<=6", Reference::identity(70), Algorithm::SimpleXor, 6,
        ComputeDevice::GPU, 64, 256, Reference::identityPartialSpectrum(70, 6));
    add("CPU XOR I(70) r<=6", Reference::identity(70), Algorithm::SimpleXor, 6,
        ComputeDevice::CPU, 64, 256, Reference::identityPartialSpectrum(70, 6));
    // То же при мелком разбиении: чанков становится много, значит много и
    // перекладываний буфера стартовых масок.
    add("GPU XOR I(70) r<=6 (8x32)", Reference::identity(70), Algorithm::SimpleXor, 6,
        ComputeDevice::GPU, 8, 32, Reference::identityPartialSpectrum(70, 6));
    // Длинный путь + матрица в ГЛОБАЛЬНОЙ памяти + округление числа слов —
    // сочетание, которого не было ни в одном случае, и оно скрыло настоящий
    // баг: в запасной ветке цикл шёл до округлённого числа слов, а шаг строки
    // в глобальной памяти настоящий, и лишние слова читались из начала
    // следующей строки.
    //
    // I(900): матрица 900 x 15 слов = 108 КБ, в разделяемую (40 КБ) не влезает;
    // 15 слов округляются до 16. Спектр при этом известен точно — C(900,w).
    add("GPU XOR I(900) r<=3, глобальная память", Reference::identity(900),
        Algorithm::SimpleXor, 3, ComputeDevice::GPU, 64, 256,
        Reference::identityPartialSpectrum(900, 3));

    // Длинный путь на случайной матрице, 670 млн комбинаций.
    add("GPU XOR rnd(90,300) r<=6", Reference::randomMatrix(90, 300, 14),
        Algorithm::SimpleXor, 6, ComputeDevice::GPU);
    // Тот же расчёт при мелком разбиении — распределение обязано не измениться.
    add("GPU XOR I(50) r<=10 (8x64)", Reference::identity(50), Algorithm::SimpleXor, 10,
        ComputeDevice::GPU, 8, 64, Reference::identityPartialSpectrum(50, 10));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        out << QStringLiteral("не удалось открыть ") << path << Qt::endl;
        return 1;
    }
    QTextStream fs(&f);
    fs.setCodec("UTF-8");

    for (const Case& c : cases) {
        if (c.cfg.device == ComputeDevice::GPU && !g_gpuAvailable) {
            out << QStringLiteral("  ПРОПУСК  ") << c.name << Qt::endl;
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

        out << QStringLiteral("  %1  %2 с, слов %3%4")
                   .arg(c.name, -34).arg(sec, 0, 'f', 2).arg(totalWords).arg(verdict)
            << Qt::endl;
        out.flush();
    }
    f.close();
    out << Qt::endl
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
    base.device = ComputeDevice::GPU;

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
        out << QStringLiteral("GPU недоступен") << Qt::endl;
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

    worker.setSettings(makeSettings(cfg).toJson());
    worker.measureUpdateRate();
    if (stopper.joinable())
        stopper.join();

    out << Qt::endl
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

    QFile file(which);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        out << QStringLiteral("не открыть матрицу: ") << which << Qt::endl;
        return false;
    }
    cfg = RunConfig();
    cfg.device    = ComputeDevice::GPU;
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
        out << QStringLiteral("GPU недоступен") << Qt::endl;
        return 1;
    }
    clearCheckpoints();

    RunConfig cfg;
    if (!loadMatrixOrCase(which, cfg))
        return 2;
    if (rows > 0) cfg.maxRows = rows;

    ComputationSettings settings = makeSettings(cfg);
    settings.timeIntSet.updateSpectrumInterval = intervalMs;

    Worker worker;
    worker.setAutosaveRoot(autosaveRoot());

    // Соединение прямое: обработчик выполняется в потоке расчёта ровно в
    // момент отправки, без очереди событий.
    QVector<double> stamps;
    const auto started = std::chrono::steady_clock::now();
    QObject::connect(&worker, &Worker::updateSpectrumPTE,
                     [&stamps, started](const SpectrumText&) {
        stamps.append(std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started).count());
    });

    worker.setSettings(settings.toJson());
    worker.setGridTuningThreshold(0.0);
    worker.initializeRunState(LoadMode::Reset);
    worker.computeSpectrum();

    const double total = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started).count();

    out << Qt::endl
        << QStringLiteral("матрица %1 x %2, строк в переборе %3, сетка %4 x %5")
               .arg(cfg.matrix.isEmpty() ? 0 : cfg.matrix.first().size())
               .arg(cfg.matrix.size()).arg(cfg.maxRows)
               .arg(cfg.blocksGpu).arg(cfg.threadsGpu) << Qt::endl
        << QStringLiteral("интервал в настройках: %1 мс").arg(intervalMs) << Qt::endl
        << QStringLiteral("расчёт занял %1 с, отправок спектра %2")
               .arg(total / 1000.0, 0, 'f', 2).arg(stamps.size()) << Qt::endl;

    if (stamps.size() < 2) {
        out << QStringLiteral("отправок слишком мало, увеличьте число строк") << Qt::endl;
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
    out << QStringLiteral("паузы подряд, мс: ") << shown.join(QStringLiteral(" ")) << Qt::endl;

    const double sum = std::accumulate(gaps.begin(), gaps.end(), 0.0);
    out << QStringLiteral("пауза между отправками: медиана %1 мс, среднее %2 мс, "
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
        out << QStringLiteral("ожидалось --tune wide|narrow|gray|long") << Qt::endl;
        return 2;
    }
    if (!g_gpuAvailable) {
        out << QStringLiteral("GPU недоступен") << Qt::endl;
        return 1;
    }

    out << QStringLiteral("Подбор сетки, случай ") << which << Qt::endl;
    out.flush();

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

    out << Qt::endl
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
        out << QStringLiteral("ожидалось --sweep wide|narrow|gray|long") << Qt::endl;
        return 2;
    }

    if (!g_gpuAvailable) {
        out << QStringLiteral("GPU недоступен") << Qt::endl;
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

    out << QStringLiteral("Перебор параметров, случай ") << which
        << QStringLiteral(" (%1 мультипроцессоров)").arg(smCount) << Qt::endl << Qt::endl;
    out << QStringLiteral("блоки \ нити");
    for (int t : threads) out << QStringLiteral("%1").arg(t, 9);
    out << Qt::endl;

    double best = 1e9;
    int bestB = 0, bestT = 0;
    Spectrum reference;

    for (int b : blocks) {
        out << QStringLiteral("%1").arg(b, 12);
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
            out << QStringLiteral("%1").arg(ok ? QStringLiteral("%1").arg(sec, 0, 'f', 2)
                                               : QStringLiteral("ПЛОХО"), 9);
            out.flush();
        }
        out << Qt::endl;
    }

    out << Qt::endl
        << QStringLiteral("лучшее: %1 блоков x %2 нитей, %3 с")
               .arg(bestB).arg(bestT).arg(best, 0, 'f', 2) << Qt::endl;
    // Для сравнения — то, что стоит по умолчанию сейчас.
    RunConfig cur = base; cur.blocksGpu = 64; cur.threadsGpu = 256;
    const auto t0 = std::chrono::steady_clock::now();
    const Spectrum s = runWorker(cur);
    const double sec = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - t0).count();
    out << QStringLiteral("сейчас по умолчанию 64 x 256: %1 с, выигрыш %2x")
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
    out.flush();
    const QPair<int, int> chosen = tunedGridFor(base, true);

    out << QStringLiteral("автоподбор выбрал %1 x %2: %3 с, выигрыш %4x, "
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
    for (ComputeDevice dev : { ComputeDevice::CPU, ComputeDevice::GPU }) {
        RunConfig c;
        c.matrix    = Reference::identity(28);
        c.algorithm = Algorithm::GrayCode;
        c.device    = dev;
        cases.append({ QStringLiteral("%1 Грей I(28)")
                           .arg(dev == ComputeDevice::CPU ? QStringLiteral("CPU")
                                                          : QStringLiteral("GPU")),
                       c, 1ULL << 28 });
    }

    // Простой XOR по слоям: самый нагруженный режим короткого пути.
    for (ComputeDevice dev : { ComputeDevice::CPU, ComputeDevice::GPU }) {
        RunConfig c;
        c.matrix    = Reference::identity(40);
        c.algorithm = Algorithm::SimpleXor;
        c.maxRows   = 7;
        c.device    = dev;
        quint64 total = 0;
        for (quint64 r = 0; r <= 7; ++r) total += Reference::binom(40, r);
        cases.append({ QStringLiteral("%1 XOR I(40) maxRows=7")
                           .arg(dev == ComputeDevice::CPU ? QStringLiteral("CPU")
                                                          : QStringLiteral("GPU")),
                       c, total });
    }

    out << QStringLiteral("Замер скорости") << Qt::endl;
    for (const Case& c : cases) {
        if (c.cfg.device == ComputeDevice::GPU && !g_gpuAvailable) {
            out << QStringLiteral("  ПРОПУСК  ") << c.name << Qt::endl;
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const Spectrum s = runWorker(c.cfg);
        const double sec = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - t0).count();

        quint64 got = 0;
        for (auto it = s.constBegin(); it != s.constEnd(); ++it) got += it.value();

        out << QStringLiteral("  %1: %2 с, %3 млн масок/с%4")
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
    out << Qt::endl << QStringLiteral("Достижимые интервалы обновления") << Qt::endl;

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
            out << QStringLiteral("      %1: %2 мс при потолке %3")
                       .arg(QString::fromUtf8(c.what)).arg(c.ms).arg(c.rate) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; out << "  ok       " << QStringLiteral("достижимость интервала") << Qt::endl; }
    else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   достижимость интервала") << Qt::endl; }

    // Секунда обязана оставаться достижимой на любом замере, который вообще
    // что-то поймал: на ней стоит заблокированный список.
    ok = true;
    for (double rate : { 1.0, 2.0, 6.9, 13.7, 54.6 }) {
        if (!intervalReachable(1000, rate)) {
            ok = false;
            out << QStringLiteral("      потолок %1 не пускает секунду").arg(rate) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; out << "  ok       " << QStringLiteral("секунда достижима на любом пойманном замере") << Qt::endl; }
    else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   секунда недостижима") << Qt::endl; }

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
            out << QStringLiteral("      %1: ожидалось %2, получено %3")
                       .arg(QString::fromUtf8(p.what)).arg(p.want).arg(got) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; out << "  ok       " << QStringLiteral("выбор самого частого допустимого") << Qt::endl; }
    else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   выбор допустимого") << Qt::endl; }

    // Что бы ни вернул fastestAllowed, это обязано быть достижимо: иначе
    // подрезка сама поставила бы пункт, который не работает.
    ok = true;
    for (double rate : { 0.1, 0.9, 1.0, 3.3, 6.9, 20.0, 54.6, 1000.0 }) {
        const int got = fastestAllowed(list, rate);
        if (got != 0 && !intervalReachable(got, rate)) {
            ok = false;
            out << QStringLiteral("      потолок %1 -> %2 мс, а это недостижимо")
                       .arg(rate).arg(got) << Qt::endl;
        }
    }
    if (ok) { ++g_passed; out << "  ok       " << QStringLiteral("выбранный интервал всегда достижим") << Qt::endl; }
    else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   выбран недостижимый интервал") << Qt::endl; }
}

// Проба потолка не имеет права трогать состояние расчёта: она идёт по тем же
// путям, что и настоящий расчёт, но с последнего слоя и без сохранений. Если
// бы она писала автосохранение, спектр в нём был бы огрызком — только
// последний слой, — и продолжение с него дало бы завышенный ответ.
static void testProbeLeavesNoTrace()
{
    out << Qt::endl << QStringLiteral("Замер потолка обновления") << Qt::endl;
    if (!g_gpuAvailable) {
        out << QStringLiteral("  пропуск  GPU недоступен") << Qt::endl;
        return;
    }
    RunConfig cfg;
    cfg.device    = ComputeDevice::GPU;
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
    QObject::connect(&worker, &Worker::updateSpectrumPTE,
                     [&spectraSent](const SpectrumText&) { ++spectraSent; });
    QObject::connect(&worker, &Worker::updateRateMeasured,
                     [&measured](double perSecond) { measured = perSecond; });
    // В приложении пробу останавливает таймер интерфейса. Здесь цикла событий
    // нет, поэтому обрываем её сразу по сигналу о старте: замер выйдет грубым,
    // а проверяется тут не он, а следы.
    QObject::connect(&worker, &Worker::updateRateProbeStarted,
                     [&worker]() { worker.cancel(); });

    worker.setSettings(makeSettings(cfg).toJson());
    worker.measureUpdateRate();

    if (measured >= 0.0) { ++g_passed; out << "  ok       " << QStringLiteral("замер отдал результат") << Qt::endl; }
    else { ++g_failed; out << QStringLiteral("  ПРОВАЛ   сигнала с результатом не было") << Qt::endl; }

    if (spectraSent == 0) { ++g_passed; out << "  ok       " << QStringLiteral("спектр в интерфейс не уходил") << Qt::endl; }
    else { ++g_failed; out << QStringLiteral("  ПРОВАЛ   проба отправила спектров: %1").arg(spectraSent) << Qt::endl; }

    AutosaveRecord record;
    if (!testStore().load(cfg.matrix, cfg.algorithm, record)) {
        ++g_passed; out << "  ok       " << QStringLiteral("автосохранение не тронуто") << Qt::endl;
    } else {
        ++g_failed; out << QStringLiteral("  ПРОВАЛ   проба записала автосохранение") << Qt::endl;
    }

    // И главное: после пробы обычный расчёт даёт тот же спектр, что и без неё.
    // Сравнение идёт с прогоном на чистом месте, а не с эталонной формулой:
    // проверяется здесь не правильность расчёта — на неё есть свои тесты, — а
    // то, что проба ничего за собой не оставила.
    const Spectrum after = runWorker(cfg);
    if (!expected.isEmpty() && after == expected) {
        ++g_passed; out << "  ok       " << QStringLiteral("расчёт после пробы даёт верный спектр") << Qt::endl;
    } else {
        ++g_failed; out << QStringLiteral("  ПРОВАЛ   спектр после пробы разошёлся с эталоном") << Qt::endl;
    }
}

// ------------------------------------------------ Брауэр–Циммерман, прототип

struct BZCase
{
    QString     name;
    QStringList rows;
    int         r;
    int         sets;
};

static QVector<BZCase> bzCases()
{
    return {
        { QStringLiteral("Голей [24,12], как есть"),       Reference::golay24_12(),                              4, 2 },
        { QStringLiteral("случайный [40,16], перемешан"),  BZ::scramble(BZ::systematicRandom(16, 40, 7), 11),   4, 3 },
        { QStringLiteral("случайный [60,20], перемешан"),  BZ::scramble(BZ::systematicRandom(20, 60, 3), 5),    4, 3 },
        { QStringLiteral("случайный [96,24], перемешан"),  BZ::scramble(BZ::systematicRandom(24, 96, 9), 2),    5, 4 },
    };
}

// Печатает низ спектра тремя способами: точный, по Брауэру–Циммерману и так,
// как перебирает программа сейчас — комбинациями строк введённой матрицы.
//
// Запуск: SpectrumTests.exe --bz
static void bzReport()
{
    for (const BZCase& c : bzCases()) {
        const int k = c.rows.size();
        const int n = c.rows.first().length();

        const Reference::Spectrum exact = Reference::bruteForce(c.rows);
        const BZ::Result          bz    = BZ::run(c.rows, c.r, c.sets);
        const Reference::Spectrum naive = BZ::naivePartial(c.rows, c.r);

        QStringList overlaps;
        for (int o : bz.overlaps) overlaps << QString::number(o);

        out << Qt::endl
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
            out << QStringLiteral("   %1  %2  %3  %4%5")
                       .arg(w, 3).arg(e, 10).arg(b, 8).arg(v, 10).arg(mark) << Qt::endl;
        }
    }
    out << Qt::endl;
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

    out << QStringLiteral("[%1,%2], до %3 строк, множеств до %4").arg(n).arg(k).arg(r).arg(maxSets) << Qt::endl;
    out.flush();

    auto t = std::chrono::steady_clock::now();
    const BZ::Result bz = BZ::run(cfg.matrix, r, maxSets);
    const double bzSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();

    t = std::chrono::steady_clock::now();
    const Reference::Spectrum naive = BZ::naivePartial(cfg.matrix, r);
    const double naiveSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();

    QStringList overlaps;
    for (int o : bz.overlaps) overlaps << QString::number(o);

    out << QStringLiteral("множеств %1 (перекрытия: %2), перебрано %3 слов за %4 с; как сейчас — за %5 с")
               .arg(bz.sets).arg(overlaps.join(QStringLiteral(", "))).arg(bz.enumerated)
               .arg(bzSec, 0, 'f', 1).arg(naiveSec, 0, 'f', 1) << Qt::endl
        << QStringLiteral("гарантия: все слова веса < %1 найдены").arg(bz.guaranteedBelow) << Qt::endl;
    if (bz.rejectedOverlap >= 0)
        out << QStringLiteral("следующее множество перекрыло бы прежние на %1 столбцов — не взято")
                   .arg(bz.rejectedOverlap) << Qt::endl;
    out << QStringLiteral("   вес        БЦ    как сейчас") << Qt::endl;

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
        out << QStringLiteral("   %1  %2  %3%4")
                   .arg(w, 3).arg(bz.spectrum.value(w, 0), 8).arg(naive.value(w, 0), 10).arg(mark) << Qt::endl;
        ++shown;
    }
    out << QStringLiteral("минимальный найденный вес: БЦ %1, как сейчас %2").arg(minBz).arg(minNaive) << Qt::endl;
    return 0;
}

// Брауэр–Циммерман в самом расчёте: ниже гарантии спектр обязан совпасть с
// точным побитово, выше — не превышать его. Заодно проверяется правило
// единственности: при весе во всю длину кода перебирается всё, и каждое слово
// должно быть засчитано ровно один раз — спектр совпадает с точным целиком.
static Spectrum checkBzExact(const QString& name, const RunConfig& cfg, const Spectrum& exact)
{
    if (cfg.device == ComputeDevice::GPU && !g_gpuAvailable) {
        out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
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
    if (ok) { ++g_passed; out << "  ok       " << what << Qt::endl; }
    else {
        ++g_failed;
        out << QStringLiteral("  ПРОВАЛ   ") << what << Qt::endl;
        for (const QString& line : problems)
            out << QStringLiteral("      ") << line << Qt::endl;
    }
    return actual;
}

// Два расчёта по одним множествам обязаны совпасть целиком — и ниже гарантии,
// и выше: найденное множество слов у них одно и то же.
static void expectSame(const QString& name, const Spectrum& a, const Spectrum& b)
{
    if (a.isEmpty() || b.isEmpty())
        return;   // один из расчётов пропущен или провалился — уже отмечено
    if (a == b) { ++g_passed; out << "  ok       " << name << Qt::endl; return; }
    ++g_failed;
    out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl
        << QStringLiteral("      CPU: ") << formatSpectrum(a) << Qt::endl
        << QStringLiteral("      GPU: ") << formatSpectrum(b) << Qt::endl;
}

// Досчёт до большего веса: запись после расчёта до w1 — начало расчёта до w2.
static void checkExtendBz(const QString& name, RunConfig cfg, int from, int to)
{
    if (cfg.device == ComputeDevice::GPU && !g_gpuAvailable) {
        out << QStringLiteral("  ПРОПУСК  ") << name << QStringLiteral("  (GPU недоступен)") << Qt::endl;
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
        out << QStringLiteral("  ПРОВАЛ   ") << name
            << QStringLiteral("  — после расчёта записи не осталось") << Qt::endl;
        clearCheckpoints();
        return;
    }
    const Spectrum extended = runWorker(target, LoadMode::FromCheckpoint);
    clearCheckpoints();

    if (extended == direct) {
        ++g_passed;
        out << "  ok       " << name
            << QStringLiteral("  (досчитано с ") << saved << QStringLiteral(" оп.)") << Qt::endl;
        return;
    }
    ++g_failed;
    out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
    out << QStringLiteral("      напрямую: ") << formatSpectrum(direct)   << Qt::endl;
    out << QStringLiteral("      досчётом: ") << formatSpectrum(extended) << Qt::endl;
}

static void testBrouwerZimmermannWorker()
{
    out << Qt::endl << QStringLiteral("Брауэр–Циммерман в расчёте: CPU и GPU, короткий и длинный пути") << Qt::endl;

    // Короткие коды с точным спектром: гарантия и полный перебор.
    for (const BZCase& c : bzCases()) {
        const Spectrum exact = Reference::bruteForce(c.rows);
        RunConfig cfg;
        cfg.matrix    = c.rows;
        cfg.algorithm = Algorithm::BrouwerZimmermann;
        cfg.bzWeight  = 6;

        cfg.device = ComputeDevice::CPU;
        const Spectrum cpu = checkBzExact(c.name + QStringLiteral(" CPU, вес 6"), cfg, exact);
        cfg.device = ComputeDevice::GPU;
        const Spectrum gpu = checkBzExact(c.name + QStringLiteral(" GPU, вес 6"), cfg, exact);
        expectSame(c.name + QStringLiteral(": CPU и GPU совпадают"), cpu, gpu);

        // Вес во всю длину: перебирается всё, спектр обязан совпасть целиком.
        cfg.bzWeight = c.rows.first().length();
        cfg.device   = ComputeDevice::CPU;
        checkBzExact(c.name + QStringLiteral(" CPU, весь код"), cfg, exact);
    }

    // Длинный путь: k > 63. Точного спектра нет, поэтому CPU против GPU и
    // против прототипа ниже общей гарантии.
    {
        const QStringList rows = BZ::scramble(BZ::systematicRandom(66, 140, 21), 4);
        RunConfig cfg;
        cfg.matrix    = rows;
        cfg.algorithm = Algorithm::BrouwerZimmermann;
        cfg.bzWeight  = 5;

        cfg.device = ComputeDevice::CPU;
        clearCheckpoints();
        const Spectrum cpu = runWorker(cfg);
        const int exactCpu = g_planExactUpTo, setsCpu = g_planSets, rowsCpu = g_planRows;
        clearCheckpoints();

        Spectrum gpu;
        if (g_gpuAvailable) {
            cfg.device = ComputeDevice::GPU;
            gpu = runWorker(cfg);
            clearCheckpoints();
        }

        const QString what = QStringLiteral("[140,66] длинный путь: множеств %1, до %2 строк, точно до веса %3")
                                 .arg(setsCpu).arg(rowsCpu).arg(exactCpu);
        if (exactCpu >= cfg.bzWeight && !cpu.isEmpty()) { ++g_passed; out << "  ok       " << what << Qt::endl; }
        else { ++g_failed; out << QStringLiteral("  ПРОВАЛ   ") << what << Qt::endl; }

        if (g_gpuAvailable)
            expectSame(QStringLiteral("[140,66]: CPU и GPU совпадают"), cpu, gpu);
        else
            out << QStringLiteral("  ПРОПУСК  [140,66] GPU  (GPU недоступен)") << Qt::endl;

        // Прототип ищет множества сам, и выше гарантии его находки другие.
        // Ниже — обязан совпасть.
        const BZ::Result proto = BZ::run(rows, 2, 3);
        const int common = std::min(proto.guaranteedBelow, exactCpu + 1);
        bool ok = true;
        for (int w = 0; w < common; ++w)
            if (cpu.value(w, 0) != proto.spectrum.value(w, 0)) {
                ok = false;
                out << QStringLiteral("      вес %1: расчёт %2, прототип %3")
                           .arg(w).arg(cpu.value(w, 0)).arg(proto.spectrum.value(w, 0)) << Qt::endl;
            }
        const QString vs = QStringLiteral("[140,66]: совпадает с прототипом ниже веса %1").arg(common);
        if (ok) { ++g_passed; out << "  ok       " << vs << Qt::endl; }
        else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   ") << vs << Qt::endl; }
    }

    // Возобновление и досчёт: слои с несколькими множествами и сквозной
    // нумерацией внутри слоя.
    out << Qt::endl << QStringLiteral("Брауэр–Циммерман: сохранение и досчёт") << Qt::endl;
    {
        // Слои маленькие — каждый слой каждого множества уходит одним чанком,
        // и обрыв приходится ровно на границу множества: 903 операции, стоп
        // после 300 — это середина слоя из двух строк.
        RunConfig cfg;
        cfg.matrix    = BZ::scramble(BZ::systematicRandom(24, 96, 9), 2);
        cfg.algorithm = Algorithm::BrouwerZimmermann;
        cfg.bzWeight  = 8;
        cfg.device    = ComputeDevice::CPU;
        checkResume(QStringLiteral("CPU [96,24], вес 8, обрыв на границе множества"), cfg, cfg, 100, 300);
        RunConfig gpuCfg = cfg; gpuCfg.device = ComputeDevice::GPU;
        checkResume(QStringLiteral("GPU [96,24], вес 8, обрыв на границе множества"), gpuCfg, gpuCfg, 100, 300);
        checkResume(QStringLiteral("CPU -> GPU [96,24], перенос записи"), cfg, gpuCfg, 100, 300);
        checkExtendBz(QStringLiteral("CPU [96,24]: вес 5, потом 8"), cfg, 5, 8);
        checkExtendBz(QStringLiteral("GPU [96,24]: вес 5, потом 8"), gpuCfg, 5, 8);

        // Слой из пяти строк на 48 — 1,7 млн комбинаций, больше чанка в
        // миллион: обрыв попадает внутрь множества.
        RunConfig mid;
        mid.matrix    = BZ::scramble(BZ::systematicRandom(48, 96, 33), 6);
        mid.algorithm = Algorithm::BrouwerZimmermann;
        mid.bzWeight  = 11;
        mid.device    = ComputeDevice::CPU;
        RunConfig midGpu = mid; midGpu.device = ComputeDevice::GPU;
        checkResume(QStringLiteral("CPU [96,48], вес 11, обрыв внутри множества"), mid, mid, 1000000, 2500000);
        checkResume(QStringLiteral("GPU [96,48], вес 11, обрыв внутри множества"), midGpu, midGpu, 1000000, 2500000);

        // Длинный путь: слой из пяти строк на 66 — 9,7 млн масок на множество,
        // чанк GPU при 8x32 нитях — миллион.
        RunConfig lng;
        lng.matrix     = BZ::scramble(BZ::systematicRandom(66, 140, 21), 4);
        lng.algorithm  = Algorithm::BrouwerZimmermann;
        lng.bzWeight   = 9;
        lng.device     = ComputeDevice::CPU;
        lng.blocksGpu  = 8;
        lng.threadsGpu = 32;
        RunConfig lngGpu = lng; lngGpu.device = ComputeDevice::GPU;
        checkResume(QStringLiteral("CPU [140,66] длинный, обрыв внутри множества"), lng, lng, 3000000, 8000000);
        checkResume(QStringLiteral("GPU [140,66] длинный, обрыв внутри множества"), lngGpu, lngGpu, 3000000, 8000000);
        lng.bzWeight = 5;
        checkExtendBz(QStringLiteral("CPU [140,66] длинный: вес 4, потом 5"), lng, 4, 5);
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
    cfg.device    = device == QStringLiteral("cpu") ? ComputeDevice::CPU : ComputeDevice::GPU;
    cfg.threadsCpu = omp_get_num_procs();
    cfg.autoTune   = cfg.device == ComputeDevice::GPU;

    const int k = cfg.matrix.size();
    const int n = cfg.matrix.first().length();
    out << QStringLiteral("[%1,%2], точно до веса %3, %4")
               .arg(n).arg(k).arg(weight)
               .arg(cfg.device == ComputeDevice::CPU ? QStringLiteral("CPU") : QStringLiteral("GPU")) << Qt::endl;
    out.flush();

    clearCheckpoints();
    const auto t = std::chrono::steady_clock::now();
    const Spectrum spectrum = runWorker(cfg);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    clearCheckpoints();

    out << QStringLiteral("множеств %1, до %2 строк, точно до веса %3; %4 с")
               .arg(g_planSets).arg(g_planRows).arg(g_planExactUpTo).arg(sec, 0, 'f', 1) << Qt::endl;
    int shown = 0;
    for (auto it = spectrum.cbegin(); it != spectrum.cend() && shown < 16; ++it, ++shown)
        out << QStringLiteral("   %1  %2%3").arg(it.key(), 3).arg(it.value(), 12)
                   .arg(it.key() <= g_planExactUpTo ? QStringLiteral("  точно") : QStringLiteral("  (неполно)"))
            << Qt::endl;
    return 0;
}

// ------------------------------------------------ случайный поиск (Леон)

static void expectLeon(const QString& name, bool ok, const QString& detail = QString())
{
    if (ok) { ++g_passed; out << "  ok       " << name << Qt::endl; }
    else {
        ++g_failed;
        out << QStringLiteral("  ПРОВАЛ   ") << name << Qt::endl;
        if (!detail.isEmpty()) out << QStringLiteral("      ") << detail << Qt::endl;
    }
}

// Модель поимки: вероятности в пределах, монотонны по глубине и по весу,
// план выбирает глубину от одной до четырёх строк.
static void testLeonModel()
{
    out << Qt::endl << QStringLiteral("Случайный поиск: модель поимки") << Qt::endl;

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

    const Leon::Plan plan = Leon::plan(n, k, 42, 1e-9);
    // С учётом цены Гаусса выгодны две строки: одна — слишком много попыток,
    // три — слишком дорогая каждая.
    expectLeon(QStringLiteral("план для веса 42: %1 строк, %2 попыток")
                   .arg(plan.rows).arg(plan.trials),
               plan.rows == 2 && plan.trials > 100000 && plan.trials < 1000000);

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
    out << Qt::endl << QStringLiteral("Случайный поиск в расчёте") << Qt::endl;

    struct Case { QString name; QStringList rows; int weight; };
    const QVector<Case> cases = {
        { QStringLiteral("Голей [24,12]"),           Reference::golay24_12(),                             12 },
        { QStringLiteral("случайный [40,16]"),       BZ::scramble(BZ::systematicRandom(16, 40, 7), 11),  12 },
        { QStringLiteral("случайный [60,20]"),       BZ::scramble(BZ::systematicRandom(20, 60, 3), 5),   15 },
        { QStringLiteral("случайный [96,24]"),       BZ::scramble(BZ::systematicRandom(24, 96, 9), 2),   20 },
    };
    auto checkDevice = [&](const Case& c, const Spectrum& exact, ComputeDevice device) {
        const QString who = device == ComputeDevice::CPU ? QStringLiteral("CPU") : QStringLiteral("GPU");
        if (device == ComputeDevice::GPU && !g_gpuAvailable) {
            out << QStringLiteral("  ПРОПУСК  ") << c.name << QStringLiteral(" GPU  (GPU недоступен)") << Qt::endl;
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
        checkDevice(c, exact, ComputeDevice::CPU);
        checkDevice(c, exact, ComputeDevice::GPU);
    }

    // Длинный код: точного спектра нет, сверяемся с Брауэром–Циммерманом,
    // который до веса 9 точен.
    {
        const QStringList rows = BZ::scramble(BZ::systematicRandom(66, 140, 21), 4);
        RunConfig bz;
        bz.matrix    = rows;
        bz.algorithm = Algorithm::BrouwerZimmermann;
        bz.bzWeight  = 8;
        bz.device    = ComputeDevice::CPU;
        clearCheckpoints();
        const Spectrum exact = runWorker(bz);
        const int exactUpTo = g_planExactUpTo;

        for (ComputeDevice device : { ComputeDevice::CPU, ComputeDevice::GPU }) {
            const bool gpu = device == ComputeDevice::GPU;
            if (gpu && !g_gpuAvailable) {
                out << QStringLiteral("  ПРОПУСК  [140,66] длинный GPU  (GPU недоступен)") << Qt::endl;
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

    // Подбор сетки включён, устройство GPU: подбор обязан промолчать (у
    // поиска своё ядро), а итог — попасть в запись автосохранения. Раньше
    // подбор лез в пустую таблицу биномов, а финал затирал спектр нулями из
    // d_spectrum, который поиск не трогает.
    if (g_gpuAvailable) {
        RunConfig cfg;
        cfg.matrix     = Reference::golay24_12();
        cfg.algorithm  = Algorithm::RandomInfoSets;
        cfg.leonWeight = 12;
        cfg.device     = ComputeDevice::GPU;
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
            return BZ::scramble(out, seed + 1);
        };
        struct Twin { QString name; QStringList rows; int weight; };
        const QVector<Twin> twins = {
            { QStringLiteral("[90,30] в разделяемой"),      BZ::scramble(BZ::systematicRandom(30, 90, 17), 3), 30 },
            { QStringLiteral("[700,40] строка в 11 слов"),  sparseCode(40, 700, 5, 23),                         12 },
            { QStringLiteral("[600,300] строка в 10 слов"), sparseCode(300, 600, 5, 29),                         8 },
        };
        for (const Twin& t : twins) {
            const int n = t.rows.first().length(), k = t.rows.size();
            const bool shared = leonFitsShared(k, n, (n + 63) / 64);
            // Глубина у устройств может разойтись из-за разной цены Гаусса;
            // сравнивать имеет смысл только при одинаковой.
            const Leon::Plan cpuPlan = Leon::plan(n, k, t.weight, 1e-12, false);
            const Leon::Plan gpuPlan = Leon::plan(n, k, t.weight, 1e-12, true);
            if (cpuPlan.rows != gpuPlan.rows) {
                out << QStringLiteral("  ПРОПУСК  %1 CPU и GPU слово в слово: разная глубина (%2 и %3)")
                           .arg(t.name).arg(cpuPlan.rows).arg(gpuPlan.rows) << Qt::endl;
                continue;
            }
            RunConfig cfg;
            cfg.matrix     = t.rows;
            cfg.algorithm  = Algorithm::RandomInfoSets;
            cfg.leonWeight = t.weight;
            cfg.device     = ComputeDevice::CPU;
            clearCheckpoints();
            const Spectrum cpu = runWorker(cfg);
            const quint64 cpuTrials = g_searchDone;
            cfg.device = ComputeDevice::GPU;
            clearCheckpoints();
            const Spectrum gpu = runWorker(cfg);
            const quint64 gpuTrials = g_searchDone;
            clearCheckpoints();
            expectLeon(QStringLiteral("%1 (%2) CPU и GPU слово в слово: попыток %3 и %4, весов %5")
                           .arg(t.name)
                           .arg(shared ? QStringLiteral("shared") : QStringLiteral("global"))
                           .arg(cpuTrials).arg(gpuTrials).arg(cpu.size()),
                       !cpu.isEmpty() && cpu == gpu && cpuTrials == gpuTrials,
                       QStringLiteral("CPU: %1\n      GPU: %2").arg(formatSpectrum(cpu), formatSpectrum(gpu)));
        }
    }
}

// Случайный поиск на матрице из файла.
//
// Запуск: SpectrumTests.exe --leon-run <файл матрицы> <вес> [<степень пропуска>] [cpu|gpu]
static int leonRun(const QString& path, int weight, int missExponent, const QString& device)
{
    RunConfig cfg;
    if (!loadMatrixOrCase(path, cfg))
        return 2;
    cfg.algorithm        = Algorithm::RandomInfoSets;
    cfg.leonWeight       = weight;
    cfg.leonMissExponent = missExponent;
    cfg.device           = device == QStringLiteral("gpu") ? ComputeDevice::GPU : ComputeDevice::CPU;
    cfg.threadsCpu       = omp_get_num_procs();

    const int k = cfg.matrix.size();
    const int n = cfg.matrix.first().length();
    const Leon::Plan plan = Leon::plan(n, k, weight, std::pow(10.0, -missExponent),
                                       cfg.device == ComputeDevice::GPU);
    out << QStringLiteral("[%1,%2], все слова до веса %3, пропуск 10^-%4, %8: %5 строк за попытку, попыток %6, слов %7")
               .arg(n).arg(k).arg(weight).arg(missExponent)
               .arg(plan.rows).arg(plan.trials).arg(double(plan.trials) * plan.wordsPerTrial, 0, 'g', 3)
               .arg(cfg.device == ComputeDevice::GPU ? QStringLiteral("GPU") : QStringLiteral("CPU")) << Qt::endl;
    out.flush();

    clearCheckpoints();
    const auto t = std::chrono::steady_clock::now();
    const Spectrum spectrum = runWorker(cfg);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    clearCheckpoints();

    out << QStringLiteral("попыток %1 из %2, %3 с; вероятность пропуска по модели ~%4")
               .arg(g_searchDone).arg(g_searchTotal).arg(sec, 0, 'f', 1).arg(g_searchMiss, 0, 'g', 2) << Qt::endl;
    for (auto it = spectrum.cbegin(); it != spectrum.cend(); ++it) {
        const float unseen = it.key() < g_searchUnseen.size() ? g_searchUnseen.at(it.key()) : 0.0f;
        out << QStringLiteral("   %1  %2%3").arg(it.key(), 3).arg(it.value(), 12)
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

static void testProductCode()
{
    out << Qt::endl << QStringLiteral("Коды произведения: низ спектра по компонентам") << Qt::endl;

    struct Case { QString name; QStringList g1, g2; };
    const QVector<Case> cases = {
        { QStringLiteral("eHamming(8,4) x eHamming(8,4) = [64,16]"), Reference::extHamming8_4(), Reference::extHamming8_4() },
        { QStringLiteral("Hamming(7,4) x Hamming(7,4) = [49,16]"),   Reference::hamming7_4(),    Reference::hamming7_4() },
        { QStringLiteral("Hamming(7,4) x eHamming(8,4) = [56,16]"),  Reference::hamming7_4(),    Reference::extHamming8_4() },
        { QStringLiteral("Hamming(7,4) x rnd[12,5] = [84,20]"),      Reference::hamming7_4(),    BZ::systematicRandom(5, 12, 3) },
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

        // Ранги до R: ниже границы ранга R+1 — точно, выше — не больше точного.
        for (int R = 1; R < maxRank; ++R) {
            const Spectrum part = productSpectrum(c.g1, c.g2, R, quint64(n), exactUpTo);
            bool okR = true;
            QStringList probs;
            for (auto it = exact.cbegin(); it != exact.cend(); ++it) {
                const quint64 got = part.value(it.key(), 0);
                if (it.key() <= exactUpTo ? got != it.value() : got > it.value()) {
                    okR = false;
                    probs << QStringLiteral("вес %1: точно %2, ранги<=%3 дают %4")
                                 .arg(it.key()).arg(it.value()).arg(R).arg(got);
                }
            }
            for (auto it = part.cbegin(); it != part.cend(); ++it)
                if (!exact.contains(it.key())) { okR = false; probs << QStringLiteral("лишний вес %1").arg(it.key()); }
            expectLeon(c.name + QStringLiteral(", ранги до %1: точно до веса %2").arg(R).arg(exactUpTo),
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
        cfg.device      = ComputeDevice::CPU;
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
        const int savedLimit = Product::bruteForceMaxK;
        Product::bruteForceMaxK = 3;
        RunConfig cfg;
        cfg.matrix      = Reference::golay24_12();
        cfg.matrix2     = Reference::hamming7_4();
        cfg.algorithm   = Algorithm::ProductCode;
        cfg.productRank = 1;
        cfg.device      = ComputeDevice::CPU;
        clearCheckpoints();
        const Spectrum got = runWorker(cfg);
        clearCheckpoints();
        Product::bruteForceMaxK = savedLimit;

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
        cfg.device      = ComputeDevice::CPU;
        clearCheckpoints();
        const Spectrum exact = runWorker(cfg);
        const int exactUpTo = g_productExactUpTo;

        const int savedLimit = Product::bruteForceMaxK;
        Product::bruteForceMaxK = 6;
        clearCheckpoints();
        const Spectrum viaLeon = runWorker(cfg);
        const QVector<AutosaveEntry> entries = testStore().list();
        clearCheckpoints();
        Product::bruteForceMaxK = savedLimit;

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
    cfg.device        = device == QStringLiteral("gpu") ? ComputeDevice::GPU : ComputeDevice::CPU;
    cfg.threadsCpu    = omp_get_num_procs();
    cfg.autoTune      = cfg.device == ComputeDevice::GPU;

    out << QStringLiteral("[%1,%2] x [%3,%4], вес %5, ранги до %6")
               .arg(c1.matrix.first().length()).arg(c1.matrix.size())
               .arg(c2.matrix.first().length()).arg(c2.matrix.size())
               .arg(weight).arg(rank) << Qt::endl;
    out.flush();

    clearCheckpoints();
    const auto t = std::chrono::steady_clock::now();
    const Spectrum spectrum = runWorker(cfg);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    clearCheckpoints();

    out << g_productText << QStringLiteral("; %1 с").arg(sec, 0, 'f', 1) << Qt::endl;
    for (auto it = spectrum.cbegin(); it != spectrum.cend(); ++it)
        out << QStringLiteral("   %1  %2").arg(it.key(), 6).arg(it.value(), 16) << Qt::endl;
    return 0;
}

// Гарантия алгоритма: ниже границы совпадение с точным спектром обязано быть
// побитовым, выше — БЦ не имеет права насчитать больше, чем есть.
static void testBrouwerZimmermann()
{
    out << Qt::endl << QStringLiteral("Брауэр–Циммерман: низ спектра с гарантией") << Qt::endl;

    for (const BZCase& c : bzCases()) {
        const Reference::Spectrum exact = Reference::bruteForce(c.rows);
        const BZ::Result          bz    = BZ::run(c.rows, c.r, c.sets);

        bool ok = true;
        for (auto it = exact.cbegin(); it != exact.cend(); ++it) {
            const quint64 found = bz.spectrum.value(it.key(), 0);
            if (it.key() < bz.guaranteedBelow ? found != it.value() : found > it.value()) {
                ok = false;
                out << QStringLiteral("      вес %1: точно %2, БЦ %3 (граница %4)")
                           .arg(it.key()).arg(it.value()).arg(found).arg(bz.guaranteedBelow) << Qt::endl;
            }
        }
        for (auto it = bz.spectrum.cbegin(); it != bz.spectrum.cend(); ++it)
            if (!exact.contains(it.key())) { ok = false; out << QStringLiteral("      лишний вес %1").arg(it.key()) << Qt::endl; }

        const QString what = QStringLiteral("%1: веса < %2 точны").arg(c.name).arg(bz.guaranteedBelow);
        if (ok) { ++g_passed; out << "  ok       " << what << Qt::endl; }
        else    { ++g_failed; out << QStringLiteral("  ПРОВАЛ   ") << what << Qt::endl; }
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
    out.setCodec("UTF-8");

    // Отдельное имя приложения: чекпоинты тестов не должны попадать в ветку
    // реестра, которой пользуется сама программа.
    QCoreApplication::setOrganizationName(QStringLiteral("Alpas"));
    QCoreApplication::setApplicationName(QStringLiteral("SpectrumTests"));

    int deviceCount = 0;
    g_gpuAvailable = (cudaGetDeviceCount(&deviceCount) == cudaSuccess) && deviceCount > 0;
    out << QStringLiteral("GPU: ")
        << (g_gpuAvailable ? QStringLiteral("доступен")
                           : QStringLiteral("не найден, GPU-тесты пропускаются"))
        << Qt::endl;

    const QStringList args = app.arguments();

    const int bzAt = args.indexOf(QStringLiteral("--bz-file"));
    if (bzAt >= 0 && bzAt + 2 < args.size()) {
        const int sets = bzAt + 3 < args.size() ? args.at(bzAt + 3).toInt() : 8;
        const int rc = bzFile(args.at(bzAt + 1), args.at(bzAt + 2).toInt(), sets > 0 ? sets : 8);
        out.flush();
        return rc;
    }

    if (args.contains(QStringLiteral("--bz"))) {
        bzReport();
        out.flush();
        return 0;
    }

    const int productAt = args.indexOf(QStringLiteral("--product-run"));
    if (productAt >= 0 && productAt + 2 < args.size()) {
        const int weight = productAt + 3 < args.size() ? args.at(productAt + 3).toInt() : 0;
        const int rank   = productAt + 4 < args.size() ? args.at(productAt + 4).toInt() : 2;
        const QString device = productAt + 5 < args.size() ? args.at(productAt + 5).toLower() : QStringLiteral("cpu");
        const int rc = productRun(args.at(productAt + 1), args.at(productAt + 2), weight, rank > 0 ? rank : 2, device);
        out.flush();
        return rc;
    }

    const int leonAt = args.indexOf(QStringLiteral("--leon-run"));
    if (leonAt >= 0 && leonAt + 2 < args.size()) {
        const int missExp = leonAt + 3 < args.size() ? args.at(leonAt + 3).toInt() : 9;
        const QString device = leonAt + 4 < args.size() ? args.at(leonAt + 4).toLower() : QStringLiteral("cpu");
        const int rc = leonRun(args.at(leonAt + 1), args.at(leonAt + 2).toInt(), missExp > 0 ? missExp : 9, device);
        out.flush();
        return rc;
    }

    const int bzRunAt = args.indexOf(QStringLiteral("--bz-run"));
    if (bzRunAt >= 0 && bzRunAt + 2 < args.size()) {
        const QString device = bzRunAt + 3 < args.size() ? args.at(bzRunAt + 3).toLower() : QStringLiteral("gpu");
        const int rc = bzRun(args.at(bzRunAt + 1), args.at(bzRunAt + 2).toInt(), device);
        out.flush();
        return rc;
    }

    if (args.contains(QStringLiteral("--bench"))) {
        benchmark();
        out.flush();
        return 0;
    }

    const int sweepAt = args.indexOf(QStringLiteral("--sweep"));
    if (sweepAt >= 0 && sweepAt + 1 < args.size()) {
        const int rc = sweepLaunchParams(args.at(sweepAt + 1));
        out.flush();
        return rc;
    }

    // --probe <случай|файл матрицы> [<строк>]
    const int probeAt = args.indexOf(QStringLiteral("--probe"));
    if (probeAt >= 0 && probeAt + 1 < args.size()) {
        const int rows = probeAt + 2 < args.size() ? args.at(probeAt + 2).toInt() : 0;
        const int rc = probeOnly(args.at(probeAt + 1), rows);
        out.flush();
        return rc;
    }

    // --rate <случай|файл матрицы> <мс> [<строк>]
    const int rateAt = args.indexOf(QStringLiteral("--rate"));
    if (rateAt >= 0 && rateAt + 2 < args.size()) {
        const int rows = rateAt + 3 < args.size() ? args.at(rateAt + 3).toInt() : 0;
        const int rc = updateRate(args.at(rateAt + 1), args.at(rateAt + 2).toInt(), rows);
        out.flush();
        return rc;
    }

    const int tuneAt = args.indexOf(QStringLiteral("--tune"));
    if (tuneAt >= 0 && tuneAt + 1 < args.size()) {
        const int rc = tuneOnly(args.at(tuneAt + 1));
        out.flush();
        return rc;
    }

    const int profAt = args.indexOf(QStringLiteral("--profile"));
    if (profAt >= 0 && profAt + 1 < args.size()) {
        const int rc = runSingleForProfiling(args.at(profAt + 1));
        out.flush();
        return rc;
    }

    const int dumpAt = args.indexOf(QStringLiteral("--dump"));
    if (dumpAt >= 0 && dumpAt + 1 < args.size()) {
        const int rc = dumpGolden(args.at(dumpAt + 1));
        out.flush();
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

    testAxisLabelStep();
    testUpdateIntervals();
    testProbeLeavesNoTrace();
    testBrouwerZimmermann();
    testBrouwerZimmermannWorker();
    testLeonModel();
    testLeonWorker();
    testProductCode();

    testAutosaveStore();
    testCanResume();
    testExtendMaxRows();

    testAutoTunedGrid();

    testCheckpoints();
    testCheckpointPortability();
    testAutoTunedCheckpoints();
    testOversizedMatrixRejected();

    out << Qt::endl
        << QStringLiteral("итого: пройдено ") << g_passed << QStringLiteral(", провалено ") << g_failed << Qt::endl;
    return g_failed == 0 ? 0 : 1;
}
