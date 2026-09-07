#include "worker.h"
#include "gridtuner.h"

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

    bool copyPending = false;
    progress.begin(totalCombinations(numOfRows, maxComb), runState.doneOps, runState.elapsedSec);

    // rOffset — число единиц в маске; счётчик обязан быть беззнаковым, иначе
    // при сравнении с maxComb получается знаковое/беззнаковое сравнение.
    for (quint64 r = runState.rOffset; r <= maxComb; r++)
    {
        quint64 startOffset = 0;

        quint64 curOps = binomTable(numOfRows, r);

        if (r == runState.rOffset)
            startOffset = runState.chunkOffset;
        // Разбиваем на чанки
        for ( quint64 offset = startOffset; offset < curOps; offset += chunkSize )
        {
            quint64 thisChunkSize = qMin(chunkSize, curOps - offset); // последний чанк может быть меньше
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
                offset,                // Смещение в комбинациях на каждом шаге
                thisChunkSize,         // Размер чанка
                r                      // Число единиц в битовой маске (число складываемых строк)
            );
            progress.addOps(thisChunkSize);
            runState.doneOps = progress.doneOps();

            const ProgressTracker::Due due = progress.due();

            if (due.estimate)
                reportEstimate();

            // Метка спектра двигается только когда копирование реально
            // началось: иначе при занятом копировании следующая попытка
            // откладывалась бы на целый интервал.
            if (!copyPending && due.spectrum) {
                progress.markSpectrum();
                cudaMemcpyAsync(h_spectrum.get(), d_spectrum.get(), (numOfCols + 1) * sizeof(quint64), cudaMemcpyDeviceToHost, stream.get());
                cudaEventRecord(ev.get(), stream.get());
                copyPending = true;
            }
            if (copyPending && cudaEventQuery(ev.get()) == cudaSuccess) {
                copyPending = false;
                updateSpectrum(numOfCols);
            }
            if (due.bar)
                reportProgressBar();

            if (due.checkpoint) {
                if (!saveGpuCheckpoint(stream.get(), numOfCols, r, offset + thisChunkSize))
                    return;
                copyPending = false;   // синхронная копия сделала async-копию ненужной
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
    CUDA_CALL(cudaEventRecord(ev.get(), stream.get()));
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

    bool          copyPending = false;
    const quint64 totalOps    = 1ULL << numOfRows;

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);
    // Обновление на случай, когда загружаем чекпоинт
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

        if (!copyPending && due.spectrum) {
            progress.markSpectrum();
            cudaMemcpyAsync(h_spectrum.get(), d_spectrum.get(), (numOfCols + 1) * sizeof(quint64), cudaMemcpyDeviceToHost, stream.get());
            cudaEventRecord(ev.get(), stream.get());
            copyPending = true;
        }
        if (copyPending && cudaEventQuery(ev.get()) == cudaSuccess) {
            copyPending = false;
            updateSpectrum(numOfCols);
        }
        if (due.bar)
            reportProgressBar();

        if (due.checkpoint) {
            // Код Грея идёт сплошной нумерацией масок, слоёв по числу единиц
            // нет — rOffset здесь всегда 0.
            if (!saveGpuCheckpoint(stream.get(), numOfCols, 0, chunkOffset + thisChunkSize))
                return;
            copyPending = false;
        }
    }

    // Финальная копия спектра
    CUDA_CALL(cudaMemcpyAsync(
        h_spectrum.get(),
        d_spectrum.get(),
        (numOfCols + 1) * sizeof(quint64),
        cudaMemcpyDeviceToHost,
        stream.get()));
    CUDA_CALL(cudaEventRecord(ev.get(), stream.get()));
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

    quint64 totalOps = totalCombinations(numOfRows, maxComb);

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
    CudaEvent  evCopy;
    localStream.create();
    evCopy.create();

    bool copyPending = false;

    // Размер чанка в масках
    const quint64 chunkSizeTarget = maxThreads * masksPerThread;

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    // Внешний цикл по числу единиц в маске
    for (quint64 r = runState.rOffset; r <= maxComb; ++r) {

        quint64 startOffset = 0;

        // Количество масок с r единицами
        quint64 curOps = binomTable(numOfRows, r);
        if (curOps == 0) continue;

        if (r == runState.rOffset)
            startOffset = runState.chunkOffset;

        // Внутренний цикл по чанку
        for (quint64 chunkOffset = startOffset; chunkOffset < curOps; chunkOffset += chunkSizeTarget) {
            while (paused.load() != 0) {
                QThread::msleep(50);
                if (cancelled.load()) break;
            }
            // Получаем фактический размер чанка в масках
            quint64 chunkSize = std::min(chunkSizeTarget, curOps - chunkOffset);

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
                quint64 globalRank = chunkOffset + tid * masksPerThread;
                assert(globalRank < curOps);
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
                d_maskCounter.get());
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
                nullptr);
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

            // Асинхронное копирование спектра для обновления интерфейса
            if (!copyPending && due.spectrum) {
                progress.markSpectrum();
                CUDA_CALL(cudaMemcpyAsync(
                    h_spectrum.get(),
                    d_spectrum.get(),
                    (numOfCols + 1) * sizeof(quint64),
                    cudaMemcpyDeviceToHost,
                    localStream.get()));
                CUDA_CALL(cudaEventRecord(evCopy.get(), localStream.get()));
                copyPending = true;
            }
            if (copyPending && cudaEventQuery(evCopy.get()) == cudaSuccess) {
                copyPending = false;
                updateSpectrum(numOfCols);
            }

            if (due.bar)
                reportProgressBar();
            if (due.estimate)
                reportEstimate();

            if (due.checkpoint) {
                if (!saveGpuCheckpoint(localStream.get(), numOfCols, r, chunkOffset + chunkSize))
                    return;
                copyPending = false;
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
    CUDA_CALL(cudaEventRecord(evCopy.get(), localStream.get()));
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
    quint64 totalOps = totalCombinations(numOfRows, maxComb);

    // Число "битовых масок", обрабатываемых одним потоком
    const uint64_t masksPerThread = 1ULL << 20; // можно настроить
    const quint64 chunkSizeTarget = numThreads * masksPerThread;

    std::vector<std::vector<quint64>> threadSpectrum( numThreads, std::vector<quint64>(numOfCols + 1, 0ULL) );

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    // Основной внешний цикл по числу единиц в маске
    for (quint64 r = runState.rOffset; r <= maxComb; ++r)
    {
        quint64 curOps = binomTable(numOfRows, r);

        quint64 startOffset = (r == runState.rOffset) ? runState.chunkOffset : 0;
        if (curOps == 0) continue;

        // Внутренний цикл по чанкам
        for (quint64 chunkOffset = startOffset; chunkOffset < curOps; chunkOffset += chunkSizeTarget) {
            if (cancelled.load()) break;

            quint64 chunkSize = std::min(chunkSizeTarget, curOps - chunkOffset);
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

                    uint64_t globalRank = chunkOffset + startRank; // ранг относительно binom curOps

                    // 1) Сгенерировать стартовые позиции
                    generateStartPositions(globalRank, (int)numOfRows, (int)r, a_local, binomTable);

                    // 2) Построить начальное codeword XOR-ом строк
                    // обнуляем codeword
                    for (size_t w = 0; w < (size_t)wordsPerRow; ++w) codeword[w] = 0ULL;

                    for (int i = 0; i < (int)r; ++i) {
                        int row = a_local[i];
                        quint64* rowData = h_matrix.get() + (quint64)row * wordsPerRow;
                        for (size_t w = 0; w < (size_t)wordsPerRow; ++w)
                            codeword[w] ^= rowData[w];
                    }

                    // посчитать вес и добавить в локальный спектр
                    quint64 weight = 0;
                    for (size_t w = 0; w < (size_t)wordsPerRow; ++w) weight += __popcnt64(codeword[w]);
                    if (weight <= numOfCols) localSpectrum[(size_t)weight]++;

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
                                quint64* rowData = h_matrix.get() + (quint64)row * wordsPerRow;
                                for (size_t w = 0; w < (size_t)wordsPerRow; ++w)
                                    codeword[w] ^= rowData[w];
                            }
                        }
                        else {
                            // применяем XOR для каждой изменённой строки
                            for (int t = 0; t < numChanged; ++t) {
                                int row = changed[t];
                                quint64* rowData = h_matrix.get() + (quint64)row * wordsPerRow;
                                for (size_t w = 0; w < (size_t)wordsPerRow; ++w)
                                    codeword[w] ^= rowData[w];
                            }
                        }

                        // считаем вес
                        quint64 weight2 = 0;
                        for (size_t w = 0; w < (size_t)wordsPerRow; ++w) weight2 += __popcnt64(codeword[w]);
                        if (weight2 <= numOfCols) localSpectrum[(size_t)weight2]++;
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

    quint64 totalOps = totalCombinations(numOfRows, maxComb);

    // Если продолжаем после чекпоинта
    quint64 startR = runState.rOffset;
    quint64 startOffsetForStartR = runState.chunkOffset;

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    for (quint64 r = startR; r <= maxComb; ++r)
    {
        if (cancelled.load())
            break;

        quint64 combCount = binomTable(numOfRows, r);
        if (combCount == 0)
            continue;

        quint64 startOffset = (r == startR) ? startOffsetForStartR : 0;

        for (quint64 offset = startOffset; offset < combCount; offset += chunkSize)
        {
            if (cancelled.load())
                break;

            while (paused.load() != 0) {
                QThread::msleep(50);
                if (cancelled.load())
                    break;
            }

            quint64 thisChunkSize = qMin(chunkSize, combCount - offset);

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
                const quint64 startIdx  = offset + quint64(tid) * perThread;
                const quint64 endIdx    = std::min(startIdx + perThread, offset + thisChunkSize);

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
                                h_matrix.get() + (numOfRows - 1 - p) * wordsPerRow;
                            for (quint64 b = 0; b < wordsPerRow; ++b)
                                localCodeword[b] ^= rowData[b];
                        }
                    };
                    auto accumulate = [&]() {
                        quint64 weight = 0;
                        for (quint64 b = 0; b < wordsPerRow; ++b)
                            weight += __popcnt64(localCodeword[b]);
                        if (weight <= numOfCols)
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
void Worker::updateSpectrum(int numOfCols)
{
    if (!h_spectrum.get())
        return;
    bool spectrumEmpty = true;
    QStringList spectrumCopyPTE;
    SpectrumFloat spectrumCopyPlot;

    quint64 val = 0;
    for (quint64 w = 0; w <= numOfCols; ++w) {
        spectrumCopyPlot.append(float(h_spectrum[w]));
        if (h_spectrum[w] != 0) {
            val += h_spectrum[w];
            spectrumCopyPTE.append(QString::number(w) + " - " + QString::number(h_spectrum[w]));
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

    g.matrixInGlobalMem =
        g.useGpu && (g.matrixWords * Constants::WORD_SIZE > Constants::CONST_MEM_SIZE);

    return g;
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
    for (quint64 i = 0; i < g.numOfRows; ++i) {
        quint64* rowData = h_matrix.get() + i * g.wordsPerRow;
        const QString& row = g.matrix[int(i)];
        for (quint64 j = 0; j < g.numOfCols; ++j)
            if (row.at(int(j)) == QLatin1Char('1'))
                rowData[j / 64] |= (1ull << (j % 64));
    }

    // Простому перебору нужна таблица биноминальных коэффициентов
    if (settings.algorithmType == ComputationSettings::SimpleXor) {
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

    d_spectrum.allocate(g.spectrumSize);
    if (exportSpectrum)
        CUDA_CALL(cudaMemcpy(d_spectrum.get(), h_spectrum.get(),
                             g.spectrumSize * sizeof(quint64), cudaMemcpyHostToDevice));
    else
        d_spectrum.fillZero();

    ev.create();
    stream.create();

    // Ядро коротких кодов читает таблицу как binomTable[n * 64 + k]. BinomTable
    // хранит её плоско ровно с таким шагом, поэтому копируем как есть, без
    // промежуточного «уплощения».
    if (settings.algorithmType == ComputationSettings::SimpleXor && !g.isLongCode) {
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

    GridTuneTask task;
    task.numOfCols   = int(g.numOfCols);
    task.numOfRows   = int(g.numOfRows);
    task.wordsPerRow = int(g.wordsPerRow);
    task.binomTable  = d_binomTable.get();
    task.chunkSize   = g.chunkSize;
    task.minWorthSeconds = tuneThresholdSec;
    task.userGrid    = { g.blocksGpu, g.threadsGpu };
    task.verbose     = tuneVerbose;

    const bool gray = settings.algorithmType != ComputationSettings::SimpleXor;
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
        task.totalMasks     = totalCombinations(g.numOfRows, g.maxRows);
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
    const bool gray = settings.algorithmType != ComputationSettings::SimpleXor;

    // Код Грея перебирает 2^k масок в одном 64-битном слове, поэтому длиннее
    // 63 строк не бывает. Раньше это проверял только диалог настроек, а прямой
    // вызов Worker давал сдвиг на 64 и больше — неопределённое поведение.
    if (gray && g.isLongCode)
        throw std::invalid_argument(
            "код Грея неприменим: больше 63 строк не помещается в маску");

    if (gray)
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

    if (g.useGpu) {
        CUDA_CALL(cudaDeviceSynchronize());
        CUDA_CALL(cudaMemcpy(h_spectrum.get(), d_spectrum.get(),
                             g.spectrumSize * sizeof(quint64), cudaMemcpyDeviceToHost));
    }

    // Автосохранение остаётся и после успеха. Слои по числу складываемых строк
    // независимы и идут по возрастанию, поэтому досчитанный до maxRows спектр —
    // это ровно начало расчёта до большего maxRows. Состояние помечается как
    // «всё до maxRows пройдено», и следующий запуск продолжит со следующего
    // слоя, а не с нуля.
    if (settings.algorithmType == ComputationSettings::SimpleXor) {
        runState.rOffset     = g.maxRows + 1;
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

    ev.reset();
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
        // Ставим флаг, что надо выгрузить спектр
        exportSpectrum = true;
    }
}
