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
#include <QFile>
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
static int sweepLaunchParams(const QString& which)
{
    RunConfig base;
    base.device = ComputeDevice::GPU;

    if (which == QStringLiteral("wide")) {
        base.matrix    = Reference::randomMatrix(50, 2000, 10);
        base.algorithm = Algorithm::SimpleXor;
        base.maxRows   = 8;
    } else if (which == QStringLiteral("narrow")) {
        base.matrix    = Reference::randomMatrix(50, 50, 3);
        base.algorithm = Algorithm::SimpleXor;
        base.maxRows   = 9;
    } else if (which == QStringLiteral("gray")) {
        base.matrix    = Reference::randomMatrix(28, 1000, 12);
        base.algorithm = Algorithm::GrayCode;
        base.maxRows   = 28;
    } else if (which == QStringLiteral("long")) {
        base.matrix    = Reference::randomMatrix(70, 1000, 11);
        base.algorithm = Algorithm::SimpleXor;
        base.maxRows   = 6;
    } else {
        out << QStringLiteral("ожидалось --sweep wide|narrow|gray|long") << Qt::endl;
        return 2;
    }

    if (!g_gpuAvailable) {
        out << QStringLiteral("GPU недоступен") << Qt::endl;
        return 1;
    }

    const QVector<int> blocks  { 23, 46, 92, 138, 184, 276, 368 };
    const QVector<int> threads { 64, 128, 256, 512, 1024 };

    out << QStringLiteral("Перебор параметров, случай ") << which
        << QStringLiteral(" (46 мультипроцессоров)") << Qt::endl << Qt::endl;
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

    testCheckpoints();
    testCheckpointPortability();
    testOversizedMatrixRejected();

    out << Qt::endl
        << QStringLiteral("итого: пройдено ") << g_passed << QStringLiteral(", провалено ") << g_failed << Qt::endl;
    return g_failed == 0 ? 0 : 1;
}
