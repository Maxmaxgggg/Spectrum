#include "worker_p.h"
#include "combinations.h"
#include "dualcode.h"
#include "gridtuner.h"
#include "leonkernel.cuh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

using namespace std::chrono;

Worker::Worker(QObject *parent)
    : QObject(parent)
    , m_buffers(new WorkerBuffers)
{
    // Спектр уходит в окно, а настройки приходят из него через очередь
    // событий; вызов по имени (invokeMethod) ищет тип тоже по имени.
    qRegisterMetaType<SpectrumCounts>("SpectrumCounts");
    qRegisterMetaType<ComputationSettings>("ComputationSettings");
}

Worker::~Worker()
{
}

// Кусок слоя с номера layerOffset (сквозного по множествам) длиной до
// chunkSize, не пересекающий границу множества. См. LayerSlice.
static LayerSlice sliceLayer(const CodeGeometry& g, quint64 perSet,
                             quint64 layerOffset, quint64 chunkSize)
{
    LayerSlice slice;
    const quint64 set = perSet ? layerOffset / perSet : 0;
    slice.offset        = layerOffset - set * perSet;
    slice.size          = std::min(chunkSize, perSet - slice.offset);
    slice.slot.rowBase  = int(set * g.numOfRows);
    slice.slot.setIndex = int(set);
    slice.slot.setCount = g.setCount;
    return slice;
}

quint64 Worker::totalLayerOps(const CodeGeometry& g) const
{
    const quint64 perSet = totalCombinations(g.numOfRows, g.maxRows);
    if (g.setCount <= 1)
        return perSet;
    if (perSet > std::numeric_limits<quint64>::max() / quint64(g.setCount))
        return std::numeric_limits<quint64>::max();
    return perSet * quint64(g.setCount);
}

// Полное число кодовых слов при переборе до maxComb строк включительно.
//
// Раньше считалось инкрементально: comb = comb * (k - r + 1) / r. Формула
// точная в математике, но промежуточное произведение вылезает за uint64 куда
// раньше самого результата. Для матрицы в 336 строк это происходит уже при
// r = 10 — а именно 10 и есть максимум, который для неё разрешает интерфейс.
// Итог получался неверным, и вместе с ним врали процент и оценка времени.
//
// Теперь складываются готовые значения из BinomTable: она строится по
// треугольнику Паскаля, без промежуточных произведений, и сама проверяет
// переполнение.
quint64 Worker::totalCombinations(quint64 k, quint64 maxComb) const
{
    if (maxComb > k) maxComb = k;

    quint64 sum = 0;
    for (quint64 r = 0; r <= maxComb; ++r) {
        const quint64 term = m_binomTable(k, r);
        // Сама сумма тоже может не поместиться: при k = 66 и maxComb = 33
        // это уже больше 2^65. Такой расчёт всё равно занял бы столетия,
        // поэтому просто упираемся в потолок, а не выдаём мусор.
        if (sum > std::numeric_limits<quint64>::max() - term)
            return std::numeric_limits<quint64>::max();
        sum += term;
    }
    return sum;
}

// Отчёт об оценке оставшегося времени и средней скорости.
void Worker::reportEstimate()
{
    if (m_probeMode)
        return;
    m_progress.markEstimate();
    m_runState.elapsedSec = m_progress.elapsedSec();
    emit estimateUpdated(int(m_progress.elapsedSec()),
                         m_progress.minutesLeft(),
                         m_progress.speed(),
                         m_progress.doneOps(),
                         m_progress.totalOps());
}

// Ход одного шага расчёта произведения: общий счётчик подменяется на шаг,
// оценка времени и полоса считаются от его начала.
void Worker::reportStageProgress(quint64 done, quint64 total)
{
    m_progress.setTotalOps(std::max<quint64>(1, total));
    m_progress.setDoneOps(done);
    const ProgressTracker::Due due = m_progress.due();
    if (due.estimate)
        reportEstimate();
    if (due.bar)
        reportProgress();
}

void Worker::reportProgress()
{
    if (m_probeMode)
        return;
    m_progress.markBar();
    emit progressChanged(m_progress.percent());
}

// Чекпоинт по ходу перебора: (rOffset, chunkOffset) — первое ещё не
// пройденное сочетание.
//
// На видеокарте спектр забирается синхронно, а не через уже начатое
// асинхронное копирование: сохранённое должно точно соответствовать
// посчитанным чанкам. На процессоре он уже лежит в h_spectrum.
//
// Ошибка CUDA здесь — исключение, как и везде: раньше чекпоинт сообщал о ней
// сигналом и возвращал false, путь перебора выходил, и расчёт рапортовал об
// успехе с недосчитанным спектром.
void Worker::saveCheckpoint(const ChunkPlan& plan, int numOfCols, quint64 rOffset, quint64 chunkOffset)
{
    m_progress.markCheckpoint();
    if (plan.stream) {
        CUDA_CALL(cudaStreamSynchronize(plan.stream));
        CUDA_CALL(cudaMemcpy(m_buffers->h_spectrum.get(), m_buffers->d_spectrum.get(), size_t(numOfCols + 1) * sizeof(quint64),
                             cudaMemcpyDeviceToHost));
    }
    m_runState.rOffset     = rOffset;
    m_runState.chunkOffset = chunkOffset;
    makeCheckpoint(numOfCols);
    stopIfOpsLimitReached();
}

// Перебор по слоям и чанкам — общий для шести путей (CPU/GPU × код Грея/
// слои × короткий/длинный код). Здесь всё, кроме самого перебора чанка:
// продолжение с чекпоинта, пауза и отмена, ход расчёта, снимки спектра для
// показа и чекпоинты.
//
// Раньше этот цикл был переписан в каждом из шести путей, и копии успели
// разойтись: пауза обрабатывалась четырьмя способами, в двух путях отмена
// посреди чанка портила чекпоинт, а GPU-путь после отмены во время паузы
// ещё запускал лишний чанк.
void Worker::runChunks(const CodeGeometry& g, const ChunkPlan& plan)
{
    m_progress.begin(plan.totalOps, m_runState.doneOps, m_runState.elapsedSec);
    // При продолжении с чекпоинта полоса сразу показывает пройденное.
    if (!m_probeMode)
        emit progressChanged(m_progress.percent());

    const int cols = int(g.numOfCols);
    // У кода Грея слой один: rOffset там не используется (а проба потолка
    // обновления, наоборот, ставит его на последний слой).
    const quint64 firstLayer = plan.layered ? m_runState.rOffset : 0;
    const quint64 lastLayer  = plan.layered ? plan.lastLayer : 0;

    for (quint64 r = firstLayer; r <= lastLayer; ++r) {
        // Слой — сочетания по каждому из множеств, подряд. Чанк не пересекает
        // границу множества, поэтому бывает короче обычного.
        const quint64 perSet = plan.layerSize(r);
        const quint64 total  = perSet * quint64(g.setCount);
        quint64 offset = (r == firstLayer) ? m_runState.chunkOffset : 0;

        while (offset < total) {
            if (!waitWhilePaused())
                return;
            const LayerSlice slice = sliceLayer(g, perSet, offset, std::max<quint64>(1, plan.chunkTarget));
            if (!plan.run(r, slice))
                return;
            offset += slice.size;
            m_progress.addOps(slice.size);
            m_runState.doneOps = m_progress.doneOps();

            const ProgressTracker::Due due = m_progress.due();
            if (due.estimate)
                reportEstimate();
            if (plan.stream) {
                // Снимок едет с видеокарты через кольцо. Метка двигается, только
                // когда копия реально встала в очередь: иначе при занятом кольце
                // следующая попытка откладывалась бы на целый интервал.
                if (due.spectrum && m_buffers->spectrumRing.enqueue(m_buffers->d_spectrum.get(), plan.stream))
                    m_progress.markSpectrum();
                if (const quint64* snapshot = m_buffers->spectrumRing.takeReady())
                    updateSpectrumFrom(snapshot, cols);
            }
            else if (due.spectrum) {
                m_progress.markSpectrum();
                updateSpectrum(cols);
            }
            if (due.bar)
                reportProgress();
            if (due.checkpoint)
                saveCheckpoint(plan, cols, r, offset);
            if (m_cancelled.load())
                return;
        }
    }
}

void Worker::setLiveIntervals(int spectrumMs, int checkpointSeconds)
{
    if (spectrumMs <= 0 || checkpointSeconds <= 0)
        return;
    m_progress.setIntervals(std::chrono::milliseconds{ spectrumMs },
                            std::chrono::seconds{ checkpointSeconds });
}

// Прерывание сразу после сохранения: состояние на диске согласовано, и
// возобновление начнётся ровно с той точки, которую записал чекпоинт.
void Worker::stopIfOpsLimitReached()
{
    if (m_stopAfterOps > 0 && m_progress.doneOps() >= m_stopAfterOps)
        m_cancelled.store(1);
}

// Возвращает false, если расчёт отменили (в том числе во время паузы).
bool Worker::waitWhilePaused()
{
    while (m_paused.load() != 0) {
        if (m_cancelled.load()) return false;
        QThread::msleep(50);
    }
    return !m_cancelled.load();
}

void Worker::updateSpectrum(int numOfCols)
{
    updateSpectrumFrom(m_buffers->h_spectrum.get(), numOfCols);
}

void Worker::updateSpectrumFrom(const quint64* spectrum, int numOfCols)
{
    if (!spectrum)
        return;
    // Проба меряет, как часто спектр успевает уйти, а не показывает его.
    // Считать надо здесь: через это место проходят все пути, включая CPU.
    if (m_probeMode) {
        ++m_probeSends;
        return;
    }

    // Веса за пределом заказа не показываются вовсе: спектр остаётся той же
    // длины, но там нули.
    const int shownUpTo = m_displayUpToWeight >= 0 ? std::min(numOfCols, m_displayUpToWeight) : numOfCols;
    SpectrumCounts shown;
    shown.counts.resize(numOfCols + 1);
    for (int w = 0; w <= shownUpTo; ++w)
        shown.counts[w] = spectrum[w];
    publishSpectrum(shown);
}

void Worker::publishSpectrum(const SpectrumCounts& shown)
{
    if (m_probeMode) {
        ++m_probeSends;
        return;
    }
    if (!shown.isEmpty())
        emit spectrumUpdated(shown);
}

void Worker::makeCheckpoint(int numOfCols, bool finished)
{
    m_runState.spectrum.resize(numOfCols + 1);
    for (int i = 0; i < numOfCols + 1; i++)
        m_runState.spectrum[i] = m_buffers->h_spectrum[i];

    AutosaveRecord record;
    record.algorithm = m_settings.algorithmType;
    record.enumType  = m_settings.enumType;
    record.maxRows   = m_settings.maxRows;
    record.finished  = finished;
    // У Брауэра–Циммермана глубина перебора выведена из веса, а продолжать
    // расчёт можно только по тем же множествам — они уходят в запись.
    if (m_settings.algorithmType == ComputationSettings::BrouwerZimmermann) {
        record.maxRows  = m_activeMaxRows;
        record.bzWeight = m_settings.bzWeight;
        record.infoSets = m_activeInfoSets;
    }
    if (m_settings.algorithmType == ComputationSettings::RandomInfoSets) {
        record.maxRows          = m_activeMaxRows;
        record.leonWeight       = m_settings.leonWeight;
        record.leonMissExponent = m_settings.leonMissExponent;
        record.leonTrials       = m_activeTrials;
    }
    if (m_settings.algorithmType == ComputationSettings::ProductCode) {
        record.productWeight    = m_settings.productWeight;
        record.productRank      = m_settings.productRank;
        record.productRows1     = m_settings.matrix.size();
        record.productExactUpTo = m_productExactUpTo;
        record.productMissExponent = m_productMissExponent;
    }
    record.savedAt   = QDateTime::currentDateTime();
    record.state     = m_runState;

    // Ключ — матрица и алгоритм. Сама матрица в запись не попадает: она лежит
    // одним файлом на папку, иначе на коде (1000,997) каждое сохранение тащило
    // бы с собой мегабайт нулей и единиц.
    m_autosave.save(autosaveKeyMatrix(), record);
    emit spectrumSaved();
}

void Worker::setAutosaveRoot(const QString& dir)
{
    m_autosaveRootDir = dir;
    m_autosave = AutosaveStore(dir);
}

QStringList Worker::autosaveKeyMatrix() const
{
    if (m_settings.algorithmType == ComputationSettings::ProductCode)
        return m_settings.matrix + m_settings.matrix2;
    return m_settings.matrix;
}
// Точка входа расчёта. Ловит всё, что может бросить вычислитель: раньше
// ошибка CUDA звала abort() и приложение молча исчезало, а переполнение в
// таблице биномов бросало голый const char*, который никто не ловил, — то
// есть std::terminate.
void Worker::computeSpectrum()
{
    try {
        computeSpectrumImpl();
    }
    catch (const CudaError& e) {
        releaseResources();
        emit errorOccurred(e.message());
        emit finished(Constants::ERROR_OCCURRED);
    }
    catch (const std::exception& e) {
        releaseResources();
        emit errorOccurred(QStringLiteral("Ошибка расчёта: %1")
                               .arg(QString::fromUtf8(e.what())));
        emit finished(Constants::ERROR_OCCURRED);
    }
}

void Worker::measureUpdateRate()
{
    m_probeMode  = true;
    m_probeSends = 0;
    m_cancelled.store(0);

    const auto startedAt = steady_clock::now();
    double seconds = 0.0;

    try {
        CodeGeometry g = describeTask();
        initializeRunState(LoadMode::Reset);
        m_runState.spectrum.resize(int(g.spectrumSize));

        // Замер идёт на самом глубоком слое. Слои по числу складываемых строк
        // перебираются по возрастанию, первые из них крошечные, и чанки в них
        // обрезаны по границе слоя: замер по началу расчёта показал бы частоту,
        // которой на деле не будет уже через минуту. У кода Грея слоёв нет, там
        // rOffset ни на что не влияет.
        if (g.maxRows > 0)
            m_runState.rOffset = g.maxRows;

        omp_set_num_threads(m_settings.compDevSet.threadsCpu);
        prepareBuffers(g);
        // Подбор сетки нужен и здесь: он выбирает сетку покрупнее, а от неё
        // напрямую зависит длина чанка и, значит, потолок. Замер без подбора
        // показал бы не ту частоту, с которой пользователь потом будет считать.
        tuneGrid(g);

        // Просить спектр как можно чаще, сохранений не делать: проба не имеет
        // права трогать состояние расчёта.
        m_progress.setIntervals(std::chrono::milliseconds{ 0 },
                                std::chrono::hours{ 24 });
        m_progress.setOpsCheckpoint(0);

        emit updateRateProbeStarted();
        dispatchComputation(g);
        seconds = std::chrono::duration<double>(steady_clock::now() - startedAt).count();
    }
    catch (const CudaError& e) {
        emit errorOccurred(e.message());
    }
    catch (const std::exception& e) {
        emit errorOccurred(QStringLiteral("Ошибка замера: %1")
                               .arg(QString::fromUtf8(e.what())));
    }

    const quint64 sends = m_probeSends;
    m_probeMode  = false;
    m_probeSends = 0;
    releaseResources();
    // Состояние расчёта после пробы — чужое: она стартовала с последнего слоя.
    initializeRunState(LoadMode::Reset);
    m_cancelled.store(0);

    emit updateRateMeasured(seconds > 0.0 ? double(sends) / seconds : 0.0);
}

// Выводит из настроек и матрицы всё, что нужно для расчёта.
CodeGeometry Worker::describeTask() const
{
    CodeGeometry g;

    g.matrix = m_settings.matrix;
    // Для дуального кода перебор идёт по проверочной матрице, а не по той,
    // что ввёл пользователь.
    if (m_settings.algorithmType == ComputationSettings::DualCode)
        g.matrix = generatorToParity(g.matrix);

    g.numOfRows    = quint64(g.matrix.length());
    g.numOfCols    = quint64(g.matrix[0].length());
    g.wordsPerRow  = (g.numOfCols + 63) / 64;
    g.matrixWords  = g.numOfRows * g.wordsPerRow;
    g.spectrumSize = g.numOfCols + 1;
    g.maxRows      = quint64(m_settings.maxRows);

    g.blocksGpu  = m_settings.compDevSet.blocksGpu;
    g.threadsGpu = m_settings.compDevSet.threadsGpu;
    g.useGpu     = m_settings.compDev == ComputationSettings::ComputeDevice::Gpu;
    g.isLongCode = g.numOfRows > Constants::MAX_SHORT_CODE_LENGTH;

    if (m_settings.algorithmType == ComputationSettings::BrouwerZimmermann)
        planInfoSets(g);

    if (m_settings.algorithmType == ComputationSettings::ProductCode) {
        // Самого произведения в памяти нет — только его размеры, под спектр.
        if (m_settings.matrix2.isEmpty())
            throw std::invalid_argument("код-произведение: не задана вторая компонента");
        const quint64 n1 = quint64(m_settings.matrix.first().length());
        const quint64 n2 = quint64(m_settings.matrix2.first().length());
        g.numOfRows    = quint64(m_settings.matrix.size()) * quint64(m_settings.matrix2.size());
        g.numOfCols    = n1 * n2;
        g.wordsPerRow  = (g.numOfCols + 63) / 64;
        g.matrixWords  = 0;
        g.spectrumSize = g.numOfCols + 1;
        g.useGpu       = false;
        g.isLongCode   = true;
    }

    if (m_settings.algorithmType == ComputationSettings::RandomInfoSets) {
        // На видеокарте короткая матрица живёт в разделяемой памяти блока,
        // длинная — в глобальной; ядро умеет строки до MAX_BLOCKWORDS слов.
        if (g.useGpu && leonSharedBytes(int(g.numOfRows), int(g.numOfCols), int(g.wordsPerRow)) == 0)
            throw std::invalid_argument(
                "стохастический поиск на видеокарте: строка длиннее, чем умеет ядро — выберите CPU");
        // Профиль ключей окна — по самой матрице, и только если окно
        // на этом устройстве вообще допустимо.
        const bool windowAllowed = g.useGpu ? m_windowPolicy.gpu : m_windowPolicy.cpu;
        const Leon::SternProfile profile = windowAllowed ? Leon::sternProfile(g.matrix)
                                                         : Leon::SternProfile();
        const Leon::Plan plan = Leon::plan(int(g.numOfCols), int(g.numOfRows),
                                           m_settings.leonWeight, m_settings.leonMissProbability(),
                                           g.useGpu, &profile, m_windowPolicy);
        g.maxRows           = quint64(plan.rows);
        g.leonWindow        = plan.window;
        g.leonPairs         = plan.window > 0 ? profile.pairs[plan.rows][plan.window] : 0.0;
        g.leonTrials        = plan.trials;
        g.leonWordsPerTrial = plan.costPerTrial;
    }

    g.matrixInGlobalMem =
        g.useGpu && (g.matrixWords > quint64(Constants::MAX_CONST_WORDS));

    return g;
}

void Worker::planInfoSets(CodeGeometry& g) const
{
    int words = 0;
    const std::vector<quint64> packed = InfoSets::packRows(g.matrix, words);
    const int rows = int(g.numOfRows);
    const int cols = int(g.numOfCols);

    // Все матрицы должны поместиться в константную память видеокарты: у
    // короткого пути другого места для них нет. Предел один для обоих
    // устройств, чтобы запись, сделанная на одном, продолжалась на другом.
    const int fit     = std::max(1, Constants::MAX_CONST_WORDS / std::max(1, rows * words));
    const int maxSets = std::min(Constants::MAX_INFO_SETS, fit);

    std::vector<InfoSets::InfoSet> sets;
    if (!m_resumedInfoSets.isEmpty()) {
        if (!InfoSets::rebuild(packed.data(), rows, cols, words, m_resumedInfoSets, sets))
            throw std::invalid_argument(
                "множества из сохранения не подходят к матрице");
    }
    else {
        sets = InfoSets::find(packed.data(), rows, cols, words, maxSets);
        if (sets.empty())
            throw std::invalid_argument(
                "строки матрицы зависимы: алгоритму Брауэра–Циммермана нужна матрица полного ранга");
        std::vector<int> overlaps;
        for (const InfoSets::InfoSet& set : sets)
            overlaps.push_back(set.overlap);
        sets.resize(size_t(InfoSets::setsForWeight(overlaps, m_settings.bzWeight, rows, cols)));
    }
    if (int(sets.size()) > maxSets)
        throw std::invalid_argument("множеств больше, чем помещается в память видеокарты");

    g.setCount = int(sets.size());
    g.setOverlaps.clear();
    g.setMasks.clear();
    g.setRows.clear();
    g.setColumns.clear();
    for (const InfoSets::InfoSet& set : sets) {
        g.setOverlaps.push_back(set.overlap);
        g.setMasks.insert(g.setMasks.end(), set.mask.begin(), set.mask.end());
        g.setRows.insert(g.setRows.end(), set.rows.begin(), set.rows.end());
        g.setColumns.append(QVector<int>(set.columns.begin(), set.columns.end()));
    }
    g.matrixWords     = quint64(g.setRows.size());
    g.maxRows         = quint64(InfoSets::rowsForWeight(g.setOverlaps, m_settings.bzWeight, rows, cols));
    g.guaranteedBelow = InfoSets::guaranteedBelow(g.setOverlaps, int(g.maxRows), rows, cols);
}

// Упаковывает матрицу в биты и раскладывает буферы по памяти.
void Worker::prepareBuffers(const CodeGeometry& g)
{
    if (g.matrixInGlobalMem) {
        /* ДОПИСАТЬ КОПИРОВАНИЕ МАТРИЦЫ В ПАМЯТЬ ДЛЯ КОРОТКИХ КОДОВ */
        if (!g.isLongCode)
            throw std::invalid_argument("матрица слишком большая для короткого кода");
        m_buffers->d_matrix.allocate(g.matrixWords);
    }

    // calloc внутри, поэтому матрица уже обнулена
    m_buffers->h_matrix.allocate(g.matrixWords, HostBuffer<quint64>::Kind::Paged);
    if (!g.setRows.empty()) {
        // Брауэр–Циммерман: матрицы множеств уже упакованы планом.
        std::copy(g.setRows.begin(), g.setRows.end(), m_buffers->h_matrix.get());
    }
    else {
        for (quint64 i = 0; i < g.numOfRows; ++i) {
            quint64* rowData = m_buffers->h_matrix.get() + i * g.wordsPerRow;
            const QString& row = g.matrix[int(i)];
            for (quint64 j = 0; j < g.numOfCols; ++j)
                if (row.at(int(j)) == QLatin1Char('1'))
                    rowData[j / 64] |= (1ull << (j % 64));
        }
    }

    // Перебору по слоям нужна таблица биноминальных коэффициентов
    if (m_settings.layered()) {
        m_binomTable = g.isLongCode
            // Для длинных кодов строим только часть таблицы
            ? BinomTable(g.numOfRows, g.maxRows)
            // Для коротких — всю. Не оптимально, но работает.
            : BinomTable(Constants::MAX_SHORT_CODE_LENGTH, Constants::MAX_SHORT_CODE_LENGTH);
    }

    // Спектр на хосте. Для GPU нужна pinned-память — иначе не работает
    // асинхронное копирование; для CPU обычная, cudaMallocHost без видеокарты
    // недоступен.
    m_buffers->h_spectrum.allocate(g.spectrumSize, g.useGpu ? HostBuffer<quint64>::Kind::Pinned
                                                 : HostBuffer<quint64>::Kind::Paged);
    // Кольцо снимков нужно только видеокарте: на CPU спектр и так лежит в
    // h_spectrum, копировать его неоткуда.
    if (g.useGpu)
        m_buffers->spectrumRing.allocate(g.spectrumSize);
    else
        m_buffers->spectrumRing.reset();
    if (m_resumeSpectrum) {
        // Продолжаем с чекпоинта — переносим накопленный спектр
        for (quint64 i = 0; i < g.spectrumSize; ++i)
            m_buffers->h_spectrum[i] = m_runState.spectrum.at(int(i));
    } else {
        m_buffers->h_spectrum.fillZero();
    }

    if (!g.useGpu)
        return;

    if (g.matrixInGlobalMem)
        CUDA_CALL(cudaMemcpy(m_buffers->d_matrix.get(), m_buffers->h_matrix.get(),
                             g.matrixWords * Constants::WORD_SIZE, cudaMemcpyHostToDevice));
    else
        CUDA_CALL(copyMatrixToConstant(m_buffers->h_matrix.get(), g.matrixWords));
    if (g.setCount > 1)
        CUDA_CALL(copyMasksToConstant(g.setMasks.data(), g.setCount, int(g.wordsPerRow)));

    m_buffers->d_spectrum.allocate(g.spectrumSize);
    if (m_resumeSpectrum)
        CUDA_CALL(cudaMemcpy(m_buffers->d_spectrum.get(), m_buffers->h_spectrum.get(),
                             g.spectrumSize * sizeof(quint64), cudaMemcpyHostToDevice));
    else
        m_buffers->d_spectrum.fillZero();

    m_buffers->stream.create();

    // Ядро коротких кодов читает таблицу как binomTable[n * 64 + k]. BinomTable
    // хранит её плоско ровно с таким шагом, поэтому копируем как есть, без
    // промежуточного «уплощения».
    if (m_settings.layered() && !g.isLongCode) {
        Q_ASSERT(m_binomTable.stride() == Constants::MAX_SHORT_CODE_LENGTH + 1);
        m_buffers->d_binomTable.allocate(Constants::BINOM_TABLE_SIZE_FOR_SHORT_CODES);
        CUDA_CALL(cudaMemcpy(m_buffers->d_binomTable.get(), m_binomTable.data(),
                             m_binomTable.bytes(), cudaMemcpyHostToDevice));
    }
}

// Выбор вычислительной функции: алгоритм, устройство, длина кода.
void Worker::tuneGrid(CodeGeometry& g)
{
    if (!m_settings.autoTuneGrid || !g.useGpu)
        return;
    // У случайного поиска своё ядро и своя сетка — блок на попытку; подбор
    // здесь мерил бы чужое ядро, да ещё по пустой таблице биномов.
    if (m_settings.algorithmType == ComputationSettings::RandomInfoSets)
        return;

    GridTuneTask task;
    task.numOfCols   = int(g.numOfCols);
    task.numOfRows   = int(g.numOfRows);
    task.wordsPerRow = int(g.wordsPerRow);
    task.binomTable  = m_buffers->d_binomTable.get();
    task.chunkSize   = g.chunkSize;
    task.minWorthSeconds = m_tuneThresholdSec;
    task.userGrid    = { g.blocksGpu, g.threadsGpu };
    task.verbose     = m_tuneVerbose;
    task.slot.setCount = g.setCount;

    const bool gray = !m_settings.layered();
    task.kernel = g.isLongCode ? GridTuneTask::Kernel::XorLong
                : gray         ? GridTuneTask::Kernel::Gray
                               : GridTuneTask::Kernel::XorShort;

    // Отдельный буфер спектра: замер не имеет права попасть в настоящий.
    DeviceBuffer<quint64> scratch;
    scratch.allocate(g.spectrumSize);
    scratch.fillZero();
    task.scratchSpectrum = scratch.get();

    // Слой, на котором идёт замер, и его размер. У кода Грея слоёв нет —
    // маски нумеруются сплошь.
    quint64 layerRank = 0;
    if (task.kernel == GridTuneTask::Kernel::Gray) {
        task.availableMasks = 1ULL << g.numOfRows;
        task.totalMasks     = task.availableMasks;
    }
    else {
        // Меряем на самом населённом слое: там расчёт и проведёт почти всё
        // время, а стоимость маски зависит от числа складываемых строк.
        quint64 bestCount = 0;
        for (quint64 r = 0; r <= g.maxRows && r <= g.numOfRows; ++r) {
            const quint64 count = m_binomTable(g.numOfRows, r);
            if (count > bestCount) {
                bestCount      = count;
                task.numOfOnes = r;
            }
        }
        task.availableMasks = bestCount;
        task.totalMasks     = totalLayerOps(g);
        layerRank           = bestCount / 2;
    }

    // Длинному пути нужны настоящие стартовые маски: его ядро читает их из
    // буфера, а не выводит из номера чанка.
    HostBuffer<int16_t>   h_tuneSlots;
    DeviceBuffer<int16_t> d_tuneSlots;

    if (task.kernel == GridTuneTask::Kernel::XorLong) {
        // Столько масок на нить хватает, чтобы стартовая сборка кодового слова
        // занимала около процента: боевые 4096 растянули бы замер на секунды.
        task.measureMasksPerThread = 256;

        // Слотов — под самого крупного кандидата; меньшие берут префикс.
        // Имя не slots: так называется макрос Qt из qobjectdefs.h.
        quint64 slotCount = quint64(Constants::MAX_TUNE_BLOCKS)
                      * quint64(Constants::MAX_TUNE_THREADS);
        if (task.availableMasks < layerRank + slotCount * task.measureMasksPerThread) {
            const quint64 room = task.availableMasks > layerRank
                               ? (task.availableMasks - layerRank) / task.measureMasksPerThread
                               : 0;
            slotCount = room;
        }
        if (slotCount == 0)
            return;

        h_tuneSlots.allocate(slotCount * Constants::MAX_POSITIONS,
                             HostBuffer<int16_t>::Kind::Paged);
        d_tuneSlots.allocate(slotCount * Constants::MAX_POSITIONS);

        int16_t* const host = h_tuneSlots.get();
        const int rows = int(g.numOfRows);
        const int ones = int(task.numOfOnes);

        #pragma omp parallel for schedule(static)
        for (long long i = 0; i < (long long)slotCount; ++i) {
            Combinations::unrankPositions(layerRank + quint64(i) * task.measureMasksPerThread,
                                          rows, ones, host + i * Constants::MAX_POSITIONS,
                                          Constants::MAX_POSITIONS, m_binomTable);
        }

        CUDA_CALL(cudaMemcpy(d_tuneSlots.get(), host,
                             slotCount * Constants::MAX_POSITIONS * sizeof(int16_t),
                             cudaMemcpyHostToDevice));

        task.startPositions   = d_tuneSlots.get();
        task.filledStartMasks = slotCount;
        task.matrixGlobal     = g.matrixInGlobalMem ? m_buffers->d_matrix.get() : nullptr;
    }

    const LaunchGrid grid = tuneLaunchGrid(task, m_buffers->stream.get());
    if (!grid.isValid())
        return;   // подбор отказался — остаёмся на настройках

    // Сигнал уходит и тогда, когда победили настройки пользователя: это
    // не пустой результат, а подтверждение замером, и видеть его полезно.
    g.blocksGpu  = grid.blocks;
    g.threadsGpu = grid.threads;
    emit gridTuned(grid.blocks, grid.threads);
}

void Worker::dispatchComputation(const CodeGeometry& g)
{
    // Дуальный код считается по проверочной матрице тем же кодом Грея
    const bool gray = !m_settings.layered()
                   && m_settings.algorithmType != ComputationSettings::RandomInfoSets;

    // Код Грея перебирает 2^k масок в одном 64-битном слове, поэтому длиннее
    // 63 строк не бывает. Раньше это проверял только диалог настроек, а прямой
    // вызов Worker давал сдвиг на 64 и больше — неопределённое поведение.
    if (gray && g.isLongCode)
        throw std::invalid_argument(
            "код Грея неприменим: больше 63 строк не помещается в маску");

    if (m_settings.algorithmType == ComputationSettings::RandomInfoSets)
        computeLeon(g);
    else if (gray)
        g.useGpu ? computeGrayGpu(g)
                 : computeGrayCpu(g);
    else if (g.isLongCode)
        g.useGpu ? computeXorGpuLong(g)
                 : computeXorCpuLong(g);
    else
        g.useGpu ? computeXorGpuShort(g)
                 : computeXorCpuShort(g);
}

// Забирает итоговый спектр, рассылает сигналы и освобождает ресурсы.
void Worker::finishComputation(const CodeGeometry& g, steady_clock::time_point startedAt)
{
    if (m_cancelled.load()) {
        initializeRunState(LoadMode::Reset);
        emit finished(Constants::ERROR_OCCURRED);
        emit progressChanged(0);
        updateSpectrum(int(g.numOfCols));
        releaseResources();
        return;
    }

    // Случайный поиск копит спектр на хосте, d_spectrum у него пустой —
    // забирать оттуда нечего, это затёрло бы найденное нулями.
    if (g.useGpu && m_settings.algorithmType != ComputationSettings::RandomInfoSets) {
        CUDA_CALL(cudaDeviceSynchronize());
        CUDA_CALL(cudaMemcpy(m_buffers->h_spectrum.get(), m_buffers->d_spectrum.get(),
                             g.spectrumSize * sizeof(quint64), cudaMemcpyDeviceToHost));
    }

    // Автосохранение остаётся и после успеха. Слои по числу складываемых строк
    // независимы и идут по возрастанию, поэтому досчитанный до maxRows спектр —
    // это ровно начало расчёта до большего maxRows. Состояние помечается как
    // «всё до maxRows пройдено», и следующий запуск продолжит со следующего
    // слоя, а не с нуля.
    if (m_settings.layered()) {
        m_runState.rOffset     = g.maxRows + 1;
        m_runState.chunkOffset = 0;
    }
    else if (m_settings.algorithmType == ComputationSettings::RandomInfoSets
             || m_settings.algorithmType == ComputationSettings::ProductCode) {
        // Такая запись не продолжается — хранится только итог.
        m_runState.rOffset     = 0;
        m_runState.chunkOffset = 0;
    }
    else {
        // У кода Грея слоёв нет: пройденным считается весь диапазон масок.
        m_runState.rOffset     = 0;
        m_runState.chunkOffset = 1ULL << g.numOfRows;
    }
    m_runState.doneOps    = m_progress.doneOps();
    m_runState.elapsedSec = duration_cast<seconds>(steady_clock::now() - startedAt).count();

    // Пишется до преобразования Мак-Вильямс: в записи должен лежать сырой
    // спектр перебираемой матрицы, с него и продолжают.
    makeCheckpoint(int(g.numOfCols), true);

    // Дуальный расчёт даёт спектр проверочной матрицы — исходный получается
    // из него преобразованием Мак-Вильямс. Считается один раз: из него и
    // итог, и то, что видит пользователь.
    const bool dual = m_settings.algorithmType == ComputationSettings::Algorithm::DualCode;
    const std::vector<mpz_class> original = dual
        ? macWilliams(m_runState.spectrum.constData(), int(g.numOfCols), int(g.numOfRows))
        : std::vector<mpz_class>();

    // Итог — на случай, если этот расчёт вложенный (компонента произведения).
    m_finalSpectrum  = dual ? saturatedCounts(original) : m_runState.spectrum;
    m_finalExactUpTo = g.guaranteedBelow > 0 ? g.guaranteedBelow - 1
                     : m_settings.algorithmType == ComputationSettings::ProductCode ? m_productExactUpTo
                     : int(g.numOfCols);

    initializeRunState(LoadMode::Reset);

    if (dual)
        publishSpectrum(spectrumCounts(original));
    else
        updateSpectrum(int(g.numOfCols));

    // Последняя оценка — по итогу, а не по последнему промежуточному
    // замеру: иначе в панели остаётся «перебрано 47,6 из 48,0 млрд,
    // осталось 1 мин» при состоянии «готово».
    reportEstimate();
    emit progressChanged(100);
    emit finished(int(duration_cast<seconds>(steady_clock::now() - startedAt).count()));

    releaseResources();
}

void Worker::computeSpectrumImpl()
{
    CodeGeometry g = describeTask();

    // Что записывать в автосохранение и что показать пользователю: глубину
    // перебора и множества он не задавал, они выведены из веса и матрицы.
    m_activeInfoSets = g.setColumns;
    m_activeMaxRows  = int(g.maxRows);
    m_activeTrials   = 0;
    m_displayUpToWeight = m_settings.algorithmType == ComputationSettings::BrouwerZimmermann
                          ? m_settings.bzWeight : -1;
    if (g.guaranteedBelow > 0)
        emit planReady(g.setCount, int(g.maxRows), g.guaranteedBelow - 1);

    // Спектр мог прийти из чекпоинта — тогда размер уже верный
    if (quint64(m_runState.spectrum.size()) != g.spectrumSize)
        m_runState.spectrum.resize(int(g.spectrumSize));

    omp_set_num_threads(m_settings.compDevSet.threadsCpu);

    // Частоты обновления из настроек. Сам отсчёт запускает вычислительная
    // функция: только она знает общее число операций.
    m_progress.setIntervals(
        std::chrono::milliseconds{ m_settings.timeIntSet.updateSpectrumInterval },
        std::chrono::seconds{ m_settings.timeIntSet.saveSpectrumInterval });
    m_progress.setOpsCheckpoint(m_checkpointEveryOps);

    const auto startedAt = steady_clock::now() - std::chrono::seconds(m_runState.elapsedSec);

    // Код произведения не перебирает собственную матрицу: ни буферов, ни
    // сетки ему не нужно, только спектр на хосте.
    if (m_settings.algorithmType == ComputationSettings::ProductCode) {
        m_buffers->h_spectrum.allocate(g.spectrumSize, HostBuffer<quint64>::Kind::Paged);
        m_buffers->h_spectrum.fillZero();
        computeProduct(g);
        finishComputation(g, startedAt);
        return;
    }

    prepareBuffers(g);
    tuneGrid(g);

    dispatchComputation(g);
    finishComputation(g, startedAt);
}

// Освобождает всё, что выделено под расчёт.
//
// Раньше это были два почти одинаковых блока — для успешного завершения и для
// отмены, — и они успели разойтись: на пути отмены оставались d_spectrum и
// d_binomTable. Теперь порядок один, а сами буферы владеющие, так что даже
// пропущенный здесь вызов не приводит к утечке: их освободит деструктор.
void Worker::releaseResources()
{
    m_buffers->h_spectrum.reset();
    m_buffers->h_matrix.reset();
    m_binomTable = BinomTable();

    m_buffers->d_spectrum.reset();
    m_buffers->d_matrix.reset();
    m_buffers->d_binomTable.reset();

    m_buffers->stream.reset();
}


void Worker::pause()
{
    m_paused.store(1);
    if (Worker* sub = m_activeSub.load())
        sub->pause();
}

void Worker::resume()
{
    m_paused.store(0);
    if (Worker* sub = m_activeSub.load())
        sub->resume();
}

void Worker::cancel()
{
    m_cancelled.store(1);
    if (Worker* sub = m_activeSub.load())
        sub->cancel();
}
void Worker::uncancel()
{
    m_cancelled.store(0);
}

bool Worker::isCancelled()
{
    return (bool)m_cancelled.load();
}


void Worker::setGridTuningThreshold(double seconds)
{
    m_tuneThresholdSec = seconds;
}

void Worker::setGridTuningVerbose(bool on)
{
    m_tuneVerbose = on;
}

void Worker::setCheckpointOpsPolicy(quint64 everyOps, quint64 stopAfter)
{
    m_checkpointEveryOps = everyOps;
    m_stopAfterOps       = stopAfter;
}

void Worker::setSettings(const ComputationSettings& newSettings) {
    m_settings = newSettings;
}

void Worker::initializeRunState(LoadMode lm)
{
    // Если сбрасываем состояние
    if ( lm == LoadMode::Reset ) {
        m_runState.rOffset = 0;
        m_runState.chunkOffset = 0;
        m_runState.elapsedSec = 0;
        m_runState.doneOps = 0;
        m_runState.spectrum.clear();
        m_resumeSpectrum = false;
        m_resumedInfoSets.clear();
        return;
    }
    else {
        AutosaveRecord record;
        if (!m_autosave.load(m_settings.matrix, m_settings.algorithmType, record)
            || !canResume(record, m_settings)) {
            initializeRunState(LoadMode::Reset);
            return;
        }

        m_runState = record.state;
        // Продолжать Брауэра–Циммермана можно только по множествам записи
        m_resumedInfoSets = record.infoSets;
        // Накопленный спектр перенесётся в буферы расчёта (prepareBuffers)
        m_resumeSpectrum = true;
    }
}
