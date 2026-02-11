#include "worker.h"

Worker::Worker(QObject *parent)
    : QObject(parent)
{
    binomTable = nullptr;
}

Worker::~Worker()
{
}
struct SpectrumHostCallbackData {
    Worker* worker;                // указатель на объект Worker
    quint64* hostSpectrumPtr;      // pinned host buffer (куда был скопирован спектр)
    int numCols;
    size_t bytes;
};
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

    for (int i = numOfOnes; i < MAX_POSITIONS; ++i)
        slot[i] = 0;
}
// Функция строит треугольную таблицу биноминальных коэффициентов
quint64** Worker::buildBinomTable(unsigned maxN, unsigned maxComb) {
    // Выделяем память под столбцы длины maxN+1
    quint64** C = new quint64 * [maxN + 1];
    const quint64 U = std::numeric_limits<quint64>::max();

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

void CUDART_CB Worker::spectrumHostCallback(void* userData)
{
    SpectrumHostCallbackData* d = static_cast<SpectrumHostCallbackData*>(userData);
    if (!d) return;

    Worker* w = d->worker;
    quint64* hostPtr = d->hostSpectrumPtr;
    int numCols = d->numCols;
    size_t bytes = d->bytes;

    // Копируем в heap-allocated QVector (быстрый memcpy)
    QVector<quint64>* vec = new QVector<quint64>(numCols + 1);
    memcpy(vec->data(), hostPtr, bytes);

    // освобождаем данные callback'а (они были выделены в compute-функции)
    delete d;
    // Постим обработку в поток объекта Worker — безопасно для Qt/UI.
    QMetaObject::invokeMethod(w,
        [w, vec]() {
            // перенести в локальную и освободить временный в лямбде
            QVector<quint64> local = std::move(*vec);
            delete vec;
            emit w->updateSpectrumPTE(local);
            emit w->updateSpectrumPlot(local);
        },
        Qt::QueuedConnection);
}


Worker::Algorithm Worker::chooseAlgorithm(bool useGpu, bool useGray) const
{
    if (useGpu) {
        return useGray ? Algorithm::GpuGray : Algorithm::GpuNoGray;
    }
    else {
        return useGray ? Algorithm::CpuGray : Algorithm::CpuNoGray;
    }
}
void Worker::computeSpectrumGpuGray( quint64 k, quint64 n, quint64 wordsPerRow, quint64 chunkSize, int blockCount, int threadsPerBlock )
{
    bool    copyPending = false;
    quint64 doneOps = 0;
    quint64 totalOps = 1ULL << k;

    for (quint64 offset = 0; offset < totalOps; offset += chunkSize) {
        quint64 thisChunkSize = qMin(chunkSize, totalOps - offset);
        while (paused.load() != 0) {
            QThread::msleep(50);
            if (cancelled.load()) break;
        }
        if (cancelled.load()) {
            break;
        }
        //launchSpectrumKernelGray(
        //    blockCount,
        //    threadsPerBlock,
        //    stream,
        //    d_spectrum,
        //    n,
        //    k,
        //    (int)wordsPerRow,
        //    offset,     
        //    thisChunkSize
        //);

        doneOps += thisChunkSize;
        std::chrono::duration<quint64, std::milli> spectrumMs{ refreshSpectrumMs.load() };
        std::chrono::duration<quint64, std::milli> barMs{ refreshProgressbarMs.load() };
        auto now = steady_clock::now();
        if (now - lastEstimateTime >= std::chrono::seconds(1)) {
            lastEstimateTime = now;
            long long elapsedSec = duration_cast<seconds>(now - startTime).count();
            double speed = 1.0;

            if (doneOps != 0)
                speed = double(doneOps) / elapsedSec;

            quint64 remainingOps = totalOps > doneOps ? totalOps - doneOps : 0;
            double estSec = speed > 0 ? remainingOps / speed : 0.0;

            int minutesLeft = int(std::ceil(estSec / 60.0));
            emit updateRemainingMinutes((int)elapsedSec,minutesLeft);

        }
        if (!copyPending && (now - lastTimeSpectrum > spectrumMs)) {
            lastTimeSpectrum = now;
            cudaMemcpyAsync(h_spectrum, d_spectrum, (n + 1) * sizeof(quint64), cudaMemcpyDeviceToHost, stream);
            cudaEventRecord(ev, stream);
            copyPending = true;
        }
        if (copyPending) {
            cudaError_t q = cudaEventQuery(ev);
            if (q == cudaSuccess) {
                // копия точно завершена — читаем h_spectrum безопасно
                copyPending = false;

                QVector<quint64> spectrumCopy(n + 1);
                bool spectrumEmpty = true;
                for (quint64 w = 0; w <= n; ++w) {
                    spectrumCopy[w] = h_spectrum[w];
                    if (h_spectrum[w] != 0) spectrumEmpty = false;
                }
                if (!spectrumEmpty) {
                    emit updateSpectrumPTE(spectrumCopy);
                    emit updateSpectrumPlot(spectrumCopy);
                }
            }
        }
        if (now - lastTimeBar > barMs) {
            lastTimeBar = now;
            emit updateInfoPBR(doneOps * 100 / totalOps);
        }
    }
}

void Worker::computeSpectrumGpuNoGray(
    quint64 numOfRows,
    quint64 numOfCols,
    quint64 wordsPerRow,
    int blockCount,
    int threadsPerBlock,
    quint64 maxComb)
{
    quint64 totalOps = sumCombinations(numOfRows, maxComb);
    quint64 doneOps = 0;

    // Общее число нитей, запущенных на видеокарте
    uint64_t maxThreads = static_cast<uint64_t>(blockCount) * static_cast<uint64_t>(threadsPerBlock);
    if (maxThreads == 0) maxThreads = 1;

    // Максимальное число единиц в маске
    const size_t slotElems = MAX_POSITIONS;
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
    // Копируем матрицу в константую память
    CUDA_CALL(copyMatrixToConstant(h_matrix, numOfRows * wordsPerRow));

    #ifdef _DEBUG
    uint64_t* h_maskCounter = nullptr;
    uint64_t* d_maskCounter = nullptr;
    CUDA_CALL(cudaMallocHost( (void**)&h_maskCounter, sizeof(uint64_t) ) );
    CUDA_CALL(cudaMalloc(     (void**)&d_maskCounter, sizeof(uint64_t) ) );
    #endif
    cudaStream_t stream = 0;
    cudaEvent_t evCopy = nullptr;
    CUDA_CALL(cudaStreamCreate(&stream));
    CUDA_CALL(cudaEventCreate(&evCopy));

    QVector<quint64> spectrumCopy(numOfCols + 1);

    auto startTime = steady_clock::now();
    auto lastTimeSpectrum = startTime;
    auto lastTimeBar = startTime;
    auto lastEstimateTime = startTime;

    bool copyPending = false;

    // Размер чанка в масках
    const quint64 chunkSizeTarget = maxThreads * masksPerThread;

    // Внешний цикл по числу единиц в маске
    for (unsigned r = 0; r <= maxComb; ++r) {
        // Количество масок с r единицами
        quint64 curOps = binomTable[numOfRows][r];
        if (curOps == 0) continue;

        // Внутренний цикл по чанку
        for (quint64 chunkOffset = 0; chunkOffset < curOps; chunkOffset += chunkSizeTarget) {
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
                generateStartPositions(globalRank, numOfRows, r, h_slots + tid * MAX_POSITIONS, binomTable);
            }

            // Копируем только те стартовые маски, которые нужны в этом чанке
            CUDA_CALL(cudaMemcpyAsync(
                d_slots,
                h_slots,
                numStartMasks * slotBytes,
                cudaMemcpyHostToDevice,
                stream));

            // Запускаем ядро: numStartMasks потоков (упаковано в grid)
            int grid = (numStartMasks + threadsPerBlock - 1) / threadsPerBlock;
            if (grid <= 0) grid = 1;

            // Параметры: chunkSize (сколько масок в этом чанке всего),
            // masksPerThread (сколько масок на поток), numStartMasks (количество активных потоков)
            #ifdef _DEBUG
            cudaMemset(d_maskCounter, 0, sizeof(uint64_t));
            
            launchSpectrumKernel(
                grid,
                threadsPerBlock,
                stream,
                d_spectrum,
                nullptr,
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
            launchSpectrumKernel(
                grid,
                threadsPerBlock,
                stream,
                d_spectrum,
                nullptr,
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
            cudaDeviceSynchronize();
            #ifdef _DEBUG
            cudaMemcpy(h_maskCounter, d_maskCounter, sizeof(uint64_t), cudaMemcpyDeviceToHost);
            uint64_t m = *h_maskCounter;
            if (m != chunkSize) {
                throw "Error, masks mismath!";
            }
            #endif
            // Учёт прогресса
            doneOps += chunkSize;

            auto now = steady_clock::now();

            // Асинхронное копирование спектра для апдейта UI (по refreshSpectrumMs)
            if (!copyPending && (now - lastTimeSpectrum > std::chrono::milliseconds(refreshSpectrumMs.load()))) {
                lastTimeSpectrum = now;
                CUDA_CALL(cudaMemcpyAsync(
                    h_spectrum,
                    d_spectrum,
                    (numOfCols + 1) * sizeof(quint64),
                    cudaMemcpyDeviceToHost,
                    stream));
                CUDA_CALL(cudaEventRecord(evCopy, stream));
                copyPending = true;
            }
            
            // Проверяем завершение копии
            if (copyPending) {
                cudaError_t evq = cudaEventQuery(evCopy);
                if (evq == cudaSuccess) {
                    copyPending = false;
                    bool empty = true;
                    for (quint64 i = 0; i <= numOfCols; ++i) {
                        spectrumCopy[static_cast<int>(i)] = h_spectrum[i];
                        if (h_spectrum[i] != 0) empty = false;
                    }
                    if (!empty) {
                        emit updateSpectrumPTE(spectrumCopy);
                        emit updateSpectrumPlot(spectrumCopy);
                    }
                }
            }

            // Обновляем progress bar по таймеру
            if (now - lastTimeBar > std::chrono::milliseconds(refreshProgressbarMs.load())) {
                lastTimeBar = now;
                emit updateInfoPBR(doneOps * 100 / totalOps);
            }
            if (now - lastEstimateTime >= std::chrono::seconds(1)) {
                lastEstimateTime = now;
                long long elapsedSec = duration_cast<seconds>(now - startTime).count();
                double speed = 1.0;

                if (doneOps != 0)
                    speed = double(doneOps) / elapsedSec;

                quint64 remainingOps = totalOps > doneOps ? totalOps - doneOps : 0;
                double estSec = speed > 0 ? remainingOps / speed : 0.0;

                int minutesLeft = int(std::ceil(estSec / 60.0));
                emit updateRemainingMinutes((int)elapsedSec, minutesLeft);

            }

            if (cancelled.load()) break;
        } // chunkOffset
        if (cancelled.load()) break;
    } // for r

    // Финальная синхронная копия спектра
    CUDA_CALL(cudaMemcpyAsync(
        h_spectrum,
        d_spectrum,
        (numOfCols + 1) * sizeof(quint64),
        cudaMemcpyDeviceToHost,
        stream));
    CUDA_CALL(cudaEventRecord(evCopy, stream));
    CUDA_CALL(cudaStreamSynchronize(stream));

    bool empty = true;
    for (quint64 i = 0; i <= numOfCols; ++i) {
        spectrumCopy[static_cast<int>(i)] = h_spectrum[i];
        if (h_spectrum[i] != 0) empty = false;
    }
    if (!empty) {
        emit updateSpectrumPTE(spectrumCopy);
        emit updateSpectrumPlot(spectrumCopy);
    }

    // Очистка
    cudaFree(d_slots);
    cudaFreeHost(h_slots);
    //cudaFree(d_maskCounter);
    cudaStreamDestroy(stream);
    cudaEventDestroy(evCopy);
}

void Worker::computeSpectrumCpuGray( quint64 k, quint64 n, quint64 wordsPerRow )
{
    quint64 doneOps  = 0;
    quint64 totalOps = 1ULL << k;
    #pragma omp parallel
    {
        int tid      = omp_get_thread_num();
        int nthreads = omp_get_num_threads();

        quint64 combosPerThread = (totalOps + nthreads - 1) / nthreads;
        quint64 startIdx = (quint64)tid * combosPerThread;
        quint64 endIdx = startIdx + combosPerThread;
        if (endIdx > totalOps) endIdx = totalOps;
        if (startIdx >= endIdx) {
            // нет работы для этого потока
        }
        else {
            // локальный codeword в виде вектора слов
            std::vector<quint64> localCodeword(wordsPerRow, 0);

            // получить Gray-маску для startIdx
            auto gray = [](quint64 i)->quint64 { return (i ^ (i >> 1)); };

            // формируем codeword для начальной маски
            quint64 g = gray(startIdx);

            // полный XOR по установленным битам в g
            quint64 tmp = g;
            while (tmp) {
                quint64 single = tmp & (~tmp + 1ULL);
                unsigned long pos;
                single == 0ULL ? pos = 64UL : _BitScanForward64(&pos, single);
                tmp &= (tmp - 1);
                quint64* rowData = h_matrix + (quint64)pos * wordsPerRow;
                for (quint64 b = 0; b < wordsPerRow; ++b) localCodeword[b] ^= rowData[b];
            }

            // если начальная маска удовлетворяет по попаунту (в этом варианте maxComb == k, значит всегда),
            // учитываем в спектр
            if (__popcnt64(g) <= (unsigned)k) {
                // считаем вес
                quint64 weight = 0;
                for (quint64 b = 0; b < wordsPerRow; ++b) weight += __popcnt64(localCodeword[b]);
                #pragma omp atomic
                ++h_spectrum[weight];
                #pragma omp atomic
                ++doneOps;
            }

            // Теперь идём по последовательности i = startIdx+1 .. endIdx-1
            for (quint64 i = startIdx + 1; i < endIdx; ++i) {
                if (cancelled.load()) break;
                while (paused.load() != 0) {
                    QThread::msleep(50);
                    if (cancelled.load()) break;
                }
                if (cancelled.load()) break;

                quint64 g_next = gray(i);
                quint64 diff = g ^ g_next;
                unsigned long pos;
                diff == 0ULL ? pos = 64 : _BitScanForward64(&pos, diff);
                // xor'им соответствующую строку (toggle)
                quint64* rowData = h_matrix + (quint64)pos * wordsPerRow;
                for (quint64 b = 0; b < wordsPerRow; ++b) localCodeword[b] ^= rowData[b];

                // теперь g = g_next
                g = g_next;

                // если popcount(g) <= maxComb => учитываем (в вашем случае maxComb==k, всегда true)
                if (__popcnt64(g) <= (unsigned)k) {
                    quint64 weight = 0;
                    for (quint64 b = 0; b < wordsPerRow; ++b) weight += __popcnt64(localCodeword[b]);
                    #pragma omp atomic
                    ++h_spectrum[weight];
                    #pragma omp atomic
                    ++doneOps;
                }

                // Только один поток обновляет GUI/таймеры/события, как раньше (thread 0)
                if (omp_get_thread_num() == 0) {
                    std::chrono::duration<quint64, std::milli> spectrumMs{ refreshSpectrumMs.load() };
                    std::chrono::duration<quint64, std::milli> barMs{ refreshProgressbarMs.load() };
                    auto now = steady_clock::now();

                    if (now - lastTimeSpectrum >= spectrumMs) {
                        lastTimeSpectrum = now;
                        QVector<quint64> spectrumCopy(n + 1);
                        #pragma omp critical
                        {
                            for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = h_spectrum[(quint64)w];
                        }
                        emit updateSpectrumPTE(spectrumCopy);
                        emit updateSpectrumPlot(spectrumCopy);
                    }
                    if (now - lastTimeBar >= barMs) {
                        lastTimeBar = now;
                        int percent = int(100.0 * double(doneOps) / double(totalOps));
                        emit updateInfoPBR(percent);
                    }
                    // estimate
                    if (now - lastEstimateTime >= std::chrono::seconds(1)) {
                        lastEstimateTime = now;
                        long long elapsedSec = duration_cast<seconds>(now - startTime).count();
                        double speed = 1.0;
                        if (doneOps != 0) speed = double(doneOps) / elapsedSec;
                        quint64 remainingOps = totalOps > doneOps ? totalOps - doneOps : 0;
                        double estSec = speed > 0 ? remainingOps / speed : 0.0;
                        int minutesLeft = int(std::ceil(estSec / 60.0));
                        emit updateRemainingMinutes((int)elapsedSec,minutesLeft);
                    }
                } // thread 0 GUI updates
            } // for i in thread range
        } // else start<end
    } // #pragma omp parallel


}
void Worker::computeSpectrumCpuNoGray( quint64 k, quint64 n, quint64 wordsPerRow, quint64 maxComb )
{
    quint64 doneOps = 0;
    quint64 totalOps = sumCombinations(k,maxComb);

    for (unsigned r = 0; r <= maxComb; ++r) {
        if (cancelled.load()) break;

        quint64 combCount = binomTable[k][r];
        if (combCount == 0) continue;
        #pragma omp parallel
        {
            // Локальное слово, которое будем XOR-ить
            std::vector<quint64> localCodeword(wordsPerRow, 0);

            BitMask bmask(k, r);

            #pragma omp for schedule(dynamic)
            for (qint64 idx = 0; idx < combCount; ++idx) {
                if (cancelled.load()) {
                    continue;
                }

                while (paused.load() != 0) {
                    QThread::msleep(50);
                    if (cancelled.load()) break;
                }
                if (cancelled.load()) {
                    continue;
                }
                // Устанавливаем начальное значение для маски
                bmask.setMask(idx, binomTable, maxComb);
                std::fill(localCodeword.begin(), localCodeword.end(), 0);
                bmask.forEachSetBit([&](unsigned pos) {
                    quint64* rowData = h_matrix + (quint64)pos * wordsPerRow;
                    for (quint64 b = 0; b < wordsPerRow; ++b) localCodeword[b] ^= rowData[b];
                    });

                quint64 weight = 0;
                for (quint64 b = 0; b < wordsPerRow; ++b)
                    weight += __popcnt64(localCodeword[b]);

                #pragma omp atomic
                ++h_spectrum[weight];

                #pragma omp atomic
                ++doneOps;

                if (omp_get_thread_num() == 0) {
                    std::chrono::duration<quint64, std::milli> spectrumMs{ refreshSpectrumMs.load() };
                    std::chrono::duration<quint64, std::milli> barMs{ refreshProgressbarMs.load() };

                    auto now = steady_clock::now();
                    if (now - lastTimeSpectrum >= spectrumMs) {
                        lastTimeSpectrum = now;
                        QVector<quint64> spectrumCopy(n + 1);
                        #pragma omp critical
                        {
                            for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = h_spectrum[(quint64)w];
                        }
                        emit updateSpectrumPTE(spectrumCopy);
                        emit updateSpectrumPlot(spectrumCopy);
                    }
                    if (now - lastTimeBar >= barMs) {
                        lastTimeBar = now;
                        int percent = int(100.0 * double(doneOps) / double(totalOps));
                        emit updateInfoPBR(percent);
                    }
                    if (now - lastEstimateTime >= std::chrono::seconds(1)) {
                        lastEstimateTime = now;
                        long long elapsedSec = duration_cast<seconds>(now - startTime).count();
                        double speed = 1.0;

                        if (doneOps != 0)
                            speed = double(doneOps) / elapsedSec;

                        quint64 remainingOps = totalOps > doneOps ? totalOps - doneOps : 0;
                        double estSec = speed > 0 ? remainingOps / speed : 0.0;

                        int minutesLeft = int(std::ceil(estSec / 60.0));
                        emit updateRemainingMinutes((int) elapsedSec, minutesLeft);

                    }

                } // if (omp_get_thread_num() == 0)

            } // for (quint64 idx = 0; idx < combCount; ++idx)

        } // #pragma omp parallel

    } // for (unsigned r = 1; r <= maxComb; ++r)
}
void Worker::computeSpectrum(QStringList rows, quint64 maxComb )
{
    int maxThreads = omp_get_max_threads();
    int workerThreads = std::max(1, maxThreads - 1);
    omp_set_num_threads(workerThreads);
    quint64 numOfRows = rows.length();
    quint64 numOfCols = rows[0].length();
    //if (useDualCode) {
    //    rows = generatorToParity(rows);
    //    k = n - k;
    //}
    // Число блоков для запуска на видеокарте ( минимум - число мультипроцессоров )
    int blockCount = 40;
    // Число нитей, запускаемых на одном блоке
    int threadsPerBlock = 512;
    // Число 64-битных слов на одну строку матрицы
    quint64 wordsPerRow = (numOfCols + 63) / 64;
    // Размер матрицы в 64-битных словах
    quint64 matrixSizeInWords = numOfRows * wordsPerRow;
    //if (useGpu) {
    //    int deviceCount = 0;
    //    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    //    if (err != cudaSuccess || deviceCount == 0) {
    //        useGpu = false;
    //        emit GPUnotFound();
    //    }
    //}
    h_matrix = (quint64*)calloc( matrixSizeInWords, WORD_SIZE );
    if (!h_matrix) {
        emit errorOccurred("Ошибка выделения памяти");
        emit finished(-1);
        return;
    }
    for (quint64 i = 0; i < numOfRows; ++i) {
        quint64* rowData = h_matrix + i * wordsPerRow;
        const QString& row = rows[(int)i];
        for (quint64 j = 0; j < numOfCols; ++j) {
            if (row.at((int)j) == QLatin1Char('1')) {
                quint64 blockIdx = j / 64;
                quint64 bitIdx = j % 64;
                rowData[blockIdx] |= (1ull << bitIdx);
            }
        }
    }
    binomTable = buildBinomTable( numOfRows, maxComb );
    //if (useGpu) {
    //    cudaDeviceProp prop;
    //    cudaGetDeviceProperties(&prop, 0);
    //    blockCount = prop.multiProcessorCount;
    //    threadsPerBlock = prop.maxThreadsPerBlock;
    //    chunkSize = (quint64)threadsPerBlock * blockCount;
    //    chunkSize <<= 2;
    //    cudaStreamCreate(&stream);
    //    cudaEventCreateWithFlags(&ev, cudaEventDisableTiming);
    CUDA_CALL(copyMatrixToConstant( h_matrix, matrixSizeInWords ));
    CUDA_CALL(cudaMallocHost((void**)&h_spectrum, (numOfCols + 1) * sizeof(quint64)));
    CUDA_CALL(cudaMalloc((void**)&d_spectrum, (numOfCols + 1) * sizeof(quint64)));
    //    cudaMemset(d_spectrum, 0, (n + 1) * sizeof(quint64));
    //
    //}
    //else {
    //h_spectrum = (quint64*)calloc(numOfCols + 1, sizeof(quint64));
    //    if (!h_spectrum) {
    //        free(h_matrix);
    //        emit errorOccurred("Ошибка выделения памяти");
    //        emit finished(-1);
    //        return;
    //    }
    //}
    startTime = steady_clock::now();
    lastTimeSpectrum = startTime;
    lastTimeBar = startTime;
    lastEstimateTime = startTime;

    //Algorithm algorithm = chooseAlgorithm(useGpu, useGrayCode);
    //switch (algorithm) {
    //case Algorithm::GpuGray:
    //    computeSpectrumGpuGray(k, n, wordsPerRow, chunkSize, blockCount, threadsPerBlock);
    //    break;
    //case Algorithm::GpuNoGray:
    //    computeSpectrumGpuNoGray(k, n, wordsPerRow, chunkSize, blockCount, threadsPerBlock, maxComb);
    //    break;
    //case Algorithm::CpuGray:
    //    computeSpectrumCpuGray(k, n, wordsPerRow);
    //    break;
    //case Algorithm::CpuNoGray:
    computeSpectrumGpuNoGray(
        numOfRows,
        numOfCols,
        wordsPerRow,
        blockCount,
        threadsPerBlock,
        maxComb);
       // computeSpectrumCpuNoGray(k, n, wordsPerRow, maxComb);
    //   break;
    //}
    if (cancelled.load()) {
        free(h_matrix);
        freeBinomTable( binomTable, numOfRows );
        emit finished(-1);
        emit updateInfoPBR(0);
        QVector<quint64> spectrumCopy(numOfCols + 1);
        #pragma omp critical
        {
            for (quint64 w = 0; w <= numOfCols; ++w) spectrumCopy[(int)w] = h_spectrum[(quint64)w];
        }
        emit updateSpectrumPTE(spectrumCopy);
        emit updateSpectrumPlot(spectrumCopy);
        if (h_spectrum != nullptr) {
            //if (useGpu) {
            //    cudaFreeHost(h_spectrum);
            //}
            //else {
                free(h_spectrum);
            //}
            h_spectrum = nullptr;
        }
        return;
    }


    // Финальное обновление интерфейса и очистка памяти
    //if (useGpu) {
    //    cudaDeviceSynchronize();
    //    cudaMemcpy(h_spectrum, d_spectrum, (n + 1) * sizeof(quint64), cudaMemcpyDeviceToHost);
    //    cudaFree(d_spectrum);
    //}
    //if (useDualCode) {
        //k = n - k;
        //computeSpectrumFromDual(h_spectrum, n, k);
    //}
    emit updateInfoPBR(100);
    emit finished(duration_cast<seconds>(steady_clock::now() - startTime).count());

    if (h_spectrum != nullptr) {
        //if (useGpu) {
            CUDA_CALL(cudaFreeHost(h_spectrum));
        //}
        //else {
            //free(h_spectrum);
        //}
        h_spectrum = nullptr;
    }
    freeBinomTable( binomTable, numOfRows );
    free(h_matrix);
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

void Worker::handleRefreshSpectrumValueChanged(int v)
{
    refreshSpectrumMs = v;
}

void Worker::handleRefreshProgressbarValueChanged(int v)
{
    refreshProgressbarMs = v;
}

void Worker::handleUseGpuToggled(bool b)
{
    useGpu = b;
}

void Worker::handleUseGrayCodeToggled(bool b)
{
    useGrayCode = b;
}

void Worker::handleUseDualCodeToggled(bool b)
{
    useDualCode = b;
}

void Worker::setInitialSettings(const QJsonObject& settings)
{
    useGpu                  = settings.value(USE_GPU_CHECKED).toBool();
    useGrayCode             = settings.value(USE_GRAY_CODE_CHECKED).toBool();
    useDualCode             = settings.value(USE_DUAL_CODE_CHECKED).toBool();
    refreshSpectrumMs       = settings.value(SPECTRUM_MS).toInt();
    refreshProgressbarMs    = settings.value(PROGRESSBAR_MS).toInt();
}

