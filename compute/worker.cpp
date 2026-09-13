#include "worker.h"
#include "gridtuner.h"
#include "leonkernel.cuh"

#include <algorithm>
#include <cmath>
#include <limits>

Worker::Worker(QObject *parent)
    : QObject(parent)
{
}

Worker::~Worker()
{
}
// Для длинных кодов
void generateStartPositions(
    uint64_t rank,
    int numOfRows,
    int numOfOnes,
    int16_t* slot,
    const BinomTable& C
) {
    int x = 0;
    for (int i = 0; i < numOfOnes; ++i) {
        for (int v = x; v <= numOfRows - numOfOnes + i; ++v) {
            uint64_t cnt = C(numOfRows - v - 1, numOfOnes - i - 1);
            if (rank < cnt) {
                slot[i] = (int16_t)v;
                x = v + 1;
                break;
            }
            rank -= cnt;
        }
    }

    for (int i = numOfOnes; i < Constants::MAX_POSITIONS; ++i)
        slot[i] = 0;
}
// Получение битовой маски длины k с r единицами с индексом rank
static quint64 unrankCombination( unsigned K, unsigned R, quint64 rank, const BinomTable& binomTable )
{
    if (R == 0) return 0ULL;

    quint64 mask = 0ULL;
    unsigned nextPos = 0;

    for (unsigned i = 0; i < R; ++i)
    {
        for (unsigned pos = nextPos; pos < K; ++pos)
        {
            unsigned remainingPositions = K - pos - 1;
            unsigned remainingToChoose = R - i - 1;

            quint64 count = 0;

            if (remainingToChoose == 0)
                count = 1;
            else if (remainingPositions >= remainingToChoose)
                count = binomTable(remainingPositions, remainingToChoose);

            if (rank >= count)
            {
                rank -= count;
            }
            else
            {
                mask |= (1ULL << pos);
                nextPos = pos + 1;
                break;
            }
        }
    }

    return mask;
}
// Следующая маска с тем же числом единиц в возрастающем числовом порядке —
// приём Госпера. Деление из классической записи заменено сдвигом: младший
// установленный бит есть степень двойки, его позиция и есть величина сдвига.
// Вызывать только при v != 0 и когда следующая комбинация существует.
static inline quint64 gosperNext(quint64 v)
{
    unsigned long t;
    _BitScanForward64(&t, v);
    const quint64 rr = v + (1ULL << t);
    return rr | ((v ^ rr) >> (t + 2));
}

// Разворот всех 64 бит.
static inline quint64 reverseBits64(quint64 v)
{
    v = ((v >> 1)  & 0x5555555555555555ULL) | ((v & 0x5555555555555555ULL) << 1);
    v = ((v >> 2)  & 0x3333333333333333ULL) | ((v & 0x3333333333333333ULL) << 2);
    v = ((v >> 4)  & 0x0F0F0F0F0F0F0F0FULL) | ((v & 0x0F0F0F0F0F0F0F0FULL) << 4);
    v = ((v >> 8)  & 0x00FF00FF00FF00FFULL) | ((v & 0x00FF00FF00FF00FFULL) << 8);
    v = ((v >> 16) & 0x0000FFFF0000FFFFULL) | ((v & 0x0000FFFF0000FFFFULL) << 16);
    return (v >> 32) | (v << 32);
}

// Разворот младших k бит: бит p переходит в позицию k-1-p.
static inline quint64 reverseLowBits(quint64 v, quint64 k)
{
    return reverseBits64(v) >> (64 - k);
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
// Кусок слоя, уходящий в один запуск ядра или один параллельный проход.
//
// Слой r у Брауэра–Циммермана — это C(k, r) комбинаций на каждое множество,
// подряд: сначала все комбинации первого, потом второго и так далее. Номер
// в слое (chunkOffset) сквозной, поэтому чекпоинты устроены так же, как в
// обычном расчёте. Кусок никогда не пересекает границу множества: ядру
// нужна одна матрица и один номер множества на запуск.
struct LayerSlice
{
    MatrixSlot slot;
    quint64    offset = 0;   // номер первой комбинации внутри своего множества
    quint64    size   = 0;
};

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

// Правило единственности Брауэра–Циммермана на хосте — то же, что bzKeep в
// ядре: слово из r строк множества setIndex засчитывается, если прежние
// множества видят у него больше r единиц, а последующие — не меньше r.
static inline bool bzKeepHost(const quint64* codeword, const CodeGeometry& g,
                              int r, int setIndex)
{
    for (int i = 0; i < g.setCount; ++i) {
        if (i == setIndex)
            continue;
        const quint64* mask = g.setMasks.data() + size_t(i) * g.wordsPerRow;
        int ones = 0;
        for (quint64 w = 0; w < g.wordsPerRow; ++w)
            ones += int(__popcnt64(codeword[w] & mask[w]));
        if (i < setIndex ? ones <= r : ones < r)
            return false;
    }
    return true;
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

quint64 Worker::totalCombinations(quint64 k, quint64 maxComb) const
{
    if (maxComb > k) maxComb = k;

    quint64 sum = 0;
    for (quint64 r = 0; r <= maxComb; ++r) {
        const quint64 term = binomTable(k, r);
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
    if (probeMode)
        return;
    progress.markEstimate();
    runState.elapsedSec = progress.elapsedSec();
    emit updateRemainingMinutes(int(progress.elapsedSec()),
                                progress.minutesLeft(),
                                progress.speed(),
                                progress.doneOps(),
                                progress.totalOps());
}

void Worker::reportProgressBar()
{
    if (probeMode)
        return;
    progress.markBar();
    emit updateInfoPBR(progress.percent());
}

// Сохраняет чекпоинт для GPU-путей.
//
// Спектр забирается синхронно, а не через уже начатое асинхронное копирование:
// нужно, чтобы сохранённые данные точно соответствовали посчитанным чанкам.
//
// false означает ошибку CUDA. Раньше в этом случае расчёт просто обрывался и
// рапортовал об успехе — пользователь получал неполный спектр как готовый.
bool Worker::saveGpuCheckpoint(cudaStream_t s, int numOfCols,
                               quint64 rOffset, quint64 chunkOffset)
{
    progress.markCheckpoint();

    cudaError_t err = cudaStreamSynchronize(s);
    if (err == cudaSuccess)
        err = cudaMemcpy(h_spectrum.get(), d_spectrum.get(), (numOfCols + 1) * sizeof(quint64),
                         cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
        emit errorOccurred(QStringLiteral("Ошибка CUDA при сохранении состояния: %1")
                               .arg(QString::fromLatin1(cudaGetErrorString(err))));
        return false;
    }

    runState.rOffset     = rOffset;
    runState.chunkOffset = chunkOffset;
    makeCheckpoint(numOfCols);
    stopIfOpsLimitReached();
    return true;
}

// Чекпоинт для CPU-путей: спектр уже в h_spectrum, забирать его неоткуда.
// Копированием в runState.spectrum занимается makeCheckpoint().
void Worker::saveCpuCheckpoint(int numOfCols, quint64 rOffset, quint64 chunkOffset)
{
    progress.markCheckpoint();
    runState.rOffset     = rOffset;
    runState.chunkOffset = chunkOffset;
    makeCheckpoint(numOfCols);
    stopIfOpsLimitReached();
}

// Прерывание сразу после сохранения: состояние на диске согласовано, и
// возобновление начнётся ровно с той точки, которую записал чекпоинт.
void Worker::setLiveIntervals(int spectrumMs, int checkpointSeconds)
{
    if (spectrumMs <= 0 || checkpointSeconds <= 0)
        return;
    progress.setIntervals(std::chrono::milliseconds{ spectrumMs },
                          std::chrono::seconds{ checkpointSeconds });
}

void Worker::stopIfOpsLimitReached()
{
    if (stopAfterOps > 0 && progress.doneOps() >= stopAfterOps)
        cancelled.store(1);
}

// Возвращает false, если во время паузы расчёт отменили.
bool Worker::waitWhilePaused()
{
    while (paused.load() != 0) {
        if (cancelled.load()) return false;
        QThread::msleep(50);
    }
    return !cancelled.load();
}

void Worker::computeSpectrumGpuNoGrayShort(const CodeGeometry& g)
{
    const quint64 numOfRows       = g.numOfRows;
    const quint64 numOfCols       = g.numOfCols;
    const quint64 wordsPerRow     = g.wordsPerRow;
    const quint64 chunkSize       = g.chunkSize;
    const int     blockCount      = g.blocksGpu;
    const int     threadsPerBlock = g.threadsGpu;
    const quint64 maxComb         = g.maxRows;

    progress.begin(totalLayerOps(g), runState.doneOps, runState.elapsedSec);

    // rOffset — число единиц в маске; счётчик обязан быть беззнаковым, иначе
    // при сравнении с maxComb получается знаковое/беззнаковое сравнение.
    for (quint64 r = runState.rOffset; r <= maxComb; r++)
    {
        quint64 startOffset = 0;

        // Слой — C(k, r) комбинаций на каждое из множеств, подряд.
        const quint64 perSet = binomTable(numOfRows, r);
        const quint64 curOps = perSet * g.setCount;

        if (r == runState.rOffset)
            startOffset = runState.chunkOffset;
        // Разбиваем на чанки. Чанк не пересекает границу множества, поэтому
        // бывает короче обычного, и шаг цикла — его фактический размер.
        for ( quint64 offset = startOffset, next = 0; offset < curOps; offset = next )
        {
            const LayerSlice slice = sliceLayer(g, perSet, offset, chunkSize);
            const quint64 thisChunkSize = slice.size;
            next = offset + thisChunkSize;
            if (!waitWhilePaused())
                break;

            // Запуск ядра
            launchSpectrumKernelShort(
                d_spectrum.get(),
                d_binomTable.get(),
                blockCount,             // Число блоков
                threadsPerBlock,       // Число нитей на блок
                stream.get(),
                numOfCols,                     // Длина строки матрицы в битах
                numOfRows,                     // Число строк матрицы
                wordsPerRow,            // Число слов на одну строку матрицы
                slice.offset,          // Смещение в комбинациях внутри множества
                thisChunkSize,         // Размер чанка
                r,                     // Число единиц в битовой маске (число складываемых строк)
                slice.slot
            );
            progress.addOps(thisChunkSize);
            runState.doneOps = progress.doneOps();

            const ProgressTracker::Due due = progress.due();

            if (due.estimate)
                reportEstimate();

            // Метка двигается только когда копия реально встала в очередь:
            // иначе при занятом кольце следующая попытка откладывалась бы на
            // целый интервал.
            if (due.spectrum && spectrumRing.enqueue(d_spectrum.get(), stream.get()))
                progress.markSpectrum();
            if (const quint64* snapshot = spectrumRing.takeReady())
                updateSpectrumFrom(snapshot, int(numOfCols));
            if (due.bar)
                reportProgressBar();

            if (due.checkpoint) {
                if (!saveGpuCheckpoint(stream.get(), numOfCols, r, offset + thisChunkSize))
                    return;
            }
        }
        if (cancelled.load()) {
            break;
        }
    }
    // Финальная копия спектра
    CUDA_CALL(cudaMemcpyAsync(
        h_spectrum.get(),
        d_spectrum.get(),
        (numOfCols + 1) * sizeof(quint64),
        cudaMemcpyDeviceToHost,
        stream.get()));
    CUDA_CALL(cudaStreamSynchronize(stream.get()));

    updateSpectrum(numOfCols);
}
void Worker::computeSpectrumGpuGrayShort(const CodeGeometry& g)
{
    const quint64 numOfRows       = g.numOfRows;
    const quint64 numOfCols       = g.numOfCols;
    const quint64 wordsPerRow     = g.wordsPerRow;
    const quint64 chunkSize       = g.chunkSize;
    const int     blockCount      = g.blocksGpu;
    const int     threadsPerBlock = g.threadsGpu;

    const quint64 totalOps    = 1ULL << numOfRows;

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);
    // Обновление на случай, когда загружаем чекпоинт
    if (!probeMode)
        emit updateInfoPBR(progress.percent());

    for (quint64 chunkOffset = runState.chunkOffset; chunkOffset < totalOps; chunkOffset += chunkSize) {
        quint64 thisChunkSize = qMin(chunkSize, totalOps - chunkOffset);
        if (!waitWhilePaused())
            break;
        launchSpectrumKernelGrayShort(
            blockCount,
            threadsPerBlock,
            stream.get(),
            d_spectrum.get(),
            numOfCols,
            numOfRows,
            (int)wordsPerRow,
            chunkOffset,
            thisChunkSize
        );
        progress.addOps(thisChunkSize);
        runState.doneOps = progress.doneOps();

        const ProgressTracker::Due due = progress.due();

        if (due.estimate)
            reportEstimate();

        if (due.spectrum && spectrumRing.enqueue(d_spectrum.get(), stream.get()))
            progress.markSpectrum();
        if (const quint64* snapshot = spectrumRing.takeReady())
            updateSpectrumFrom(snapshot, int(numOfCols));
        if (due.bar)
            reportProgressBar();

        if (due.checkpoint) {
            // Код Грея идёт сплошной нумерацией масок, слоёв по числу единиц
            // нет — rOffset здесь всегда 0.
            if (!saveGpuCheckpoint(stream.get(), numOfCols, 0, chunkOffset + thisChunkSize))
                return;
        }
    }

    // Финальная копия спектра
    CUDA_CALL(cudaMemcpyAsync(
        h_spectrum.get(),
        d_spectrum.get(),
        (numOfCols + 1) * sizeof(quint64),
        cudaMemcpyDeviceToHost,
        stream.get()));
    CUDA_CALL(cudaStreamSynchronize(stream.get()));

    updateSpectrum(numOfCols);
}

void Worker::computeSpectrumGpuNoGrayLong(const CodeGeometry& g)
{
    const quint64 numOfRows       = g.numOfRows;
    const quint64 numOfCols       = g.numOfCols;
    const quint64 wordsPerRow     = g.wordsPerRow;
    const int     blockCount      = g.blocksGpu;
    const int     threadsPerBlock = g.threadsGpu;
    const quint64 maxComb         = g.maxRows;

    quint64 totalOps = totalLayerOps(g);

    // Общее число нитей, запущенных на видеокарте
    uint64_t maxThreads = static_cast<uint64_t>(blockCount) * static_cast<uint64_t>(threadsPerBlock);
    if (maxThreads == 0) maxThreads = 1;

    // Максимальное число единиц в маске
    const size_t slotElems = Constants::MAX_POSITIONS;
    // Размер массива позиций (размер одной маски)
    const size_t slotBytes = slotElems * sizeof(int16_t);
    // Размер массива начальных масок
    const size_t bytesPerChunk = maxThreads * slotBytes;

    // Максимальное число масок, обрабатываемых одним потоком
    const uint64_t masksPerThread = 1ULL << 12; 

    // Стартовые массивы масок.
    //
    // На хосте их два: пока с одного идёт копирование на устройство, хост
    // заполняет второй. Иначе он затирал бы данные под работающим DMA, и
    // приходилось бы после каждой итерации дожидаться устройства целиком.
    //
    // На устройстве достаточно одного: копия следующей итерации ставится в
    // очередь того же потока после ядра текущей и раньше не начнётся.
    const size_t slotsPerBuffer = maxThreads * slotElems;

    HostBuffer<int16_t>   h_slots;
    DeviceBuffer<int16_t> d_slots;
    h_slots.allocate(2 * slotsPerBuffer, HostBuffer<int16_t>::Kind::Pinned);
    d_slots.allocate(slotsPerBuffer);

    // Событие на каждый буфер: отмечает конец копирования именно из него.
    CudaEvent slotsCopied[2];
    slotsCopied[0].create();
    slotsCopied[1].create();
    bool slotsBusy[2] = { false, false };
    int  slotsBuf     = 0;

    #ifdef _DEBUG
    // Счётчик обработанных масок для сверки. Раньше он не освобождался вовсе.
    HostBuffer<uint64_t>   h_maskCounter;
    DeviceBuffer<uint64_t> d_maskCounter;
    h_maskCounter.allocate(1, HostBuffer<uint64_t>::Kind::Pinned);
    d_maskCounter.allocate(1);
    #endif
    // Собственный поток, а не поле класса: имя намеренно отличается, чтобы
    // не перекрывать Worker::stream и не синхронизировать по ошибке чужой.
    CudaStream localStream;
    localStream.create();

    // Размер чанка в масках
    const quint64 chunkSizeTarget = maxThreads * masksPerThread;

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    // Внешний цикл по числу единиц в маске
    for (quint64 r = runState.rOffset; r <= maxComb; ++r) {

        quint64 startOffset = 0;

        // Количество масок с r единицами — на каждое из множеств, подряд
        const quint64 perSet = binomTable(numOfRows, r);
        const quint64 curOps = perSet * g.setCount;
        if (curOps == 0) continue;

        if (r == runState.rOffset)
            startOffset = runState.chunkOffset;

        // Внутренний цикл по чанку. Чанк не пересекает границу множества,
        // поэтому шаг цикла — его фактический размер.
        for (quint64 chunkOffset = startOffset, next = 0; chunkOffset < curOps; chunkOffset = next) {
            while (paused.load() != 0) {
                QThread::msleep(50);
                if (cancelled.load()) break;
            }
            const LayerSlice slice = sliceLayer(g, perSet, chunkOffset, chunkSizeTarget);
            const quint64 chunkSize = slice.size;
            next = chunkOffset + chunkSize;

            // Фактическое число нитей, участвующих в вычислениях
            quint64 numStartMasks = (chunkSize + masksPerThread - 1ULL) / masksPerThread;
            if (numStartMasks == 0) continue;

            // Ждём, пока освободится тот буфер, который сейчас будем
            // переписывать. На второй итерации после его использования
            // копирование давно закончилось, так что ожидание холостое.
            if (slotsBusy[slotsBuf])
                CUDA_CALL(cudaEventSynchronize(slotsCopied[slotsBuf].get()));

            // Имя не slots: так называется макрос Qt из qobjectdefs.h (тот
            // самый из "public slots:"), он раскрывается в пустоту и ломает
            // объявление переменной.
            int16_t* const hostSlots = h_slots.get() + slotsBuf * slotsPerBuffer;

            // Генерируем стартовые позиции на CPU. Это и есть та работа, ради
            // совмещения которой с расчётом заведён второй буфер.
            for (quint64 tid = 0; tid < numStartMasks; ++tid) {
                quint64 globalRank = slice.offset + tid * masksPerThread;
                assert(globalRank < perSet);
                // Генерируем стартовую комбинацию для ранга globalRank
                generateStartPositions(globalRank, numOfRows, r, hostSlots + tid * Constants::MAX_POSITIONS, binomTable);
            }

            // Копируем только те стартовые маски, которые нужны в этом чанке
            CUDA_CALL(cudaMemcpyAsync(
                d_slots.get(),
                hostSlots,
                numStartMasks * slotBytes,
                cudaMemcpyHostToDevice,
                localStream.get()));
            CUDA_CALL(cudaEventRecord(slotsCopied[slotsBuf].get(), localStream.get()));
            slotsBusy[slotsBuf] = true;
            slotsBuf ^= 1;

            // Запускаем ядро: numStartMasks потоков (упаковано в grid)
            int grid = (numStartMasks + threadsPerBlock - 1) / threadsPerBlock;
            if (grid <= 0) grid = 1;

            // Параметры: chunkSize (сколько масок в этом чанке всего),
            // masksPerThread (сколько масок на поток), numStartMasks (количество активных потоков)
            #ifdef _DEBUG
            d_maskCounter.fillZero();
            
            launchSpectrumKernelLong(
                grid,
                threadsPerBlock,
                localStream.get(),
                d_spectrum.get(),
                d_matrix.get(),
                static_cast<int>(numOfCols),
                static_cast<int>(numOfRows),
                static_cast<int>(wordsPerRow),
                chunkSize,
                d_slots.get(),
                masksPerThread,
                numStartMasks,
                r,
                d_maskCounter.get(),
                slice.slot);
            #else
            launchSpectrumKernelLong(
                grid,
                threadsPerBlock,
                localStream.get(),
                d_spectrum.get(),
                d_matrix.get(),
                static_cast<int>(numOfCols),
                static_cast<int>(numOfRows),
                static_cast<int>(wordsPerRow),
                chunkSize,
                d_slots.get(),
                masksPerThread,
                numStartMasks,
                r,
                nullptr,
                slice.slot);
            #endif
            // Здесь стоял cudaDeviceSynchronize(), без которого спектр считался
            // неверно. Защищал он ровно одну гонку: хост переписывал h_slots,
            // пока с него ещё шло асинхронное копирование, и часть стартовых
            // масок терялась. Теперь буферов два, и синхронизация не нужна.
            //
            // Остальное упорядочено самим потоком: копия d_slots следующей
            // итерации встаёт в очередь после ядра текущей, а спектр
            // накапливается атомарно в d_spectrum, куда пишут только ядра
            // этого же потока.

            #ifdef _DEBUG
            CUDA_CALL(cudaMemcpy(h_maskCounter.get(), d_maskCounter.get(), sizeof(uint64_t), cudaMemcpyDeviceToHost));
            const uint64_t m = h_maskCounter[0];
            if (m != chunkSize) {
                throw std::runtime_error("расхождение числа обработанных масок");
            }
            #endif
            // Учёт прогресса
            progress.addOps(chunkSize);
            runState.doneOps = progress.doneOps();

            const ProgressTracker::Due due = progress.due();

            // Снимок спектра для интерфейса
            if (due.spectrum && spectrumRing.enqueue(d_spectrum.get(), localStream.get()))
                progress.markSpectrum();
            if (const quint64* snapshot = spectrumRing.takeReady())
                updateSpectrumFrom(snapshot, int(numOfCols));

            if (due.bar)
                reportProgressBar();
            if (due.estimate)
                reportEstimate();

            if (due.checkpoint) {
                if (!saveGpuCheckpoint(localStream.get(), numOfCols, r, chunkOffset + chunkSize))
                    return;
            }

            if (cancelled.load()) break;
        } // chunkOffset
        if (cancelled.load()) break;
    } // for r

    // Финальная копия спектра
    CUDA_CALL(cudaMemcpyAsync(
        h_spectrum.get(),
        d_spectrum.get(),
        (numOfCols + 1) * sizeof(quint64),
        cudaMemcpyDeviceToHost,
        localStream.get()));
    CUDA_CALL(cudaStreamSynchronize(localStream.get()));

    updateSpectrum(numOfCols);
    // Освобождать вручную нечего: h_slots, d_slots, счётчик масок, поток и
    // событие владеющие — их снимут деструкторы, в том числе при исключении.
}




bool nextPositions(int16_t* a, int numOnes, int numRows) {
    // a[0] < a[1] < ... < a[numOnes-1]
    for (int i = numOnes - 1; i >= 0; --i) {
        if (a[i] < (int16_t)(numRows - numOnes + i)) {
            a[i] += 1;
            for (int j = i + 1; j < numOnes; ++j)
                a[j] = a[j - 1] + 1;
            return true;
        }
    }
    return false;
}

// diffPositions: находит симметрическую разницу между old_a и a,
// результат записывается в changed (уникальные номера строк).
// Возвращает число элементов в changed через numChanged (по ссылке).
void diffPositions(const int16_t* old_a, const int16_t* a, int numOnes, int16_t* changed, int& numChanged) {
    // Поскольку массивы отсортированы, можно пройти двумя указателями и собрать элементы,
    // которые присутствуют в одном массиве, но не в другом (симметрическая разность).
    int i = 0, j = 0;
    numChanged = 0;
    while (i < numOnes && j < numOnes) {
        if (old_a[i] == a[j]) {
            ++i; ++j;
        }
        else if (old_a[i] < a[j]) {
            changed[numChanged++] = old_a[i++];
        }
        else { // old_a[i] > a[j]
            changed[numChanged++] = a[j++];
        }
    }
    while (i < numOnes) changed[numChanged++] = old_a[i++];
    while (j < numOnes) changed[numChanged++] = a[j++];
}

void Worker::computeSpectrumCpuNoGrayLong(const CodeGeometry& g)
{
    const quint64 numOfRows   = g.numOfRows;
    const quint64 numOfCols   = g.numOfCols;
    const quint64 wordsPerRow = g.wordsPerRow;
    const quint64 maxComb     = g.maxRows;

    using namespace std::chrono;
    int numThreads = omp_get_max_threads();
    quint64 totalOps = totalLayerOps(g);

    // Число "битовых масок", обрабатываемых одним потоком
    const uint64_t masksPerThread = 1ULL << 20; // можно настроить
    const quint64 chunkSizeTarget = numThreads * masksPerThread;

    std::vector<std::vector<quint64>> threadSpectrum( numThreads, std::vector<quint64>(numOfCols + 1, 0ULL) );

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    // Основной внешний цикл по числу единиц в маске
    for (quint64 r = runState.rOffset; r <= maxComb; ++r)
    {
        // C(k, r) масок на каждое из множеств, подряд
        const quint64 perSet = binomTable(numOfRows, r);
        const quint64 curOps = perSet * g.setCount;

        quint64 startOffset = (r == runState.rOffset) ? runState.chunkOffset : 0;
        if (curOps == 0) continue;

        // Внутренний цикл по чанкам. Чанк не пересекает границу множества,
        // поэтому шаг цикла — его фактический размер.
        for (quint64 chunkOffset = startOffset, next = 0; chunkOffset < curOps; chunkOffset = next) {
            if (cancelled.load()) break;

            const LayerSlice slice = sliceLayer(g, perSet, chunkOffset, chunkSizeTarget);
            const quint64 chunkSize = slice.size;
            next = chunkOffset + chunkSize;
            if (chunkSize == 0) continue;

            
            uint64_t numStartMasks = (chunkSize + masksPerThread - 1ULL) / masksPerThread;
            if (numStartMasks == 0) continue;
            

            for (int t = 0; t < numThreads; ++t)
                std::fill(threadSpectrum[t].begin(), threadSpectrum[t].end(), 0ULL);

            #pragma omp parallel
            {
                // Локальный спектр для данного потока
                int tid = omp_get_thread_num();
                auto& localSpectrum = threadSpectrum[tid];

                // Локальные буферы, хранящие позиции массивов единиц
                int16_t a_local[Constants::MAX_POSITIONS];
                int16_t old_a_local[Constants::MAX_POSITIONS];
                int16_t changed[2 * Constants::MAX_POSITIONS];

                // локальное кодовое слово (wordsPerRow слов)
                std::vector<quint64> codeword((size_t)wordsPerRow, 0ULL);

                // Счётчик знаковый: OpenMP до версии 3.0 не допускает
                // беззнаковых индексов в parallel for. numStartMasks заведомо
                // помещается в int64_t, поэтому приводим его, а не счётчик.
                #pragma omp for schedule(dynamic)
                for (int64_t gtid = 0; gtid < int64_t(numStartMasks); ++gtid) {
                    if (cancelled.load()) continue;

                    // стартовый ранг и сколько итераций реально нужно
                    uint64_t startRank = gtid * masksPerThread;
                    if (startRank >= chunkSize) continue;
                    uint64_t iters = masksPerThread;
                    if (startRank + iters > chunkSize) iters = chunkSize - startRank;
                    if (iters == 0) continue;

                    uint64_t globalRank = slice.offset + startRank; // ранг внутри своего множества

                    // 1) Сгенерировать стартовые позиции
                    generateStartPositions(globalRank, (int)numOfRows, (int)r, a_local, binomTable);

                    // 2) Построить начальное codeword XOR-ом строк
                    // обнуляем codeword
                    for (size_t w = 0; w < (size_t)wordsPerRow; ++w) codeword[w] = 0ULL;

                    for (int i = 0; i < (int)r; ++i) {
                        int row = a_local[i];
                        quint64* rowData = h_matrix.get() + (quint64)(slice.slot.rowBase + row) * wordsPerRow;
                        for (size_t w = 0; w < (size_t)wordsPerRow; ++w)
                            codeword[w] ^= rowData[w];
                    }

                    // посчитать вес и добавить в локальный спектр
                    quint64 weight = 0;
                    for (size_t w = 0; w < (size_t)wordsPerRow; ++w) weight += __popcnt64(codeword[w]);
                    if (weight <= numOfCols
                        && (g.setCount <= 1 || bzKeepHost(codeword.data(), g, int(r), slice.slot.setIndex)))
                        localSpectrum[(size_t)weight]++;

                    // 3) Основной цикл по iters-1 следующим комбинациям
                    for (uint64_t it = 1; it < iters; ++it) {
                        if (cancelled.load()) break;
                        while (paused.load() != 0) {
                            // при паузе спим в основном потоке (та же логика, что и в другом коде)
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            if (cancelled.load()) break;
                        }
                        if (cancelled.load()) break;

                        // запомним старую позицию
                        for (int i = 0; i < (int)r; ++i) old_a_local[i] = a_local[i];

                        // получить следующую комбинацию позиций (возведение в следующий лекс. порядок)
                        if (!nextPositions(a_local, (int)r, (int)numOfRows)) {
                            // больше комбинаций нет (на границе) — выходим
                            break;
                        }

                        // найдем разницу old_a_local <-> a_local
                        int numChanged = 0;
                        diffPositions(old_a_local, a_local, (int)r, changed, numChanged);

                        // если изменений много — перестроим codeword полностью
                        if (numChanged > (int)r) {
                            // rebuild
                            for (size_t w = 0; w < (size_t)wordsPerRow; ++w) codeword[w] = 0ULL;
                            for (int i = 0; i < (int)r; ++i) {
                                int row = a_local[i];
                                quint64* rowData = h_matrix.get() + (quint64)(slice.slot.rowBase + row) * wordsPerRow;
                                for (size_t w = 0; w < (size_t)wordsPerRow; ++w)
                                    codeword[w] ^= rowData[w];
                            }
                        }
                        else {
                            // применяем XOR для каждой изменённой строки
                            for (int t = 0; t < numChanged; ++t) {
                                int row = changed[t];
                                quint64* rowData = h_matrix.get() + (quint64)(slice.slot.rowBase + row) * wordsPerRow;
                                for (size_t w = 0; w < (size_t)wordsPerRow; ++w)
                                    codeword[w] ^= rowData[w];
                            }
                        }

                        // считаем вес
                        quint64 weight2 = 0;
                        for (size_t w = 0; w < (size_t)wordsPerRow; ++w) weight2 += __popcnt64(codeword[w]);
                        if (weight2 <= numOfCols
                            && (g.setCount <= 1 || bzKeepHost(codeword.data(), g, int(r), slice.slot.setIndex)))
                            localSpectrum[(size_t)weight2]++;
                    } // for it
                } // for gtid

                // Слияние локального спектра в глобальный
                //#pragma omp critical
                //{
                //    for (quint64 w = 0; w <= numOfCols; ++w)
                //        h_spectrum[(size_t)w] += localSpectrum[(size_t)w];
                //}
            } // omp parallel
            for (int t = 0; t < numThreads; ++t)
            {
                for (quint64 w = 0; w <= numOfCols; ++w)
                {
                    h_spectrum[w] += threadSpectrum[t][w];
                }
            }
            // После расчёта чанка — учитываем прогресс и обновляем интерфейс.
            // Спектр в h_spectrum.get() уже актуален, копировать ниоткуда не надо.
            progress.addOps(chunkSize);
            runState.doneOps = progress.doneOps();

            const ProgressTracker::Due due = progress.due();

            if (due.spectrum) {
                progress.markSpectrum();
                updateSpectrum((int)numOfCols);
            }
            if (due.bar)
                reportProgressBar();
            if (due.estimate)
                reportEstimate();
            if (due.checkpoint)
                saveCpuCheckpoint((int)numOfCols, r, chunkOffset + chunkSize);

            if (cancelled.load()) break;
        } // chunkOffset
        if (cancelled.load()) break;
    } // for r
    // финал: update UI и выход
    updateSpectrum((int)numOfCols);
}

void Worker::computeSpectrumCpuGrayShort(const CodeGeometry& g)
{
    const quint64 numOfRows   = g.numOfRows;
    const quint64 numOfCols   = g.numOfCols;
    const quint64 wordsPerRow = g.wordsPerRow;
    const quint64 chunkSize   = g.chunkSize;

    using namespace std::chrono;

    // Число масок для обработки
    quint64 totalOps = 1ULL << numOfRows;
    // Функция для получения кода Грея из обычного
    auto gray = [](quint64 i)->quint64
        {
            return (i ^ (i >> 1));
        };

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    for (quint64 offset = runState.chunkOffset; offset < totalOps; offset += chunkSize)
    {
        // Если пользователь нажал кнопку отмены, завершаем расчет
        if (cancelled.load())
            break;
        // Получаем индекс первой маски в чанке
        quint64 chunkStart = offset;
        // Получаем индекс последней маски в чанке
        quint64 chunkEnd = std::min(offset + chunkSize, totalOps);

        // =========================
        // ПАРАЛЛЕЛЬНЫЙ РАСЧЁТ ЧАНКА
        // =========================
        #pragma omp parallel
        {
            /*------- ПОДГОТОВКА И РАСЧЕТ ВЕСА ПЕРВОЙ МАСКИ ДЛЯ КАЖДОГО ПОТОКА --------*/

            // Получаем индекс текущего потока
            int tid = omp_get_thread_num();
            // Получаем максимальное число потоков
            int nthreads = omp_get_num_threads();

            // Размер чанка
            quint64 chunkLen = chunkEnd - chunkStart;
            // Число масок, обрабатываемых одним потоком
            quint64 perThread =
                (chunkLen + nthreads - 1) / nthreads;
            // Индекс первой маски, рассчитываемой данным потоком
            quint64 startIdx = chunkStart + tid * perThread;
            // Индекс последней маски, -||-
            quint64 endIdx = std::min(startIdx + perThread,
                chunkEnd);

            // Если у потока есть маски для обработки
            if (startIdx < endIdx) {
                // Локальное кодовое слово для данного потока
                std::vector<quint64> localCodeword(wordsPerRow, 0);
                // Локальный спектр для данного потока
                std::vector<quint64> localSpectrum(numOfCols + 1, 0);

                // Первую маску для данного потока
                quint64 g = gray(startIdx);

                quint64 tmp = g;
                // Пока в маске есть единицы
                while (tmp)
                {
                    // Выделяем младший установленный бит
                    quint64 single = tmp & (~tmp + 1ULL);
                    // Находим его позицию
                    unsigned long pos;
                    _BitScanForward64(&pos, single);
                    // Ставим младший бит в 0
                    tmp &= (tmp - 1);
                    // Получаем строку матрицы
                    quint64* rowData =
                        h_matrix.get() + pos * wordsPerRow;
                    // XOR-им с поулченной строкой
                    for (quint64 b = 0; b < wordsPerRow; ++b)
                        localCodeword[b] ^= rowData[b];
                }

                // Считаем вес полученного кодового слова
                quint64 weight = 0;
                for (quint64 b = 0; b < wordsPerRow; ++b)
                    weight += __popcnt64(localCodeword[b]);
                if (weight <= numOfCols)
                    localSpectrum[weight]++;

                /*-------------------------------------------------------------------------*/


                /*------------ ОСНОВНОЙ ЦИКЛ ПО КУСКУ ЧАНКА ДЛЯ КАЖДОГО ПОТОКА ------------*/

                for (quint64 i = startIdx + 1; i < endIdx; ++i) {
                    // -||-
                    if (cancelled.load())
                        break;
                    // Если пользователь поставил паузу
                    while (paused.load() != 0)
                    {
                        QThread::msleep(50);
                        if (cancelled.load())
                            break;
                    }

                    // Получаем следующую битовую маску
                    quint64 g_next = gray(i);
                    // Находим разницу между масками
                    quint64 diff = g ^ g_next;
                    // Так маски отличаются только в одной позиции (Код Грея), то находим её
                    unsigned long pos;
                    // Находим её индекс
                    _BitScanForward64(&pos, diff);

                    // Получаем строку матрицы 
                    quint64* rowData =
                        h_matrix.get() + pos * wordsPerRow;
                    // XOR-им
                    for (quint64 b = 0; b < wordsPerRow; ++b)
                        localCodeword[b] ^= rowData[b];

                    g = g_next;

                    // Считаем вес
                    weight = 0;
                    for (quint64 b = 0; b < wordsPerRow; ++b)
                        weight += __popcnt64(localCodeword[b]);
                    // Записываем в спектр
                    if (weight <= numOfCols)
                        localSpectrum[weight]++;
                } // for (quint64 i = startIdx + 1; i < endIdx; ++i)

                // После расчета чанка производим объединение локальных спектров
                #pragma omp critical
                {
                    for (quint64 w = 0; w <= numOfCols; ++w)
                        h_spectrum[w] += localSpectrum[w];
                } //#pragma omp critical

            } // if (startIdx < endIdx)

        } // ===== Конец omp parallel =====
        

        // chunkEnd — абсолютный индекс маски, поэтому присваивание, а не +=
        progress.setDoneOps(chunkEnd);
        runState.doneOps = chunkEnd;

        const ProgressTracker::Due due = progress.due();

        if (due.spectrum) {
            progress.markSpectrum();
            updateSpectrum(numOfCols);
        }
        if (due.bar)
            reportProgressBar();
        if (due.estimate)
            reportEstimate();
        if (due.checkpoint) {
            // Код Грея нумерует маски сплошь, слоёв по числу единиц нет.
            saveCpuCheckpoint(numOfCols, 0, runState.doneOps);
        }
        if (cancelled.load())
            break;

    } // for (quint64 offset = startChunkInd; offset < totalOps; offset += chunkSize)
}

void Worker::computeSpectrumCpuNoGrayShort(const CodeGeometry& g)
{
    const quint64 numOfRows   = g.numOfRows;
    const quint64 numOfCols   = g.numOfCols;
    const quint64 wordsPerRow = g.wordsPerRow;
    const quint64 chunkSize   = g.chunkSize;
    const quint64 maxComb     = g.maxRows;

    using namespace std::chrono;

    quint64 totalOps = totalLayerOps(g);

    // Если продолжаем после чекпоинта
    quint64 startR = runState.rOffset;
    quint64 startOffsetForStartR = runState.chunkOffset;

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    for (quint64 r = startR; r <= maxComb; ++r)
    {
        if (cancelled.load())
            break;

        // C(k, r) комбинаций на каждое из множеств, подряд
        const quint64 perSet    = binomTable(numOfRows, r);
        const quint64 combCount = perSet * g.setCount;
        if (combCount == 0)
            continue;

        quint64 startOffset = (r == startR) ? startOffsetForStartR : 0;

        // Чанк не пересекает границу множества, поэтому шаг цикла — его
        // фактический размер.
        for (quint64 offset = startOffset, next = 0; offset < combCount; offset = next)
        {
            const LayerSlice slice = sliceLayer(g, perSet, offset, chunkSize);
            const quint64 thisChunkSize = slice.size;
            next = offset + thisChunkSize;

            if (cancelled.load())
                break;

            while (paused.load() != 0) {
                QThread::msleep(50);
                if (cancelled.load())
                    break;
            }

            // Локальный результат чанка
            QVector<quint64> chunkSpectrum(numOfCols + 1, 0);

            // Каждый поток берёт непрерывный кусок диапазона рангов и идёт по
            // нему приёмом Госпера — так же, как ядро коротких кодов.
            //
            // Раньше на каждую комбинацию звался unrankCombination (разбор
            // ранга по таблице биномов, O(k)) и кодовое слово собиралось с
            // нуля перебором всех numOfRows строк. Теперь ранг разбирается
            // один раз на поток, а дальше маска шагает за несколько операций,
            // и XOR-ятся только изменившиеся строки.
            //
            // Нумерация комбинаций лексикографическая — её задаёт смысл
            // chunkOffset, и менять её нельзя. Госпер идёт по числовому
            // порядку, но развёрнутая по битам маска (позиция p -> k-1-p)
            // превращает одно в другое. Поэтому берётся последний ранг куска,
            // и обход идёт в обратную сторону: для гистограммы порядок
            // безразличен.
            #pragma omp parallel
            {
                const int tid      = omp_get_thread_num();
                const int nthreads = omp_get_num_threads();

                const quint64 perThread = (thisChunkSize + nthreads - 1) / nthreads;
                const quint64 startIdx  = slice.offset + quint64(tid) * perThread;
                const quint64 endIdx    = std::min(startIdx + perThread, slice.offset + thisChunkSize);

                // Отмена проверяется только между чанками. Прерывать перебор
                // внутри нельзя: частичный чанк всё равно прибавится к спектру,
                // и если на него выпадет чекпоинт, сохранится испорченное
                // состояние. Чанк короткий, задержка отмены незаметна.
                if (startIdx < endIdx) {
                    QVector<quint64> localSpectrum(numOfCols + 1, 0);
                    QVector<quint64> localCodeword(wordsPerRow, 0);

                    quint64 revMask = reverseLowBits(
                        unrankCombination(unsigned(numOfRows), unsigned(r),
                                          endIdx - 1, binomTable), numOfRows);

                    // Бит p развёрнутой маски отвечает строке numOfRows-1-p.
                    auto xorRows = [&](quint64 bits) {
                        while (bits) {
                            unsigned long p;
                            _BitScanForward64(&p, bits);
                            bits &= (bits - 1);
                            const quint64* rowData =
                                h_matrix.get() + (slice.slot.rowBase + numOfRows - 1 - p) * wordsPerRow;
                            for (quint64 b = 0; b < wordsPerRow; ++b)
                                localCodeword[b] ^= rowData[b];
                        }
                    };
                    auto accumulate = [&]() {
                        quint64 weight = 0;
                        for (quint64 b = 0; b < wordsPerRow; ++b)
                            weight += __popcnt64(localCodeword[b]);
                        if (weight <= numOfCols
                            && (g.setCount <= 1
                                || bzKeepHost(localCodeword.data(), g, int(r), slice.slot.setIndex)))
                            localSpectrum[weight]++;
                    };

                    xorRows(revMask);
                    accumulate();

                    // При r = 0 и r = numOfRows комбинация одна, и цикл не
                    // выполняется — gosperNext на нулевой маске звать нельзя.
                    const quint64 count = endIdx - startIdx;
                    for (quint64 i = 1; i < count; ++i) {
                        const quint64 nextRev = gosperNext(revMask);
                        xorRows(revMask ^ nextRev);
                        accumulate();
                        revMask = nextRev;
                    }

                    #pragma omp critical
                    {
                        for (quint64 w = 0; w <= numOfCols; ++w)
                            chunkSpectrum[w] += localSpectrum[w];
                    }
                }
            }

            // Прибавляем чанк к общему спектру
            for (quint64 w = 0; w <= numOfCols; ++w)
                h_spectrum[w] += chunkSpectrum[w];

            progress.addOps(thisChunkSize);
            runState.doneOps = progress.doneOps();

            const ProgressTracker::Due due = progress.due();

            if (due.spectrum) {
                progress.markSpectrum();
                updateSpectrum(numOfCols);
            }
            if (due.bar)
                reportProgressBar();
            if (due.estimate)
                reportEstimate();
            // chunkOffset указывает на первый ещё не обработанный ранг
            if (due.checkpoint)
                saveCpuCheckpoint(numOfCols, r, offset + thisChunkSize);
        }
    }
    updateSpectrum(numOfCols);
}
// Случайный поиск по информационным множествам — см. leonsearch.h.
//
// Попытки независимы и нумерованы; номер задаёт порядок столбцов, поэтому
// попытка с одним номером даёт одно и то же множество на CPU и на GPU, и оба
// пути обязаны находить одни и те же слова. Найденное складывается в таблицу
// на хосте: там считаются поимки, по ним — оценка ненайденного.
//
// CPU: пачка попыток раздаётся потокам OpenMP, слова кладутся в таблицу под
// замком. Замок не мешает: слов нужного веса — доли процента от перебранных.
//
// GPU: блок нитей — попытка (leonkernel.cu), слова выкладываются в буфер, а
// хост забирает их и вставляет в таблицу. Буферов два: пока хост разбирает
// одну пачку, видеокарта считает следующую.
//
// Чекпоинтов по ходу нет: пришлось бы писать на диск всю таблицу, а поиск по
// самой своей природе короткий — его предел ставит память под слова.
void Worker::computeSpectrumLeon(const CodeGeometry& g)
{
    const int rows  = int(g.numOfRows);
    const int cols  = int(g.numOfCols);
    const int words = int(g.wordsPerRow);
    const int depth = int(g.maxRows);
    const int maxWeight = settings.leonWeight;

    // Больше гигабайта под слова не берём: дальше уже не поиск, а полный
    // перебор, и для него есть другие алгоритмы.
    constexpr quint64 kTableLimitBytes = 1ULL << 30;

    // Ранг проверяется один раз здесь: ядро молча даёт пустую попытку, а
    // CPU-путь узнал бы об этом только внутри параллельной области.
    {
        std::vector<int> order;
        Leon::shuffledColumns(cols, 0, order);
        InfoSets::InfoSet set;
        if (!InfoSets::systematize(h_matrix.get(), rows, cols, words, order, nullptr, set))
            throw std::invalid_argument(
                "строки матрицы зависимы: случайному поиску нужна матрица полного ранга");
    }

    // Частей — по числу потоков: каждая наполняется своим.
    Leon::ShardedWordTable table(words, maxWeight, std::max(1, omp_get_max_threads()));

    // Прогресс считается в словах, как везде: скорость тогда сравнима.
    // Число попыток растёт по ходу: чем больше слов какого-то веса нашлось,
    // тем больше попыток нужно, чтобы ни одно из них не оказалось пропущено
    // с заданной вероятностью. План даёт нижнюю границу — на одно слово.
    quint64 target = g.leonTrials;
    auto opsFor = [&](quint64 trials) {
        const double ops = double(trials) * g.leonWordsPerTrial;
        return ops >= 1.8e19 ? std::numeric_limits<quint64>::max() : quint64(ops);
    };
    progress.begin(opsFor(target), 0, 0);

    quint64 launched  = 0;   // попыток начато
    quint64 collected = 0;   // попыток, чьи слова уже в таблице

    auto retarget = [&]() {
        const quint64 needed = Leon::trialsForAll(cols, rows, maxWeight, depth,
                                                  settings.leonMissProbability(),
                                                  table.countByWeight());
        if (needed > target) {
            target = needed;
            progress.setTotalOps(opsFor(target));
        }
    };

    auto publish = [&](bool force) {
        const std::vector<quint64> found = table.countByWeight();
        h_spectrum.fillZero();
        h_spectrum[0] = 1;
        for (size_t w = 1; w < found.size() && w < g.spectrumSize; ++w)
            h_spectrum[w] = found[w];

        const ProgressTracker::Due due = progress.due();
        if (due.estimate)
            reportEstimate();
        if (due.bar)
            reportProgressBar();
        if (due.spectrum || force) {
            progress.markSpectrum();
            updateSpectrum(cols);

            // Вероятность пропустить хотя бы одно слово: по модели, для
            // каждого веса — найденные слова умножить на шанс пропуска
            // одного слова, поделённый на шанс поимки. Сумма по весам.
            double missTotal = 0.0;
            SpectrumFloat unseen(cols + 1, 0.0f);
            const std::vector<double> chao = table.unseenByWeight();
            for (int w = 1; w <= maxWeight && w <= cols; ++w) {
                const double p = Leon::catchProbability(cols, rows, w, depth);
                const double q = std::exp(double(collected) * std::log1p(-p));   // (1-p)^collected
                if (found[size_t(w)] > 0)
                    missTotal += double(found[size_t(w)]) * q / std::max(1.0 - q, 1e-300);
                unseen[w] = float(chao[size_t(w)]);
            }
            emit searchEstimate(maxWeight, collected, target,
                                std::min(1.0, missTotal), unseen);
        }
    };

    auto checkMemory = [&]() {
        if (table.bytes() > kTableLimitBytes) {
            publish(true);
            throw std::runtime_error(
                "слишком много слов до заданного веса: таблица не помещается в память, "
                "уменьшите вес");
        }
    };

    // ------------------------------------------------------------- CPU
    if (!g.useGpu) {
        // Пачка попыток на один проход: достаточно мелкая, чтобы отмена и
        // пауза отзывались быстро, и достаточно крупная, чтобы потоки не
        // простаивали.
        const quint64 batch = quint64(std::max(1, omp_get_max_threads())) * 16;

        while (launched < target) {
            if (!waitWhilePaused())
                break;
            const quint64 count = std::min(batch, target - launched);

            #pragma omp parallel for schedule(dynamic)
            for (long long t = 0; t < (long long)count; ++t) {
                Leon::trial(h_matrix.get(), rows, cols, words, depth, maxWeight,
                            launched + quint64(t),
                            [&](const quint64* word, int weight) {
                                table.add(word, weight);
                            });
            }

            launched  += count;
            collected  = launched;
            activeTrials = collected;
            progress.addOps(quint64(double(count) * g.leonWordsPerTrial));
            runState.doneOps = progress.doneOps();

            retarget();
            checkMemory();
            publish(false);
            if (cancelled.load())
                break;
        }
        publish(true);
        return;
    }

    // ------------------------------------------------------------- GPU
    // Блок на попытку, 256 нитей: Гаусс идёт всем блоком по строкам, а
    // перебор — по комбинациям; сетка из настроек тут ни при чём.
    constexpr int     kThreads     = 256;
    constexpr quint64 kBatchMax    = 8192;         // попыток на запуск, потолок
    constexpr quint64 kCapacityMax = 8ULL << 20;   // слов в буфере, потолок
    quint64           capacity     = 1ULL << 20;

    // Матрица, не влезшая в разделяемую память, у каждого блока своя — в
    // рабочем буфере. Буфер на слот не больше 128 МБ, им и ограничена пачка.
    const bool   global       = !leonFitsShared(rows, cols, words);
    const size_t scratchWords = global ? size_t(rows) * size_t(leonPaddedWords(words)) : 0;
    quint64      blocksMax    = kBatchMax;
    if (global)
        blocksMax = std::max<quint64>(1, std::min<quint64>(kBatchMax,
                        (128ULL << 20) / (scratchWords * sizeof(quint64))));

    DeviceBuffer<quint64> d_mat;
    d_mat.allocate(size_t(rows) * words);
    CUDA_CALL(cudaMemcpy(d_mat.get(), h_matrix.get(), size_t(rows) * words * sizeof(quint64),
                         cudaMemcpyHostToDevice));

    struct Slot
    {
        DeviceBuffer<quint64>  d_out;
        DeviceBuffer<quint64>  d_scratch;
        DeviceBuffer<unsigned> d_count;
        HostBuffer<quint64>    h_out;
        HostBuffer<unsigned>   h_count;
        CudaStream             stream;
        quint64                first   = 0;
        quint64                count   = 0;
        bool                   pending = false;
    };
    Slot slot[2];
    auto allocateOut = [&](Slot& s) {
        s.d_out.allocate(size_t(capacity) * words);
        s.h_out.allocate(size_t(capacity) * words, HostBuffer<quint64>::Kind::Pinned);
    };
    for (Slot& s : slot) {
        allocateOut(s);
        if (global)
            s.d_scratch.allocate(size_t(blocksMax) * scratchWords);
        s.d_count.allocate(1);
        s.h_count.allocate(1, HostBuffer<unsigned>::Kind::Pinned);
        s.stream.create();
    }

    // Размер пачки подстраивается под плотность находок: у плотного кода
    // слов нужного веса тысячи на попытку, у редкого — доли. Первая пачка
    // маленькая — по ней и меряется.
    quint64 batchTrials  = std::min<quint64>(256, blocksMax);
    double  hitsPerTrial = 0.0;
    auto adaptBatch = [&](quint64 found, quint64 count) {
        if (count == 0)
            return;
        hitsPerTrial = std::max(hitsPerTrial, double(found) / double(count));
        const double room = double(capacity) / 4.0 / std::max(1.0, hitsPerTrial);
        batchTrials = quint64(std::min(double(blocksMax), std::max(1.0, room)));
    };

    // Пачки, которые пришлось отложить: переполнившаяся делится пополам, и
    // вторая половина ждёт своей очереди здесь.
    std::vector<std::pair<quint64, quint64>> deferred;

    auto launchBatch = [&](Slot& s, quint64 first, quint64 count) {
        CUDA_CALL(cudaMemsetAsync(s.d_count.get(), 0, sizeof(unsigned), s.stream.get()));
        LeonLaunch L;
        L.matrix       = d_mat.get();
        L.rows         = rows;
        L.cols         = cols;
        L.wordsPerRow  = words;
        L.rowsPerTrial = depth;
        L.maxWeight    = maxWeight;
        L.firstTrial   = first;
        L.trials       = int(count);
        L.outWords     = s.d_out.get();
        L.outCount     = s.d_count.get();
        L.capacity     = unsigned(capacity);
        L.scratch      = s.d_scratch.get();
        launchLeonTrials(L, kThreads, s.stream.get());
        CUDA_CALL(cudaMemcpyAsync(s.h_count.get(), s.d_count.get(), sizeof(unsigned),
                                  cudaMemcpyDeviceToHost, s.stream.get()));
        s.first = first; s.count = count; s.pending = true;
    };

    // Забирает слова пачки в таблицу. false — буфер оказался мал: слова
    // сверх него потеряны, пачку надо повторить.
    auto collectBatch = [&](Slot& s) -> bool {
        CUDA_CALL(cudaStreamSynchronize(s.stream.get()));
        const unsigned found = s.h_count[0];
        if (quint64(found) > capacity)
            return false;
        if (found > 0) {
            CUDA_CALL(cudaMemcpy(s.h_out.get(), s.d_out.get(),
                                 size_t(found) * words * sizeof(quint64), cudaMemcpyDeviceToHost));
            table.addBatch(s.h_out.get(), found);
        }
        adaptBatch(found, s.count);
        s.pending  = false;
        collected += s.count;
        return true;
    };

    // Переполнение. Пока буфер можно увеличить — увеличивается (оба сразу,
    // они одного размера); упёрлись в потолок — пачка делится пополам, и
    // вторая половина откладывается. Чужую пачку сначала забрать: иначе её
    // слова пропадут вместе со старым буфером; не влезла и она — повторится
    // тем же порядком.
    auto shrinkOrGrow = [&](Slot& s, quint64 need) {
        // Счётчик ядра считает все находки, и за пределами буфера тоже, —
        // плотность по нему честная.
        adaptBatch(need, s.count);
        if (need <= kCapacityMax && capacity < kCapacityMax) {
            capacity = std::min(kCapacityMax, std::max(capacity * 2, need + need / 4 + 1024));
            return;
        }
        if (s.count <= 1)
            throw std::runtime_error(
                "одна попытка даёт больше восьми миллионов слов до заданного веса: уменьшите вес");
        const quint64 half = s.count / 2;
        deferred.push_back({ s.first + half, s.count - half });
        s.count = half;
    };
    auto collectOrRetry = [&](Slot& s) {
        while (!collectBatch(s)) {
            const quint64 before = capacity;
            Slot& other = (&s == &slot[0]) ? slot[1] : slot[0];
            bool rerunOther = false;
            if (other.pending && !collectBatch(other)) {
                rerunOther = true;
                shrinkOrGrow(other, other.h_count[0]);
            }
            shrinkOrGrow(s, s.h_count[0]);
            if (capacity != before) {
                allocateOut(slot[0]);
                allocateOut(slot[1]);
            }
            if (rerunOther)
                launchBatch(other, other.first, other.count);
            launchBatch(s, s.first, s.count);
        }
    };

    // Следующий кусок работы: сначала отложенное, потом новые попытки.
    auto takeRange = [&](quint64& first, quint64& count) -> bool {
        if (!deferred.empty()) {
            const std::pair<quint64, quint64> range = deferred.back();
            deferred.pop_back();
            first = range.first;
            count = std::min(range.second, batchTrials);
            if (range.second > count)
                deferred.push_back({ first + count, range.second - count });
            return true;
        }
        if (launched >= target)
            return false;
        first = launched;
        count = std::min(batchTrials, target - launched);
        launched += count;
        progress.addOps(quint64(double(count) * g.leonWordsPerTrial));
        runState.doneOps = progress.doneOps();
        return true;
    };

    int cur = 0;
    for (;;) {
        quint64 first = 0, count = 0;
        if (!takeRange(first, count)) {
            // Всё запущено — дождаться хвоста и решить, не нужно ли ещё.
            for (Slot& s : slot)
                if (s.pending)
                    collectOrRetry(s);
            activeTrials = collected;
            retarget();
            checkMemory();
            if (deferred.empty() && launched >= target)
                break;
            continue;
        }
        if (!waitWhilePaused())
            break;

        Slot& s = slot[cur];
        if (s.pending)
            collectOrRetry(s);
        launchBatch(s, first, count);
        activeTrials = collected;

        retarget();
        checkMemory();
        publish(false);
        cur ^= 1;
        if (cancelled.load())
            break;
    }

    // Буферы освобождаются деструкторами; ядра к этому моменту должны
    // закончиться — иначе они писали бы в уже отданную память.
    for (Slot& s : slot)
        CUDA_CALL(cudaStreamSynchronize(s.stream.get()));
    activeTrials = collected;
    publish(true);
}

void Worker::updateSpectrum(int numOfCols)
{
    updateSpectrumFrom(h_spectrum.get(), numOfCols);
}

void Worker::updateSpectrumFrom(const quint64* spectrum, int numOfCols)
{
    if (!spectrum)
        return;
    // Проба меряет, как часто спектр успевает уйти, а не показывает его.
    // Считать надо здесь: через это место проходят все пути, включая CPU.
    if (probeMode) {
        ++probeSends;
        return;
    }
    bool spectrumEmpty = true;
    QStringList spectrumCopyPTE;
    SpectrumFloat spectrumCopyPlot;

    quint64 val = 0;
    for (quint64 w = 0; w <= numOfCols; ++w) {
        spectrumCopyPlot.append(float(spectrum[w]));
        if (spectrum[w] != 0) {
            val += spectrum[w];
            spectrumCopyPTE.append(QString::number(w) + " - " + QString::number(spectrum[w]));
            spectrumEmpty = false;
        }
    }
    if (!spectrumEmpty) {
        emit updateSpectrumPTE(spectrumCopyPTE);
        emit updateSpectrumPlot(spectrumCopyPlot);
    }
}
void Worker::updateSpectrumDual(int numOfCols, int numOfRows)
{
    if (probeMode) {
        ++probeSends;
        return;
    }
    if (!h_spectrum.get())
        return;

    bool spectrumEmpty = true;
    // Считаем текстовый спектр из дуального
    QStringList spectrumCopyPTE = computeSpectrumFromDual( h_spectrum.get(), numOfCols, numOfRows );
    SpectrumFloat spectrumCopyPlot;
    spectrumCopyPlot.reserve(numOfCols+1);

    for (int i = 0; i <= numOfCols; i++) { spectrumCopyPlot.push_back(0.f); }
    // Получаем значения типа float из текстового спектра
    for (const QString& line : spectrumCopyPTE) {
        QStringList parts = line.split(" - ");
        int index = parts[0].toInt();
        float value = parts[1].toFloat();
        if (value != 0.f)
            spectrumEmpty = false;
        spectrumCopyPlot[index] = value;
    }
    // Если спектр не пуст, то обновляем его
    if (!spectrumEmpty) {
        emit updateSpectrumPTE(spectrumCopyPTE);
        emit updateSpectrumPlot(spectrumCopyPlot);
    }
}
void Worker::makeCheckpoint(int numOfCols, bool finished)
{
    runState.spectrum.resize(numOfCols + 1);
    for (int i = 0; i < numOfCols + 1; i++)
        runState.spectrum[i] = h_spectrum[i];

    AutosaveRecord record;
    record.algorithm = settings.algorithmType;
    record.enumType  = settings.enumType;
    record.maxRows   = settings.maxRows;
    record.finished  = finished;
    // У Брауэра–Циммермана глубина перебора выведена из веса, а продолжать
    // расчёт можно только по тем же множествам — они уходят в запись.
    if (settings.algorithmType == ComputationSettings::BrouwerZimmermann) {
        record.maxRows  = activeMaxRows;
        record.bzWeight = settings.bzWeight;
        record.infoSets = activeInfoSets;
    }
    if (settings.algorithmType == ComputationSettings::RandomInfoSets) {
        record.maxRows          = activeMaxRows;
        record.leonWeight       = settings.leonWeight;
        record.leonMissExponent = settings.leonMissExponent;
        record.leonTrials       = activeTrials;
    }
    record.savedAt   = QDateTime::currentDateTime();
    record.state     = runState;

    // Ключ — матрица и алгоритм. Сама матрица в запись не попадает: она лежит
    // одним файлом на папку, иначе на коде (1000,997) каждое сохранение тащило
    // бы с собой мегабайт нулей и единиц.
    autosave.save(settings.matrix, record);
    emit showSaveLBL();
}

void Worker::setAutosaveRoot(const QString& dir)
{
    autosave = AutosaveStore(dir);
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
        emit finished(Constants::ERROR_OCCURED);
    }
    catch (const std::exception& e) {
        releaseResources();
        emit errorOccurred(QStringLiteral("Ошибка расчёта: %1")
                               .arg(QString::fromUtf8(e.what())));
        emit finished(Constants::ERROR_OCCURED);
    }
}

void Worker::measureUpdateRate()
{
    probeMode  = true;
    probeSends = 0;
    cancelled.store(0);

    const auto startedAt = steady_clock::now();
    double seconds = 0.0;

    try {
        CodeGeometry g = describeTask();
        initializeRunState(LoadMode::Reset);
        runState.spectrum.resize(int(g.spectrumSize));

        // Замер идёт на самом глубоком слое. Слои по числу складываемых строк
        // перебираются по возрастанию, первые из них крошечные, и чанки в них
        // обрезаны по границе слоя: замер по началу расчёта показал бы частоту,
        // которой на деле не будет уже через минуту. У кода Грея слоёв нет, там
        // rOffset ни на что не влияет.
        if (g.maxRows > 0)
            runState.rOffset = g.maxRows;

        omp_set_num_threads(settings.compDevSet.threadsCpu);
        prepareBuffers(g);
        // Подбор сетки нужен и здесь: он выбирает сетку покрупнее, а от неё
        // напрямую зависит длина чанка и, значит, потолок. Замер без подбора
        // показал бы не ту частоту, с которой пользователь потом будет считать.
        tuneGrid(g);

        // Просить спектр как можно чаще, сохранений не делать: проба не имеет
        // права трогать состояние расчёта.
        progress.setIntervals(std::chrono::milliseconds{ 0 },
                              std::chrono::hours{ 24 });
        progress.setOpsCheckpoint(0);

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

    const quint64 sends = probeSends;
    probeMode  = false;
    probeSends = 0;
    releaseResources();
    // Состояние расчёта после пробы — чужое: она стартовала с последнего слоя.
    initializeRunState(LoadMode::Reset);
    cancelled.store(0);

    emit updateRateMeasured(seconds > 0.0 ? double(sends) / seconds : 0.0);
}

// Выводит из настроек и матрицы всё, что нужно для расчёта.
CodeGeometry Worker::describeTask() const
{
    CodeGeometry g;

    g.matrix = settings.matrix;
    // Для дуального кода перебор идёт по проверочной матрице, а не по той,
    // что ввёл пользователь.
    if (settings.algorithmType == ComputationSettings::DualCode)
        g.matrix = generatorToParity(g.matrix);

    g.numOfRows    = quint64(g.matrix.length());
    g.numOfCols    = quint64(g.matrix[0].length());
    g.wordsPerRow  = (g.numOfCols + 63) / 64;
    g.matrixWords  = g.numOfRows * g.wordsPerRow;
    g.spectrumSize = g.numOfCols + 1;
    g.maxRows      = quint64(settings.maxRows);

    g.blocksGpu  = settings.compDevSet.blocksGpu;
    g.threadsGpu = settings.compDevSet.threadsGpu;
    g.useGpu     = settings.compDev == ComputationSettings::ComputeDevice::GPU;
    g.isLongCode = g.numOfRows > Constants::MAX_SHORT_CODE_LENGTH;

    if (settings.algorithmType == ComputationSettings::BrouwerZimmermann)
        planInfoSets(g);

    if (settings.algorithmType == ComputationSettings::RandomInfoSets) {
        // На видеокарте короткая матрица живёт в разделяемой памяти блока,
        // длинная — в глобальной; ядро умеет строки до MAX_BLOCKWORDS слов.
        if (g.useGpu && leonSharedBytes(int(g.numOfRows), int(g.numOfCols), int(g.wordsPerRow)) == 0)
            throw std::invalid_argument(
                "случайный поиск на видеокарте: строка длиннее, чем умеет ядро — выберите CPU");
        const Leon::Plan plan = Leon::plan(int(g.numOfCols), int(g.numOfRows),
                                           settings.leonWeight, settings.leonMissProbability(),
                                           g.useGpu);
        g.maxRows           = quint64(plan.rows);
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
    if (!resumedInfoSets.isEmpty()) {
        if (!InfoSets::rebuild(packed.data(), rows, cols, words, resumedInfoSets, sets))
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
        sets.resize(size_t(InfoSets::setsForWeight(overlaps, settings.bzWeight, rows, cols)));
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
    g.maxRows         = quint64(InfoSets::rowsForWeight(g.setOverlaps, settings.bzWeight, rows, cols));
    g.guaranteedBelow = InfoSets::guaranteedBelow(g.setOverlaps, int(g.maxRows), rows, cols);
}

// Упаковывает матрицу в биты и раскладывает буферы по памяти.
void Worker::prepareBuffers(const CodeGeometry& g)
{
    if (g.matrixInGlobalMem) {
        /* ДОПИСАТЬ КОПИРОВАНИЕ МАТРИЦЫ В ПАМЯТЬ ДЛЯ КОРОТКИХ КОДОВ */
        if (!g.isLongCode)
            throw std::invalid_argument("матрица слишком большая для короткого кода");
        d_matrix.allocate(g.matrixWords);
    }

    // calloc внутри, поэтому матрица уже обнулена
    h_matrix.allocate(g.matrixWords, HostBuffer<quint64>::Kind::Paged);
    if (!g.setRows.empty()) {
        // Брауэр–Циммерман: матрицы множеств уже упакованы планом.
        std::copy(g.setRows.begin(), g.setRows.end(), h_matrix.get());
    }
    else {
        for (quint64 i = 0; i < g.numOfRows; ++i) {
            quint64* rowData = h_matrix.get() + i * g.wordsPerRow;
            const QString& row = g.matrix[int(i)];
            for (quint64 j = 0; j < g.numOfCols; ++j)
                if (row.at(int(j)) == QLatin1Char('1'))
                    rowData[j / 64] |= (1ull << (j % 64));
        }
    }

    // Перебору по слоям нужна таблица биноминальных коэффициентов
    if (settings.layered()) {
        binomTable = g.isLongCode
            // Для длинных кодов строим только часть таблицы
            ? BinomTable(g.numOfRows, g.maxRows)
            // Для коротких — всю. Не оптимально, но работает.
            : BinomTable(Constants::MAX_SHORT_CODE_LENGTH, Constants::MAX_SHORT_CODE_LENGTH);
    }

    // Спектр на хосте. Для GPU нужна pinned-память — иначе не работает
    // асинхронное копирование; для CPU обычная, cudaMallocHost без видеокарты
    // недоступен.
    h_spectrum.allocate(g.spectrumSize, g.useGpu ? HostBuffer<quint64>::Kind::Pinned
                                                 : HostBuffer<quint64>::Kind::Paged);
    // Кольцо снимков нужно только видеокарте: на CPU спектр и так лежит в
    // h_spectrum, копировать его неоткуда.
    if (g.useGpu)
        spectrumRing.allocate(g.spectrumSize);
    else
        spectrumRing.reset();
    if (exportSpectrum) {
        // Продолжаем с чекпоинта — переносим накопленный спектр
        for (quint64 i = 0; i < g.spectrumSize; ++i)
            h_spectrum[i] = runState.spectrum.at(int(i));
    } else {
        h_spectrum.fillZero();
    }

    if (!g.useGpu)
        return;

    if (g.matrixInGlobalMem)
        CUDA_CALL(cudaMemcpy(d_matrix.get(), h_matrix.get(),
                             g.matrixWords * Constants::WORD_SIZE, cudaMemcpyHostToDevice));
    else
        CUDA_CALL(copyMatrixToConstant(h_matrix.get(), g.matrixWords));
    if (g.setCount > 1)
        CUDA_CALL(copyMasksToConstant(g.setMasks.data(), g.setCount, int(g.wordsPerRow)));

    d_spectrum.allocate(g.spectrumSize);
    if (exportSpectrum)
        CUDA_CALL(cudaMemcpy(d_spectrum.get(), h_spectrum.get(),
                             g.spectrumSize * sizeof(quint64), cudaMemcpyHostToDevice));
    else
        d_spectrum.fillZero();

    stream.create();

    // Ядро коротких кодов читает таблицу как binomTable[n * 64 + k]. BinomTable
    // хранит её плоско ровно с таким шагом, поэтому копируем как есть, без
    // промежуточного «уплощения».
    if (settings.layered() && !g.isLongCode) {
        Q_ASSERT(binomTable.stride() == Constants::MAX_SHORT_CODE_LENGTH + 1);
        d_binomTable.allocate(Constants::BINOM_TABLE_SIZE_FOR_SHORT_CODES);
        CUDA_CALL(cudaMemcpy(d_binomTable.get(), binomTable.data(),
                             binomTable.bytes(), cudaMemcpyHostToDevice));
    }
}

// Выбор вычислительной функции: алгоритм, устройство, длина кода.
void Worker::tuneGrid(CodeGeometry& g)
{
    if (!settings.autoTuneGrid || !g.useGpu)
        return;
    // У случайного поиска своё ядро и своя сетка — блок на попытку; подбор
    // здесь мерил бы чужое ядро, да ещё по пустой таблице биномов.
    if (settings.algorithmType == ComputationSettings::RandomInfoSets)
        return;

    GridTuneTask task;
    task.numOfCols   = int(g.numOfCols);
    task.numOfRows   = int(g.numOfRows);
    task.wordsPerRow = int(g.wordsPerRow);
    task.binomTable  = d_binomTable.get();
    task.chunkSize   = g.chunkSize;
    task.minWorthSeconds = tuneThresholdSec;
    task.userGrid    = { g.blocksGpu, g.threadsGpu };
    task.verbose     = tuneVerbose;
    task.slot.setCount = g.setCount;

    const bool gray = !settings.layered();
    task.kernel = g.isLongCode ? GridTuneTask::Kernel::XorLong
                : gray         ? GridTuneTask::Kernel::GrayShort
                               : GridTuneTask::Kernel::XorShort;

    // Отдельный буфер спектра: замер не имеет права попасть в настоящий.
    DeviceBuffer<quint64> scratch;
    scratch.allocate(g.spectrumSize);
    scratch.fillZero();
    task.scratchSpectrum = scratch.get();

    // Слой, на котором идёт замер, и его размер. У кода Грея слоёв нет —
    // маски нумеруются сплошь.
    quint64 layerRank = 0;
    if (task.kernel == GridTuneTask::Kernel::GrayShort) {
        task.availableMasks = 1ULL << g.numOfRows;
        task.totalMasks     = task.availableMasks;
    }
    else {
        // Меряем на самом населённом слое: там расчёт и проведёт почти всё
        // время, а стоимость маски зависит от числа складываемых строк.
        quint64 bestCount = 0;
        for (quint64 r = 0; r <= g.maxRows && r <= g.numOfRows; ++r) {
            const quint64 count = binomTable(g.numOfRows, r);
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
            generateStartPositions(layerRank + quint64(i) * task.measureMasksPerThread,
                                   rows, ones,
                                   host + i * Constants::MAX_POSITIONS, binomTable);
        }

        CUDA_CALL(cudaMemcpy(d_tuneSlots.get(), host,
                             slotCount * Constants::MAX_POSITIONS * sizeof(int16_t),
                             cudaMemcpyHostToDevice));

        task.startPositions   = d_tuneSlots.get();
        task.filledStartMasks = slotCount;
        task.matrixGlobal     = g.matrixInGlobalMem ? d_matrix.get() : nullptr;
    }

    const LaunchGrid grid = tuneLaunchGrid(task, stream.get());
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
    const bool gray = !settings.layered()
                   && settings.algorithmType != ComputationSettings::RandomInfoSets;

    // Код Грея перебирает 2^k масок в одном 64-битном слове, поэтому длиннее
    // 63 строк не бывает. Раньше это проверял только диалог настроек, а прямой
    // вызов Worker давал сдвиг на 64 и больше — неопределённое поведение.
    if (gray && g.isLongCode)
        throw std::invalid_argument(
            "код Грея неприменим: больше 63 строк не помещается в маску");

    if (settings.algorithmType == ComputationSettings::RandomInfoSets)
        computeSpectrumLeon(g);
    else if (gray)
        g.useGpu ? computeSpectrumGpuGrayShort(g)
                 : computeSpectrumCpuGrayShort(g);
    else if (g.isLongCode)
        g.useGpu ? computeSpectrumGpuNoGrayLong(g)
                 : computeSpectrumCpuNoGrayLong(g);
    else
        g.useGpu ? computeSpectrumGpuNoGrayShort(g)
                 : computeSpectrumCpuNoGrayShort(g);
}

// Забирает итоговый спектр, рассылает сигналы и освобождает ресурсы.
void Worker::finishComputation(const CodeGeometry& g, steady_clock::time_point startedAt)
{
    if (cancelled.load()) {
        initializeRunState(LoadMode::Reset);
        emit finished(Constants::ERROR_OCCURED);
        emit updateInfoPBR(0);
        updateSpectrum(int(g.numOfCols));
        releaseResources();
        return;
    }

    // Случайный поиск копит спектр на хосте, d_spectrum у него пустой —
    // забирать оттуда нечего, это затёрло бы найденное нулями.
    if (g.useGpu && settings.algorithmType != ComputationSettings::RandomInfoSets) {
        CUDA_CALL(cudaDeviceSynchronize());
        CUDA_CALL(cudaMemcpy(h_spectrum.get(), d_spectrum.get(),
                             g.spectrumSize * sizeof(quint64), cudaMemcpyDeviceToHost));
    }

    // Автосохранение остаётся и после успеха. Слои по числу складываемых строк
    // независимы и идут по возрастанию, поэтому досчитанный до maxRows спектр —
    // это ровно начало расчёта до большего maxRows. Состояние помечается как
    // «всё до maxRows пройдено», и следующий запуск продолжит со следующего
    // слоя, а не с нуля.
    if (settings.layered()) {
        runState.rOffset     = g.maxRows + 1;
        runState.chunkOffset = 0;
    }
    else if (settings.algorithmType == ComputationSettings::RandomInfoSets) {
        // Запись случайного поиска не продолжается — хранится только итог.
        runState.rOffset     = 0;
        runState.chunkOffset = 0;
    }
    else {
        // У кода Грея слоёв нет: пройденным считается весь диапазон масок.
        runState.rOffset     = 0;
        runState.chunkOffset = 1ULL << g.numOfRows;
    }
    runState.doneOps    = progress.doneOps();
    runState.elapsedSec = duration_cast<seconds>(steady_clock::now() - startedAt).count();

    // Пишется до преобразования Мак-Вильямс: в записи должен лежать сырой
    // спектр перебираемой матрицы, с него и продолжают.
    makeCheckpoint(int(g.numOfCols), true);

    initializeRunState(LoadMode::Reset);

    // Дуальный расчёт даёт спектр проверочной матрицы — исходный получается
    // из него преобразованием Мак-Вильямс.
    if (settings.algorithmType == ComputationSettings::Algorithm::DualCode)
        updateSpectrumDual(int(g.numOfCols), int(g.numOfRows));
    else
        updateSpectrum(int(g.numOfCols));

    emit updateInfoPBR(100);
    emit finished(int(duration_cast<seconds>(steady_clock::now() - startedAt).count()));

    releaseResources();
}

void Worker::computeSpectrumImpl()
{
    CodeGeometry g = describeTask();

    // Что записывать в автосохранение и что показать пользователю: глубину
    // перебора и множества он не задавал, они выведены из веса и матрицы.
    activeInfoSets = g.setColumns;
    activeMaxRows  = int(g.maxRows);
    activeTrials   = 0;
    if (g.guaranteedBelow > 0)
        emit planReady(g.setCount, int(g.maxRows), g.guaranteedBelow - 1);

    // Спектр мог прийти из чекпоинта — тогда размер уже верный
    if (quint64(runState.spectrum.size()) != g.spectrumSize)
        runState.spectrum.resize(int(g.spectrumSize));

    omp_set_num_threads(settings.compDevSet.threadsCpu);

    prepareBuffers(g);
    tuneGrid(g);

    // Частоты обновления из настроек. Сам отсчёт запускает вычислительная
    // функция: только она знает общее число операций.
    progress.setIntervals(
        std::chrono::milliseconds{ settings.timeIntSet.updateSpectrumInterval },
        std::chrono::seconds{ settings.timeIntSet.saveSpectrumInterval });
    progress.setOpsCheckpoint(checkpointEveryOps);

    const auto startedAt = steady_clock::now() - std::chrono::seconds(runState.elapsedSec);

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
    h_spectrum.reset();
    h_matrix.reset();
    binomTable = BinomTable();

    d_spectrum.reset();
    d_matrix.reset();
    d_binomTable.reset();

    stream.reset();
}


void Worker::pause()
{
    paused.store(1);
}

void Worker::resume()
{
    paused.store(0);
}

void Worker::cancel()
{
    cancelled.store(1);
}
void Worker::uncancel()
{
    cancelled.store(0);
}

bool Worker::isCancelled()
{
    return (bool)cancelled.load();
}


void Worker::setGridTuningThreshold(double seconds)
{
    tuneThresholdSec = seconds;
}

void Worker::setGridTuningVerbose(bool on)
{
    tuneVerbose = on;
}

void Worker::setCheckpointOpsPolicy(quint64 everyOps, quint64 stopAfter)
{
    checkpointEveryOps = everyOps;
    stopAfterOps       = stopAfter;
}

void Worker::setSettings(const QJsonObject& jsonSettings) {
    this->settings = ComputationSettings::fromJson(jsonSettings);
}

void Worker::initializeRunState(LoadMode lm)
{
    // Если сбрасываем состояние
    if ( lm == LoadMode::Reset ) {
        runState.rOffset = 0;
        runState.chunkOffset = 0;
        runState.elapsedSec = 0;
        runState.doneOps = 0;
        runState.spectrum.clear();
        exportSpectrum = false;
        resumedInfoSets.clear();
        return;
    }
    else {
        AutosaveRecord record;
        if (!autosave.load(settings.matrix, settings.algorithmType, record)
            || !canResume(record, settings)) {
            initializeRunState(LoadMode::Reset);
            return;
        }

        runState = record.state;
        // Продолжать Брауэра–Циммермана можно только по множествам записи
        resumedInfoSets = record.infoSets;
        // Ставим флаг, что надо выгрузить спектр
        exportSpectrum = true;
    }
}
