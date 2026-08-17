#include "worker.h"

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
    uint64_t** C   // binomTable
) {
    int x = 0;
    for (int i = 0; i < numOfOnes; ++i) {
        for (int v = x; v <= numOfRows - numOfOnes + i; ++v) {
            uint64_t cnt = C[numOfRows - v - 1][numOfOnes - i - 1];
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
// Функция строит треугольную таблицу биноминальных коэффициентов
quint64** Worker::buildBinomTable(unsigned maxN, unsigned maxComb) {
    // Выделяем память под столбцы длины maxN+1
    quint64** C = new quint64 * [maxN + 1];
    constexpr quint64 U = std::numeric_limits<quint64>::max();

    // Выделяем память под строки длины maxComb+1
    for (unsigned n = 0; n <= maxN; ++n) {
        C[n] = new quint64[maxComb + 1];
        // Заполняем строки
        for (unsigned r = 0; r <= maxComb; ++r) {
            if (r == 0) {
                C[n][r] = 1;
            }
            else if (r > n) {
                C[n][r] = 0; // если r>n — 0 (не существует)
            }
            else {
                // C(n,r) = C(n-1,r-1) + C(n-1,r)
                quint64 a = (n >= 1) ? C[n - 1][r - 1] : 0;
                quint64 b = (n >= 1) ? C[n - 1][r] : 0;
                quint64 sum = a + b;
                if (sum < a) throw "Error: Combin overflow";
                C[n][r] = sum;
            }
        }
    }
    return C;
}
// Функция, освобождающая память из под таблицы с комбинами
void Worker::freeBinomTable(quint64** C, unsigned maxN)
{
    if (!C) return;

    for (unsigned n = 0; n <= maxN; ++n) {
        delete[] C[n];
    }
    delete[] C;
}

// Получение битовой маски длины k с r единицами с индексом rank
static quint64 unrankCombination( unsigned K, unsigned R, quint64 rank, quint64** binomTable )
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
                count = binomTable[remainingPositions][remainingToChoose];

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
// Вычисляет сумму сочетаний C(k,i) где i пробегает от 0 до maxComb
quint64 Worker::sumCombinations(quint64 k, quint64 maxComb)
{
    if (maxComb > k) maxComb = k;
    if (maxComb == k) return ( ( 1ull << k ) - 1 );
    quint64 sum = 0;
    quint64 comb = 1; // C(k,0) = 1
    for (quint64 r = 1; r <= maxComb; ++r) {
        comb = comb * (k - r + 1) / r;
        sum += comb;
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
                                progress.speed());
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
        err = cudaMemcpy(h_spectrum, d_spectrum, (numOfCols + 1) * sizeof(quint64),
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

void Worker::computeSpectrumGpuNoGrayShort(quint64 numOfRows, quint64 numOfCols, quint64 wordsPerRow, quint64 chunkSize, int blockCount, int threadsPerBlock, quint64 maxComb)
{
    bool copyPending = false;
    progress.begin(sumCombinations(numOfRows, maxComb), runState.doneOps, runState.elapsedSec);

    // rOffset — число единиц в маске; счётчик обязан быть беззнаковым, иначе
    // при сравнении с maxComb получается знаковое/беззнаковое сравнение.
    for (quint64 r = runState.rOffset; r <= maxComb; r++)
    {
        quint64 startOffset = 0;

        quint64 curOps = h_binomTable[numOfRows][r];

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
                d_spectrum,
                d_binomTable,
                blockCount,             // Число блоков
                threadsPerBlock,       // Число нитей на блок
                stream,
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
                cudaMemcpyAsync(h_spectrum, d_spectrum, (numOfCols + 1) * sizeof(quint64), cudaMemcpyDeviceToHost, stream);
                cudaEventRecord(ev, stream);
                copyPending = true;
            }
            if (copyPending && cudaEventQuery(ev) == cudaSuccess) {
                copyPending = false;
                updateSpectrum(numOfCols);
            }
            if (due.bar)
                reportProgressBar();

            if (due.checkpoint) {
                if (!saveGpuCheckpoint(stream, numOfCols, r, offset + thisChunkSize))
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
        h_spectrum,
        d_spectrum,
        (numOfCols + 1) * sizeof(quint64),
        cudaMemcpyDeviceToHost,
        stream));
    CUDA_CALL(cudaEventRecord(ev, stream));
    CUDA_CALL(cudaStreamSynchronize(stream));

    updateSpectrum(numOfCols);
}
void Worker::computeSpectrumGpuGrayShort( quint64 numOfRows, quint64 numOfCols, quint64 wordsPerRow, quint64 chunkSize, int blockCount, int threadsPerBlock )
{
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
            stream,
            d_spectrum,
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
            cudaMemcpyAsync(h_spectrum, d_spectrum, (numOfCols + 1) * sizeof(quint64), cudaMemcpyDeviceToHost, stream);
            cudaEventRecord(ev, stream);
            copyPending = true;
        }
        if (copyPending && cudaEventQuery(ev) == cudaSuccess) {
            copyPending = false;
            updateSpectrum(numOfCols);
        }
        if (due.bar)
            reportProgressBar();

        if (due.checkpoint) {
            // Код Грея идёт сплошной нумерацией масок, слоёв по числу единиц
            // нет — rOffset здесь всегда 0.
            if (!saveGpuCheckpoint(stream, numOfCols, 0, chunkOffset + thisChunkSize))
                return;
            copyPending = false;
        }
    }

    // Финальная копия спектра
    CUDA_CALL(cudaMemcpyAsync(
        h_spectrum,
        d_spectrum,
        (numOfCols + 1) * sizeof(quint64),
        cudaMemcpyDeviceToHost,
        stream));
    CUDA_CALL(cudaEventRecord(ev, stream));
    CUDA_CALL(cudaStreamSynchronize(stream));

    updateSpectrum(numOfCols);
}

void Worker::computeSpectrumGpuNoGrayLong(
    quint64 numOfRows,
    quint64 numOfCols,
    quint64 wordsPerRow,
    int     blockCount,
    int     threadsPerBlock,
    quint64 maxComb,
    quint64* d_matrix
)
{
    quint64 totalOps = sumCombinations(numOfRows, maxComb);

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

    // Стартовые массивы масок на процессоре и на видеокарте
    int16_t* h_slots = nullptr;
    int16_t* d_slots = nullptr;
    CUDA_CALL(cudaMallocHost((void**)&h_slots, bytesPerChunk));
    CUDA_CALL(cudaMalloc((void**)&d_slots, bytesPerChunk));

    #ifdef _DEBUG
    uint64_t* h_maskCounter = nullptr;
    uint64_t* d_maskCounter = nullptr;
    CUDA_CALL(cudaMallocHost( (void**)&h_maskCounter, sizeof(uint64_t) ) );
    CUDA_CALL(cudaMalloc(     (void**)&d_maskCounter, sizeof(uint64_t) ) );
    #endif
    // Собственный поток, а не поле класса: имя намеренно отличается, чтобы
    // не перекрывать Worker::stream и не синхронизировать по ошибке чужой.
    cudaStream_t localStream = nullptr;
    cudaEvent_t  evCopy      = nullptr;
    CUDA_CALL(cudaStreamCreate(&localStream));
    CUDA_CALL(cudaEventCreate(&evCopy));

    bool copyPending = false;

    // Размер чанка в масках
    const quint64 chunkSizeTarget = maxThreads * masksPerThread;

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    // Внешний цикл по числу единиц в маске
    for (quint64 r = runState.rOffset; r <= maxComb; ++r) {

        quint64 startOffset = 0;

        // Количество масок с r единицами
        quint64 curOps = h_binomTable[numOfRows][r];
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

            // Генерируем стартовые позиции на CPU
            for (quint64 tid = 0; tid < numStartMasks; ++tid) {
                quint64 globalRank = chunkOffset + tid * masksPerThread;
                assert(globalRank < curOps);
                // Генерируем стартовую комбинацию для ранга globalRank
                generateStartPositions(globalRank, numOfRows, r, h_slots + tid * Constants::MAX_POSITIONS, h_binomTable);
            }

            // Копируем только те стартовые маски, которые нужны в этом чанке
            CUDA_CALL(cudaMemcpyAsync(
                d_slots,
                h_slots,
                numStartMasks * slotBytes,
                cudaMemcpyHostToDevice,
                localStream));

            // Запускаем ядро: numStartMasks потоков (упаковано в grid)
            int grid = (numStartMasks + threadsPerBlock - 1) / threadsPerBlock;
            if (grid <= 0) grid = 1;

            // Параметры: chunkSize (сколько масок в этом чанке всего),
            // masksPerThread (сколько масок на поток), numStartMasks (количество активных потоков)
            #ifdef _DEBUG
            cudaMemset(d_maskCounter, 0, sizeof(uint64_t));
            
            launchSpectrumKernelLong(
                grid,
                threadsPerBlock,
                localStream,
                d_spectrum,
                d_matrix,
                static_cast<int>(numOfCols),
                static_cast<int>(numOfRows),
                static_cast<int>(wordsPerRow),
                chunkSize,
                d_slots,
                masksPerThread,
                numStartMasks,
                r,
                d_maskCounter);
            #else
            launchSpectrumKernelLong(
                grid,
                threadsPerBlock,
                localStream,
                d_spectrum,
                d_matrix,
                static_cast<int>(numOfCols),
                static_cast<int>(numOfRows),
                static_cast<int>(wordsPerRow),
                chunkSize,
                d_slots,
                masksPerThread,
                numStartMasks,
                r,
                nullptr);
            #endif
            // НЕ УДАЛЯТЬ. Без этой синхронизации спектр считается неверно.
            //
            // h_slots — pinned-память, из которой идёт асинхронное копирование
            // на устройство. На следующей итерации цикл начинает переписывать
            // h_slots стартовыми позициями нового чанка, а копия предыдущего
            // может быть ещё в полёте — хост затирает данные под работающим
            // DMA, и часть масок теряется.
            //
            // Убрать синхронизацию можно только вместе с двойной буферизацией
            // h_slots, и это отдельная задача со своим прогоном против эталона,
            // а не побочный эффект рефакторинга.
            cudaDeviceSynchronize();

            #ifdef _DEBUG
            cudaMemcpy(h_maskCounter, d_maskCounter, sizeof(uint64_t), cudaMemcpyDeviceToHost);
            uint64_t m = *h_maskCounter;
            if (m != chunkSize) {
                throw "Error, masks mismath!";
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
                    h_spectrum,
                    d_spectrum,
                    (numOfCols + 1) * sizeof(quint64),
                    cudaMemcpyDeviceToHost,
                    localStream));
                CUDA_CALL(cudaEventRecord(evCopy, localStream));
                copyPending = true;
            }
            if (copyPending && cudaEventQuery(evCopy) == cudaSuccess) {
                copyPending = false;
                updateSpectrum(numOfCols);
            }

            if (due.bar)
                reportProgressBar();
            if (due.estimate)
                reportEstimate();

            if (due.checkpoint) {
                if (!saveGpuCheckpoint(localStream, numOfCols, r, chunkOffset + chunkSize))
                    return;
                copyPending = false;
            }

            if (cancelled.load()) break;
        } // chunkOffset
        if (cancelled.load()) break;
    } // for r

    // Финальная копия спектра
    CUDA_CALL(cudaMemcpyAsync(
        h_spectrum,
        d_spectrum,
        (numOfCols + 1) * sizeof(quint64),
        cudaMemcpyDeviceToHost,
        localStream));
    CUDA_CALL(cudaEventRecord(evCopy, localStream));
    CUDA_CALL(cudaStreamSynchronize(localStream));

    updateSpectrum(numOfCols);
    // Очистка
    cudaFree(d_slots);
    cudaFreeHost(h_slots);
    //cudaFree(d_maskCounter);
    cudaStreamDestroy(localStream);
    cudaEventDestroy(evCopy);
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

void Worker::computeSpectrumCpuNoGrayLong(
    quint64 numOfRows,
    quint64 numOfCols,
    quint64 wordsPerRow,
    quint64 maxComb)
{
    using namespace std::chrono;
    int numThreads = omp_get_max_threads();
    quint64 totalOps = sumCombinations(numOfRows, maxComb);

    // Число "битовых масок", обрабатываемых одним потоком
    const uint64_t masksPerThread = 1ULL << 20; // можно настроить
    const quint64 chunkSizeTarget = numThreads * masksPerThread;

    std::vector<std::vector<quint64>> threadSpectrum( numThreads, std::vector<quint64>(numOfCols + 1, 0ULL) );

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    // Основной внешний цикл по числу единиц в маске
    for (quint64 r = runState.rOffset; r <= maxComb; ++r)
    {
        quint64 curOps = h_binomTable[numOfRows][r];

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
                    generateStartPositions(globalRank, (int)numOfRows, (int)r, a_local, h_binomTable);

                    // 2) Построить начальное codeword XOR-ом строк
                    // обнуляем codeword
                    for (size_t w = 0; w < (size_t)wordsPerRow; ++w) codeword[w] = 0ULL;

                    for (int i = 0; i < (int)r; ++i) {
                        int row = a_local[i];
                        quint64* rowData = h_matrix + (quint64)row * wordsPerRow;
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
                                quint64* rowData = h_matrix + (quint64)row * wordsPerRow;
                                for (size_t w = 0; w < (size_t)wordsPerRow; ++w)
                                    codeword[w] ^= rowData[w];
                            }
                        }
                        else {
                            // применяем XOR для каждой изменённой строки
                            for (int t = 0; t < numChanged; ++t) {
                                int row = changed[t];
                                quint64* rowData = h_matrix + (quint64)row * wordsPerRow;
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
            // Спектр в h_spectrum уже актуален, копировать ниоткуда не надо.
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

void Worker::computeSpectrumCpuGrayShort(
    quint64 numOfRows,
    quint64 numOfCols,
    quint64 wordsPerRow
)
{
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
                        h_matrix + pos * wordsPerRow;
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
                        h_matrix + pos * wordsPerRow;
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

void Worker::computeSpectrumCpuNoGrayShort(
    quint64 numOfRows,
    quint64 numOfCols,
    quint64 wordsPerRow,
    quint64 maxComb
)
{
    using namespace std::chrono;

    quint64 totalOps = sumCombinations(numOfRows, maxComb);

    // Если продолжаем после чекпоинта
    quint64 startR = runState.rOffset;
    quint64 startOffsetForStartR = runState.chunkOffset;

    progress.begin(totalOps, runState.doneOps, runState.elapsedSec);

    for (quint64 r = startR; r <= maxComb; ++r)
    {
        if (cancelled.load())
            break;

        quint64 combCount = h_binomTable[numOfRows][r];
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

            #pragma omp parallel
            {
                QVector<quint64> localSpectrum(numOfCols + 1, 0);
                QVector<quint64> localCodeword(wordsPerRow, 0);

                #pragma omp for schedule(static)
                for (qint64 idx = (qint64)offset; idx < (qint64)(offset + thisChunkSize); ++idx)
                {
                    if (cancelled.load())
                        continue;

                    quint64 mask = unrankCombination(numOfRows, r, (quint64)idx, h_binomTable);
                    std::fill(localCodeword.begin(), localCodeword.end(), 0);

                    for (quint64 i = 0; i < numOfRows; ++i) {
                        if (mask & (1ULL << i)) {
                            quint64* rowData = h_matrix + i * wordsPerRow;
                            for (quint64 b = 0; b < wordsPerRow; ++b)
                                localCodeword[b] ^= rowData[b];
                        }
                    }

                    quint64 weight = 0;
                    for (quint64 b = 0; b < wordsPerRow; ++b)
                        weight += __popcnt64(localCodeword[b]);

                    if (weight <= numOfCols)
                        localSpectrum[weight]++;
                }

                #pragma omp critical
                {
                    for (quint64 w = 0; w <= numOfCols; ++w)
                        chunkSpectrum[w] += localSpectrum[w];
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
    if (!h_spectrum)
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
    if (!h_spectrum)
        return;

    bool spectrumEmpty = true;
    // Считаем текстовый спектр из дуального
    QStringList spectrumCopyPTE = computeSpectrumFromDual( h_spectrum, numOfCols, numOfRows );
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
void Worker::makeCheckpoint(int numOfCols)
{
    // 1. Обновляем spectrum из GPU буфера
    runState.spectrum.resize(numOfCols + 1);
    for (int i = 0; i < numOfCols + 1; i++) {
        runState.spectrum[i] = h_spectrum[i];
    }
    // 2. Хеш → имя группы
    quint64 hash = settings.computeHash();
    QString group = QString("checkpoints/%1").arg(hash);
    QSettings s;
    s.beginGroup(group);
    // 3. Сохраняем settings
    s.setValue("settings", settings.toJson());
    // 4. Сохраняем runState
    s.setValue("runState", runState.toJson());
    s.endGroup();
    emit showSaveLBL();
}
void Worker::computeSpectrum()
{

    /*  РАБОТА С МАТРИЦЕЙ   */
    QStringList matrix = settings.matrix;
    // Если применяется дуальный код - генерируем проверочную матрицу
    if (settings.algorithmType == ComputationSettings::DualCode) {
        matrix = generatorToParity(matrix);
    }
    // Число строк и столбцов матрицы
    quint64 numOfRows = matrix.length();
    quint64 numOfCols = matrix[0].length();
    quint64 maxRows = settings.maxRows;
    quint64 spectrumSize = numOfCols + 1;
    // Если спектр уже есть, то ничего не произойдет
    if (runState.spectrum.size() != spectrumSize)
        runState.spectrum.resize(spectrumSize);

    /*  НАСТРОЙКИ ВЫЧИСЛИТЕЛЯ   */
    // Устанавливаем число потоков CPU
    int workerThreads = settings.compDevSet.threadsCpu;
    omp_set_num_threads(workerThreads);
    // Число блоков для запуска на видеокарте ( минимум - число мультипроцессоров )
    int blockCount = settings.compDevSet.blocksGpu;
    // Число нитей, запускаемых на одном блоке
    int threadsPerBlock = settings.compDevSet.threadsGpu;


    // Число 64-битных слов на одну строку матрицы
    quint64 wordsPerRow = (numOfCols + 63) / 64;
    // Размер матрицы в 64-битных словах
    quint64 matrixSizeInWords = numOfRows * wordsPerRow;
    bool matrixInGlobalMem = false;
    if ((matrixSizeInWords * Constants::WORD_SIZE > Constants::CONST_MEM_SIZE)
        && settings.compDev == ComputationSettings::ComputeDevice::GPU) {

        /* ДОПИСАТЬ КОПИРОВАНИЕ МАТРИЦЫ В ПАМЯТЬ ДЛЯ КОРОТКИХ КОДОВ */
        if (numOfRows < 64) {
            emit errorOccurred("Ошибка, матрица слишком большая");
            emit finished(Constants::ERROR_OCCURED);
            return;
        }
        CUDA_CALL(cudaMalloc((void**)&d_matrix, matrixSizeInWords * Constants::WORD_SIZE));
        matrixInGlobalMem = true;
    }
    // Выделяем матрицу на хосте
    h_matrix = (quint64*)calloc(matrixSizeInWords, Constants::WORD_SIZE);
    if (!h_matrix) {
        emit errorOccurred("Ошибка выделения памяти");
        emit finished(Constants::ERROR_OCCURED);
        return;
    }
    // Копируем из QStringList-а
    for (quint64 i = 0; i < numOfRows; ++i) {
        quint64* rowData = h_matrix + i * wordsPerRow;
        const QString& row = matrix[(int)i];
        for (quint64 j = 0; j < numOfCols; ++j) {
            if (row.at((int)j) == QLatin1Char('1')) {
                quint64 blockIdx = j / 64;
                quint64 bitIdx = j % 64;
                rowData[blockIdx] |= (1ull << bitIdx);
            }
        }
    }
    // Если используется простой перебор, то необходима таблица биноминальных коэффициентов
    if (settings.algorithmType == ComputationSettings::SimpleXor) {
        // Для коротких - строим всю таблицу. Не оптимально, но работает.
        if (numOfRows < 64) {
            h_binomTable = buildBinomTable(Constants::MAX_SHORT_CODE_LENGTH, Constants::MAX_SHORT_CODE_LENGTH);
        }
        // Для длинных кодов строим только часть таблицы
        else {
            h_binomTable = buildBinomTable(numOfRows, settings.maxRows);
        }



    }

    // Если расчет производится на GPU
    if (settings.compDev == ComputationSettings::GPU) {
        if (!matrixInGlobalMem) {
            // Копируем порождающую матрицу в константную память
            CUDA_CALL(copyMatrixToConstant(h_matrix, matrixSizeInWords));
        }
        else {
            // Если матрица слишком большая - копируем её в глобальную память
            CUDA_CALL(cudaMemcpy(d_matrix, h_matrix, matrixSizeInWords * Constants::WORD_SIZE, cudaMemcpyHostToDevice));
        }

        // Выделяем оперативную память
        CUDA_CALL(cudaMallocHost((void**)&h_spectrum, (spectrumSize) * sizeof(quint64)));
        
        if (exportSpectrum) {
            // Если загружаемся с чекпоинта, то копируем спектр
            for (int i = 0; i < spectrumSize; i++)
            {
                h_spectrum[i] = runState.spectrum.at(i);
            }
        }
        else {
            // Иначе - заполняем нулями
            memset(h_spectrum, 0, (spectrumSize) * sizeof(quint64));
        }
        
        
        // Выделяем видеопамять
        CUDA_CALL(cudaMalloc((void**)&d_spectrum, (spectrumSize) * sizeof(quint64)));

        if (exportSpectrum) {
            // Если загружаемся с чекпоинта, то копируем спектр с хоста на устройство
            CUDA_CALL(cudaMemcpy(d_spectrum, h_spectrum, (spectrumSize) * sizeof(quint64), cudaMemcpyHostToDevice));
        }
        else {
            // Иначе - заполняем нулями
            CUDA_CALL(cudaMemset(d_spectrum, 0, ((spectrumSize) * sizeof(quint64))));
        }
        
        CUDA_CALL(cudaEventCreate(&ev));
        CUDA_CALL(cudaStreamCreate(&stream));

        // Если расчитываем спектр короткого кода простым XOR - ом
        if (settings.algorithmType == ComputationSettings::SimpleXor && (numOfRows < 64)) {
            // Переводим двумерный массив биноминальных коэффициентов в одномерный
            int width = Constants::MAX_SHORT_CODE_LENGTH + 1;
            quint64* flat = (quint64*)malloc(Constants::BINOM_TABLE_SIZE_FOR_SHORT_CODES * sizeof(quint64));

            for (int n = 0; n <= Constants::MAX_SHORT_CODE_LENGTH; ++n) {
                for (int r = 0; r <= n; ++r) {
                    flat[n * width + r] = h_binomTable[n][r];
                }
            }
            CUDA_CALL(cudaMalloc((void**)&d_binomTable, Constants::BINOM_TABLE_SIZE_FOR_SHORT_CODES * sizeof(quint64)));
            CUDA_CALL(cudaMemcpy(d_binomTable, flat, Constants::BINOM_TABLE_SIZE_FOR_SHORT_CODES * sizeof(quint64), cudaMemcpyHostToDevice));
            free(flat);
        }
    }
    else {
        h_spectrum = (quint64*)malloc((spectrumSize) * sizeof(quint64));
        if (exportSpectrum) {
            // Если загружаемся с чекпоинта, то копируем спектр
            for (int i = 0; i < spectrumSize; i++)
            {
                h_spectrum[i] = runState.spectrum.at(i);
            }
        }
        else {
            // Иначе - заполняем нулями
            memset(h_spectrum, 0, (spectrumSize) * sizeof(quint64));
        }
    }

    // Частоты обновления из настроек. Отсчёт запускает сама функция расчёта:
    // только она знает общее число операций.
    progress.setIntervals(
        std::chrono::seconds{ settings.timeIntSet.updateSpectrumInterval },
        std::chrono::seconds{ settings.timeIntSet.saveSpectrumInterval });
    progress.setOpsCheckpoint(checkpointEveryOps);

    const auto runStartedAt = steady_clock::now() - std::chrono::seconds(runState.elapsedSec);

    switch (settings.algorithmType) {
        // В дуальном коде для ускорения используется код Грея
        case ComputationSettings::Algorithm::DualCode:
        case ComputationSettings::Algorithm::GrayCode: {
            if (settings.compDev == ComputationSettings::ComputeDevice::GPU) {
                computeSpectrumGpuGrayShort(
                    numOfRows,
                    numOfCols,
                    wordsPerRow,
                    chunkSize,
                    settings.compDevSet.blocksGpu,
                    settings.compDevSet.threadsGpu
                );
            }
            else {
                computeSpectrumCpuGrayShort(
                    numOfRows,
                    numOfCols,
                    wordsPerRow
                );
            };
        } break;
        case ComputationSettings::Algorithm::SimpleXor: {
            if ( settings.compDev == ComputationSettings::ComputeDevice::GPU ) {
                if ( numOfRows <= 63 ) {
                    computeSpectrumGpuNoGrayShort(
                        numOfRows,
                        numOfCols,
                        wordsPerRow,
                        chunkSize,
                        settings.compDevSet.blocksGpu,
                        settings.compDevSet.threadsGpu,
                        maxRows
                    );
                } else {
                    computeSpectrumGpuNoGrayLong(
                        numOfRows,
                        numOfCols,
                        wordsPerRow,
                        settings.compDevSet.blocksGpu,
                        settings.compDevSet.threadsGpu,
                        maxRows,
                        d_matrix
                    );
                }
            } else {
                if ( numOfRows <= 63 ) {
                    computeSpectrumCpuNoGrayShort(
                        numOfRows,
                        numOfCols,
                        wordsPerRow,
                        maxRows
                    );
                } else {
                    computeSpectrumCpuNoGrayLong(
                        numOfRows,
                        numOfCols,
                        wordsPerRow,
                        maxRows
                    );
                }
            }
        } break;
    }
    // После того, как произвели расчеты - сбрасываем RunState
    initializeRunState(LoadMode::Reset);
    if ( cancelled.load() ) {

        emit finished(Constants::ERROR_OCCURED);
        emit updateInfoPBR(0);

        updateSpectrum( numOfCols );
        if ( h_spectrum != nullptr ) {
            if ( settings.compDev == ComputationSettings::ComputeDevice::GPU ) {
                cudaFreeHost( h_spectrum );
            } else {
              free( h_spectrum );
            }
            h_spectrum = nullptr;
        }
        if ( settings.algorithmType == ComputationSettings::SimpleXor ) {
            freeBinomTable( h_binomTable, numOfRows );
            h_binomTable = nullptr;
        }
        if ( h_matrix != nullptr ) {
            free( h_matrix );
            h_matrix = nullptr;
        }
        if (d_matrix != nullptr) {
            cudaFree(d_matrix);
            d_matrix = nullptr;
        }
        // Раньше на пути отмены эти два буфера не освобождались: каждое
        // нажатие "Отмена" оставляло на видеокарте спектр и таблицу биномов.
        if (d_spectrum != nullptr) {
            cudaFree(d_spectrum);
            d_spectrum = nullptr;
        }
        if (d_binomTable != nullptr) {
            cudaFree(d_binomTable);
            d_binomTable = nullptr;
        }
        if (ev != nullptr) {
            CUDA_CALL(cudaEventDestroy(ev));
            ev = nullptr;
        }

        if (stream != nullptr) {
            CUDA_CALL(cudaStreamDestroy(stream));
            stream = nullptr;
        }
        return;
    }
    // Финальное обновление интерфейса
    if ( settings.compDev == ComputationSettings::ComputeDevice::GPU ) {
        cudaDeviceSynchronize();
        cudaMemcpy( h_spectrum, d_spectrum, (spectrumSize) * sizeof(quint64), cudaMemcpyDeviceToHost );
        if (d_spectrum != nullptr) {
            cudaFree(d_spectrum);
            d_spectrum = nullptr;
        }
        if (d_binomTable != nullptr) {
            cudaFree(d_binomTable);
            d_binomTable = nullptr;
        }
            
    }
    // Если применялся дуальный код - рассчитываем спектр из дуального
    if ( settings.algorithmType == ComputationSettings::Algorithm::DualCode ) {
        updateSpectrumDual( numOfCols, numOfRows );
    } else {
        updateSpectrum( numOfCols );
    }
    emit updateInfoPBR(100);
    emit finished(int(duration_cast<seconds>(steady_clock::now() - runStartedAt).count()));

    if ( h_spectrum != nullptr ) {
        if ( settings.compDev == ComputationSettings::ComputeDevice::GPU ) {
            CUDA_CALL( cudaFreeHost( h_spectrum ) );
        } else {
            free( h_spectrum );
        }
        h_spectrum = nullptr;
    }
    // При расчете простым XOR-ом строилась таблица биноминальных коэффициентов - очищаем её
    if ( settings.algorithmType == ComputationSettings::SimpleXor ) {
        freeBinomTable( h_binomTable, numOfRows );
    }
    if (h_matrix != nullptr) {
        free(h_matrix);
        h_matrix = nullptr;
    }
    
    if (d_matrix != nullptr) {
        cudaFree(d_matrix);
        d_matrix = nullptr;
    }

    if (ev != nullptr) {
        CUDA_CALL(cudaEventDestroy(ev));
        ev = nullptr;
    }
    if (stream != nullptr) {
        CUDA_CALL(cudaStreamDestroy(stream));
        stream = nullptr;
    }
        
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
        // Считаем хеш текущих настроек
        quint64 hash = settings.computeHash();
        // Хеш - имя чекпоинта
        QString group = QString("checkpoints/%1").arg(hash);

        QSettings s;
        s.beginGroup(group);

        // Проверяем, есть ли чекпоинт вообще
        if (!s.contains("runState")){
            s.endGroup();
            initializeRunState(LoadMode::Reset);
            return;
        }

        // --- Проверка settings ---
        QJsonObject savedSettingsObj = s.value("settings").toJsonObject();
        ComputationSettings saved = ComputationSettings::fromJson(savedSettingsObj);

        // Защита от коллизий. Если случилась - сбрасываем RunState
        if (!(saved == settings)) {
            s.endGroup();
            initializeRunState(LoadMode::Reset);
            return;
        }

        // Если всё ок, то загружаем RunState
        QJsonObject runObj = s.value("runState").toJsonObject();
        runState = RunState::fromJson(runObj);

        s.endGroup();
        // Ставим флаг, что надо выгрузить спектр
        exportSpectrum = true;
    }
}
