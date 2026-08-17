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
#include <QSettings>
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
//
// loadMode задаёт, начинать с нуля или продолжить с сохранённого состояния.
// checkpointEveryOps/stopAfterOps включают воспроизводимое прерывание.
static Spectrum runWorker(const RunConfig& cfg,
                          LoadMode loadMode           = LoadMode::Reset,
                          quint64  checkpointEveryOps = 0,
                          quint64  stopAfterOps       = 0)
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
    worker.setCheckpointOpsPolicy(checkpointEveryOps, stopAfterOps);
    worker.initializeRunState(loadMode);
    worker.computeSpectrum();

    if (errored) {
        out << QStringLiteral("      ошибка от Worker: ") << errorMessage << Qt::endl;
        return Spectrum();
    }
    return stripZeros(captured);
}

// Стирает все сохранённые чекпоинты, чтобы прогон не зависел от предыдущего.
static void clearCheckpoints()
{
    QSettings s;
    s.remove(QStringLiteral("checkpoints"));
    s.sync();
}

// Полное число операций, которое должен выполнить расчёт при данных настройках.
// Нужно, чтобы отличить настоящий обрыв на середине от «досчитали до конца и
// только потом сохранились».
static quint64 expectedTotalOps(const RunConfig& cfg)
{
    const quint64 k = quint64(cfg.matrix.size());
    if (cfg.algorithm != Algorithm::SimpleXor)
        return 1ULL << k;                    // код Грея перебирает все маски подряд

    const quint64 maxRows = cfg.maxRows > 0 ? quint64(cfg.maxRows) : k;
    quint64 total = 0;
    for (quint64 r = 0; r <= maxRows; ++r)
        total += Reference::binom(k, r);
    return total;
}

// Сколько операций записано в единственном сохранённом чекпоинте.
// -1 — чекпоинта нет вовсе.
static qint64 savedDoneOps()
{
    QSettings s;
    s.beginGroup(QStringLiteral("checkpoints"));
    const QStringList groups = s.childGroups();
    if (groups.isEmpty()) return -1;

    s.beginGroup(groups.first());
    const QJsonObject runState = s.value(QStringLiteral("runState")).toJsonObject();
    if (runState.isEmpty()) return -1;

    return runState[QStringLiteral("doneOps")].toVariant().toLongLong();
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

    testCheckpoints();
    testCheckpointPortability();

    out << Qt::endl
        << QStringLiteral("итого: пройдено ") << g_passed << QStringLiteral(", провалено ") << g_failed << Qt::endl;
    return g_failed == 0 ? 0 : 1;
}
