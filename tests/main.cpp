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
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>

#include <cuda_runtime.h>

// worker.h тянет gmpxx.h, где есть std::numeric_limits<...>::min(). Его нужно
// разобрать до windows.h, иначе макросы min/max из windows.h ломают тело класса.
#include "worker.h"
#include "reference.h"

#ifdef Q_OS_WIN
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

using Reference::Spectrum;
using Algorithm     = ComputationSettings::Algorithm;
using ComputeDevice = ComputationSettings::ComputeDevice;

static QTextStream out(stdout);
static int g_passed = 0;
static int g_failed = 0;
static bool g_gpuAvailable = false;

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
};

static ComputationSettings makeSettings(const RunConfig& cfg)
{
    ComputationSettings s;
    s.matrix        = cfg.matrix;
    s.algorithmType = cfg.algorithm;
    s.enumType      = ComputationSettings::Full;
    s.maxRows       = cfg.maxRows > 0 ? cfg.maxRows : cfg.matrix.size();
    s.compDev       = cfg.device;
    s.compDevSet.threadsCpu = cfg.threadsCpu;
    s.compDevSet.blocksGpu  = cfg.blocksGpu;
    s.compDevSet.threadsGpu = cfg.threadsGpu;
    // Интервалы задраны так, чтобы за время теста чекпоинт не сработал:
    // сохранение состояния проверяется отдельными тестами.
    s.timeIntSet.saveSpectrumInterval   = 100000;
    s.timeIntSet.updateSpectrumInterval = 100000;
    return s;
}

// Прогоняет расчёт синхронно и возвращает итоговый спектр.
static Spectrum runWorker(const RunConfig& cfg)
{
    Worker worker;
    Spectrum captured;
    bool errored = false;
    QString errorMessage;

    QObject::connect(&worker, &Worker::updateSpectrumPTE,
                     [&captured](const SpectrumText& s) { captured = parseSpectrumText(s); });
    QObject::connect(&worker, &Worker::errorOccurred,
                     [&](const QString& m) { errored = true; errorMessage = m; });

    worker.setSettings(makeSettings(cfg).toJson());
    worker.initializeRunState(LoadMode::Reset);
    worker.computeSpectrum();

    if (errored) {
        out << QStringLiteral("      ошибка от Worker: ") << errorMessage << Qt::endl;
        return Spectrum();
    }
    return stripZeros(captured);
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

    testShortCode(QStringLiteral("Хэмминг (7,4)"),
                  Reference::hamming7_4(), Reference::analyticHamming7_4());
    testShortCode(QStringLiteral("Расширенный Хэмминг (8,4)"),
                  Reference::extHamming8_4(), Reference::analyticExtHamming8_4());
    testShortCode(QStringLiteral("Голей (24,12)"),
                  Reference::golay24_12(), Reference::analyticGolay24_12());

    testPartialShort(Reference::golay24_12(), 4);

    testLongCode(70, 3);
    testLongCode(64, 2);

    testDualCode(QStringLiteral("Хэмминг (7,4)"), Reference::hamming7_4());

    out << Qt::endl
        << QStringLiteral("итого: пройдено ") << g_passed << QStringLiteral(", провалено ") << g_failed << Qt::endl;
    return g_failed == 0 ? 0 : 1;
}
