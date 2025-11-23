#include "worker.h"



Worker::Worker(QObject *parent)
    : QObject(parent)
{
    binomTable = nullptr;
}

Worker::~Worker()
{
}
void Worker::freeBinomTable(quint64** C, unsigned maxN) {
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
quint64** Worker::buildBinomTable(unsigned maxN) {
    quint64** C = new quint64 * [maxN + 1];
    for (unsigned n = 0; n <= maxN; ++n) {
        C[n] = new quint64[n + 1];
        for (unsigned k = 0; k <= n; ++k) {
            if (k == 0 || k == n) {
                C[n][k] = 1;
            }
            else {
                quint64 a = C[n - 1][k - 1];
                quint64 b = C[n - 1][k];
                quint64 sum = a + b;
                if (sum < a) sum = std::numeric_limits<quint64>::max(); // защита от переполнения
                C[n][k] = sum;
            }
        }
    }
    return C;
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
        launchSpectrumKernelGray(
            blockCount,
            threadsPerBlock,
            stream,
            d_spectrum,
            n,
            k,
            (int)wordsPerRow,
            offset,     
            thisChunkSize
        );

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
void Worker::computeSpectrumGpuNoGray( quint64 k, quint64 n, quint64 wordsPerRow, quint64 chunkSize, int blockCount, int threadsPerBlock, quint64 maxComb )
{   
    bool    copyPending = false;
    quint64 doneOps = 0;
    quint64 totalOps = sumCombinations(k,maxComb);
    for (int r = 0; r <= maxComb; r++)
            {
                quint64 curOps = binomTable[k][r];
                // Разбиваем на чанки
                for (quint64 offset = 0; offset < curOps; offset += chunkSize)
                {
                    quint64 thisChunkSize = qMin(chunkSize, curOps - offset); // последний чанк может быть меньше
                    while (paused.load() != 0) {
                        QThread::msleep(50);
                        if (cancelled.load()) break;
                    }
                    if (cancelled.load()) {
                        break;
                    }

                    // Запуск ядра
                    launchSpectrumKernel(
                        blockCount,             // Число блоков
                        threadsPerBlock,       // Число нитей на блок
                        stream,
                        d_spectrum,            // Копия спектра в глобальной памяти
                        n,                     // Длина строки матрицы в битах
                        k,                     // Число строк матрицы
                        wordsPerRow,            // Число слов на одну строку матрицы
                        offset,                // Смещение в комбинациях на каждом шаге
                        thisChunkSize,         // Размер чанка
                        r                      // Число единиц в битовой маске (число складываемых строк)
                    );
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
                        emit updateRemainingMinutes((int)elapsedSec, minutesLeft);

                    }
                    if (!copyPending && (now - lastTimeSpectrum > spectrumMs)) {
                        lastTimeSpectrum = now;
                        cudaMemcpyAsync(h_spectrum, d_spectrum, (n + 1) * sizeof(quint64), cudaMemcpyDeviceToHost, stream);
                        cudaEventRecord(ev, stream);
                        copyPending = true;
                    }
                    if ( copyPending ) {
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
                if (cancelled.load()) {
                    break;
                }
            }



    
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
            std::vector<quint64> localCodeword(wordsPerRow, 0);

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

                quint64 mask = generateBitMask(k, r, idx, binomTable);
                std::fill(localCodeword.begin(), localCodeword.end(), 0);

                for (quint64 i = 0; i < k; ++i) {
                    if (mask & (1ULL << i)) {
                        quint64* rowData = h_matrix + i * wordsPerRow;
                        for (quint64 b = 0; b < wordsPerRow; ++b) localCodeword[b] ^= rowData[b];
                    }
                }

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
    // Обработка матрицы
    if (rows.isEmpty()) {
        emit errorOccurred("Матрица пуста");
        emit finished(-1);
        return;
    }
    quint64 k = (quint64)rows.size();
    quint64 n = (quint64)rows.first().length();
    for (const QString& r : rows) {
        if ((quint64)r.length() != n) {
            emit errorOccurred("Все строки должны быть одинаковой длины");
            emit finished(-1);
            return;
        }
    }
    if (useDualCode) {
        rows = generatorToParity(rows);
        k = n - k;
    }
    int blockCount = 0;
    int threadsPerBlock = 0;
    quint64 chunkSize = 0;
    quint64 wordsPerRow = (n + 63) / 64;
    quint64 wordsNeeded = k * wordsPerRow;
    if (useGpu) {
        int deviceCount = 0;
        cudaError_t err = cudaGetDeviceCount(&deviceCount);
        if (err != cudaSuccess || deviceCount == 0) {
            useGpu = false;
            emit GPUnotFound();
        }
    }
    h_matrix = (quint64*)calloc(wordsNeeded, WORD_SIZE);
    if (!h_matrix) {
        emit errorOccurred("Ошибка выделения памяти");
        emit finished(-1);
        return;
    }
    for (quint64 i = 0; i < k; ++i) {
        quint64* rowData = h_matrix + i * wordsPerRow;
        const QString& row = rows[(int)i];
        for (quint64 j = 0; j < n; ++j) {
            if (row.at((int)j) == QLatin1Char('1')) {
                quint64 blockIdx = j / 64;
                quint64 bitIdx = j % 64;
                rowData[blockIdx] |= (1ull << bitIdx);
            }
        }
    }
    binomTable = buildBinomTable(MAX_K);
    if (useGpu) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, 0);
        blockCount = prop.multiProcessorCount;
        threadsPerBlock = prop.maxThreadsPerBlock;
        chunkSize = (quint64)threadsPerBlock * blockCount;
        chunkSize <<= 2;
        cudaStreamCreate(&stream);
        cudaEventCreateWithFlags(&ev, cudaEventDisableTiming);
        copyMatrixAndTableToSymbol(h_matrix, binomTable, wordsNeeded);
        cudaMallocHost((void**)&h_spectrum, (n + 1) * sizeof(quint64));
        cudaMalloc((void**)&d_spectrum, (n + 1) * sizeof(quint64));
        cudaMemset(d_spectrum, 0, (n + 1) * sizeof(quint64));

    }
    else {
        h_spectrum = (quint64*)calloc(n + 1, sizeof(quint64));
        if (!h_spectrum) {
            free(h_matrix);
            emit errorOccurred("Ошибка выделения памяти");
            emit finished(-1);
            return;
        }
    }
    startTime = steady_clock::now();
    lastTimeSpectrum = startTime;
    lastTimeBar = startTime;
    lastEstimateTime = startTime;

    Algorithm algorithm = chooseAlgorithm(useGpu, useGrayCode);
    switch (algorithm) {
    case Algorithm::GpuGray:
        computeSpectrumGpuGray(k, n, wordsPerRow, chunkSize, blockCount, threadsPerBlock);
        break;
    case Algorithm::GpuNoGray:
        computeSpectrumGpuNoGray(k, n, wordsPerRow, chunkSize, blockCount, threadsPerBlock, maxComb);
        break;
    case Algorithm::CpuGray:
        computeSpectrumCpuGray(k, n, wordsPerRow);
        break;
    case Algorithm::CpuNoGray:
        computeSpectrumCpuNoGray(k, n, wordsPerRow, maxComb);
        break;
    }
    if (cancelled.load()) {
        free(h_matrix);
        freeBinomTable(binomTable, MAX_K);
        emit finished(-1);
        emit updateInfoPBR(0);
        QVector<quint64> spectrumCopy(n + 1);
        #pragma omp critical
        {
            for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = h_spectrum[(quint64)w];
        }
        emit updateSpectrumPTE(spectrumCopy);
        emit updateSpectrumPlot(spectrumCopy);
        if (h_spectrum != nullptr) {
            if (useGpu) {
                cudaFreeHost(h_spectrum);
            }
            else {
                free(h_spectrum);
            }
            h_spectrum = nullptr;
        }
        return;
    }


    // Финальное обновление интерфейса и очистка памяти
    if (useGpu) {
        cudaDeviceSynchronize();
        cudaMemcpy(h_spectrum, d_spectrum, (n + 1) * sizeof(quint64), cudaMemcpyDeviceToHost);
        cudaFree(d_spectrum);
    }
    if (useDualCode) {
        k = n - k;
        //Если включен полный перебор
        if( k == maxComb )
            computeSpectrumFromDual(h_spectrum, n, k);
    }
    QVector<quint64> spectrumCopy(n + 1);
    for (quint64 w = 0; w <= n; ++w) spectrumCopy[w] = h_spectrum[w];

    emit updateSpectrumPTE(spectrumCopy);
    emit updateSpectrumPlot(spectrumCopy);
    emit updateInfoPBR(100);
    emit finished(duration_cast<seconds>(steady_clock::now() - startTime).count());

    if (h_spectrum != nullptr) {
        if (useGpu) {
            cudaFreeHost(h_spectrum);
        }
        else {
            free(h_spectrum);
        }
        h_spectrum = nullptr;
    }
    freeBinomTable(binomTable, MAX_K);
    free(h_matrix);
}
// Генерирует битовую маску с индексом idx длины k с r единицами
quint64 Worker::generateBitMask(unsigned k, unsigned r, quint64 idx, quint64** C)
{
    if (r == 0) return 0ULL;
    if (r > k) return 0ULL;
    quint64 mask = 0ULL;
    unsigned nextPos = 0;     // минимальная позиция, которую ещё можно занять
    quint64 rank = idx;

    for (unsigned i = r; i > 0; --i) {
        unsigned j = nextPos;
        while (j <= k - i) {
            quint64 c = C[k - j - 1][i - 1];
            if (c <= rank) {
                rank -= c;
                ++j;
            }
            else {
                break;
            }
        }
        // j — позиция следующей единицы
        mask |= (1ULL << j);
        nextPos = j + 1;
    }
    return mask;
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

