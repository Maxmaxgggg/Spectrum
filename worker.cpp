#include "worker.h"
#include <QMetaObject>
#include <QThread>
#include <QDateTime>
#include <QDebug>
#include <cmath>
#include <cuda_runtime.h>
#include "defines.h"
#include "computeSpectrumKernel.cuh"
#include <iostream>
using namespace std::chrono;


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
    this->USE_GPU = b;
}

void Worker::handleUseGrayCodeToggled(bool b)
{
    this->USE_GRAY_CODE = b;
}

void Worker::setInitialSettings(bool useGpu, bool useGrayCode, int spectrumMs, int progressbarMs)
{
    USE_GPU              = useGpu;
    USE_GRAY_CODE        = useGrayCode;
    refreshSpectrumMs    = spectrumMs;
    refreshProgressbarMs = progressbarMs;

}

void Worker::computeSpectrum( QStringList rows, quint64 maxComb )
{
    int maxThreads = omp_get_max_threads();
    int workerThreads = std::max(1, maxThreads - 1);
    omp_set_num_threads(workerThreads);
    // Обработка матрицы
    if (rows.isEmpty()) {
        emit errorOccurred("Матрица пуста");
        emit finished();
        return;
    }
    quint64 k = (quint64)rows.size();
    quint64 n = (quint64)rows.first().length();
    for (const QString& r : rows) {
        if ((quint64)r.length() != n) {
            emit errorOccurred("Все строки должны быть одинаковой длины");
            emit finished();
            return;
        }
    }
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if ( USE_GPU && ( err != cudaSuccess || count == 0 ) ) {
        USE_GPU = false;
        emit GPUnotFound();
    }
    if (USE_GPU) {

        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, 0);
        //int mem = prop.totalConstMem;
        //// Максимальное число блоков, ограниченное количеством разделяемой памяти
        //int maxBlocksByShared = prop.multiProcessorCount * prop.sharedMemPerMultiprocessor /
        //    ((n + 1) * sizeof(quint64));

        int maxBlocks       = prop.multiProcessorCount;
        int threadsPerBlock = prop.maxThreadsPerBlock;
      
        // Разделяем расчет на чанки
        quint64 chunkSize = (quint64) threadsPerBlock * maxBlocks;
        chunkSize <<= 2;
        quint64 blockCount = (n + 63) / 64;
        quint64 wordsNeeded = k * blockCount;

        quint64* h_matrix = (quint64*)calloc( wordsNeeded, WORD_SIZE );
        if (!h_matrix) {
            emit errorOccurred("Ошибка выделения памяти");
            emit finished();
            return;
        }
        for (quint64 i = 0; i < k; ++i) {
            quint64* rowData = h_matrix + i * blockCount;
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
        quint64 totalOps = sumCombinations(k, maxComb);
        quint64 doneOps  = 0;
        copyMatrixAndTableToSymbol(h_matrix, binomTable, wordsNeeded);

        quint64* h_spectrum = nullptr;
        quint64* d_spectrum = nullptr;
        cudaMallocHost((void**) & h_spectrum, (n + 1) * sizeof(quint64));
        cudaMalloc((void**) &d_spectrum, (n + 1) * sizeof(quint64));
        cudaMemset(d_spectrum, 0, (n + 1) * sizeof(quint64));
        cudaStream_t stream;
        cudaStreamCreate(&stream);



        bool copyPending = false;
        cudaEvent_t ev;
        cudaEventCreateWithFlags(&ev, cudaEventDisableTiming);
        

        auto startTime = steady_clock::now();
        auto lastTimeSpectrum = startTime;
        auto lastTimeBar = startTime;
        auto lastEstimateTime = startTime;

        QSettings s;
        if (USE_GRAY_CODE) {
                const quint64 totalComb = 1ULL << k;
                for (quint64 offset = 0; offset < totalComb; offset += chunkSize) {
                    quint64 thisChunkSize = qMin(chunkSize, totalComb - offset);
                    while (paused.load() != 0) {
                        QThread::msleep(50);
                        if (cancelled.load()) break;
                    }
                    if (cancelled.load()) {
                        break;
                    }
                    launchSpectrumKernelGray(
                        maxBlocks,
                        threadsPerBlock,
                        stream,
                        d_spectrum,
                        n,
                        k,
                        (int)blockCount,
                        offset,            // chunkOffset interpreted as Gray-index
                        thisChunkSize
                    );

                    doneOps += thisChunkSize;
                    std::chrono::duration<quint64, std::milli> spectrumMs{ refreshSpectrumMs.load() };
                    std::chrono::duration<quint64, std::milli> barMs{ refreshProgressbarMs.load() };
                    auto now = steady_clock::now();
                    if (now - lastEstimateTime >= std::chrono::seconds(10)) {
                        lastEstimateTime = now;
                        double elapsedSec = duration_cast<seconds>(now - startTime).count();
                        double speed = 1.0;

                        if (doneOps != 0)
                            speed = double(doneOps) / elapsedSec;

                        quint64 remainingOps = totalOps > doneOps ? totalOps - doneOps : 0;
                        double estSec = speed > 0 ? remainingOps / speed : 0.0;

                        int minutesLeft = int(std::ceil(estSec / 60.0));
                        emit updateRemainingMinutes(minutesLeft);

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
                
                
        } else {
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
                        maxBlocks,             // Число блоков
                        threadsPerBlock,       // Число нитей на блок
                        stream,
                        d_spectrum,            // Копия спектра в глобальной памяти
                        n,                     // Длина строки матрицы в битах
                        k,                     // Число строк матрицы
                        blockCount,            // Число слов на одну строку матрицы
                        offset,                // Смещение в комбинациях на каждом шаге
                        thisChunkSize,         // Размер чанка
                        r                      // Число единиц в битовой маске (число складываемых строк)
                    );
                    doneOps += thisChunkSize;
                    std::chrono::duration<quint64, std::milli> spectrumMs{ refreshSpectrumMs.load() };
                    std::chrono::duration<quint64, std::milli> barMs{ refreshProgressbarMs.load() };
                    auto now = steady_clock::now();
                    if (now - lastEstimateTime >= std::chrono::seconds(10)) {
                        lastEstimateTime = now;
                        double elapsedSec = duration_cast<seconds>(now - startTime).count();
                        double speed = 1.0;

                        if (doneOps != 0)
                            speed = double(doneOps) / elapsedSec;

                        quint64 remainingOps = totalOps > doneOps ? totalOps - doneOps : 0;
                        double estSec = speed > 0 ? remainingOps / speed : 0.0;

                        int minutesLeft = int(std::ceil(estSec / 60.0));
                        emit updateRemainingMinutes(minutesLeft);

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
        if (cancelled.load()) {
            emit updateInfoPBR(0);
            emit finished();
        }
        emit updateInfoPBR(100);
        cudaDeviceSynchronize();
        cudaMemcpy(h_spectrum, d_spectrum, (n + 1) * sizeof(quint64), cudaMemcpyDeviceToHost);
        QVector<quint64> spectrumCopy(n + 1);
        for (quint64 w = 0; w <= n; ++w) spectrumCopy[w] = h_spectrum[w];
        emit updateSpectrumPTE(spectrumCopy);
        emit updateSpectrumPlot(spectrumCopy);
        emit finished();
        freeBinomTable(binomTable, MAX_K);

        cudaFree(d_spectrum);
        cudaFreeHost(h_spectrum);
    } else {

        quint64* spectrum = (quint64*)calloc(n + 1, sizeof(quint64));
        if (!spectrum) {
            emit errorOccurred("Ошибка выделения памяти");
            emit finished();
            return;
        }
        quint64 blockCount = (n + 63) / 64;
        quint64 wordsNeeded = k * blockCount;
        quint64* packed = (quint64*)calloc( wordsNeeded, WORD_SIZE );
        if (!packed) {
            emit errorOccurred("Ошибка выделения памяти");
            emit finished();
            free(spectrum);
            return;
        }

        for (quint64 i = 0; i < k; ++i) {
            quint64* rowData = packed + i * blockCount;
            const QString& row = rows[(int)i];
            for (quint64 j = 0; j < n; ++j) {
                if (row.at((int)j) == QLatin1Char('1')) {
                    quint64 blockIdx = j / 64;
                    quint64 bitIdx = j % 64;
                    rowData[blockIdx] |= (1ull << bitIdx);
                }
            }
        }

        quint64 totalOps = sumCombinations(k, maxComb);
        quint64 doneOps = 0;
        if (totalOps == 0) {
            emit finished();
            free(packed);
            free(spectrum);
            return;
        }

        QSettings s;


        refreshSpectrumMs    = s.value( SPECTRUM_MS    ).toULongLong();
        refreshProgressbarMs = s.value( PROGRESSBAR_MS ).toULongLong();

        auto startTime         = steady_clock::now();
        auto lastTimeSpectrum  = startTime;
        auto lastTimeBar       = startTime;
        auto lastEstimateTime  = startTime;

        binomTable = buildBinomTable(MAX_K);
        if (USE_GRAY_CODE) {
            // Gray-code path: iterate over all 2^k subsets in Gray order.
            if (k >= 64) {
                emit errorOccurred("Gray-path поддерживается только для k <= 63");
                return;
            }
            else {
                const quint64 totalComb = 1ULL << k; // 2^k
                // разделение работы между потоками OpenMP
                #pragma omp parallel
                {
                    int tid = omp_get_thread_num();
                    int nthreads = omp_get_num_threads();

                    quint64 combosPerThread = (totalComb + nthreads - 1) / nthreads;
                    quint64 startIdx = (quint64)tid * combosPerThread;
                    quint64 endIdx = startIdx + combosPerThread;
                    if (endIdx > totalComb) endIdx = totalComb;
                    if (startIdx >= endIdx) {
                        // нет работы для этого потока
                    }
                    else {
                        // локальный codeword в виде вектора слов
                        std::vector<quint64> localCodeword(blockCount, 0);

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
                            quint64* rowData = packed + (quint64)pos * blockCount;
                            for (quint64 b = 0; b < blockCount; ++b) localCodeword[b] ^= rowData[b];
                        }

                        // если начальная маска удовлетворяет по попаунту (в этом варианте maxComb == k, значит всегда),
                        // учитываем в спектр
                        if (__popcnt64(g) <= (unsigned)maxComb) {
                            // считаем вес
                            quint64 weight = 0;
                            for (quint64 b = 0; b < blockCount; ++b) weight += __popcnt64(localCodeword[b]);
                        #pragma omp atomic
                            ++spectrum[weight];
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
                            quint64* rowData = packed + (quint64)pos * blockCount;
                            for (quint64 b = 0; b < blockCount; ++b) localCodeword[b] ^= rowData[b];

                            // теперь g = g_next
                            g = g_next;

                            // если popcount(g) <= maxComb => учитываем (в вашем случае maxComb==k, всегда true)
                            if (__popcnt64(g) <= (unsigned)maxComb) {
                                quint64 weight = 0;
                                for (quint64 b = 0; b < blockCount; ++b) weight += __popcnt64(localCodeword[b]);
                                #pragma omp atomic
                                ++spectrum[weight];
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
                                        for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = spectrum[(quint64)w];
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
                                if (now - lastEstimateTime >= std::chrono::seconds(10)) {
                                    lastEstimateTime = now;
                                    double elapsedSec = duration_cast<seconds>(now - startTime).count();
                                    double speed = 1.0;
                                    if (doneOps != 0) speed = double(doneOps) / elapsedSec;
                                    quint64 remainingOps = totalOps > doneOps ? totalOps - doneOps : 0;
                                    double estSec = speed > 0 ? remainingOps / speed : 0.0;
                                    int minutesLeft = int(std::ceil(estSec / 60.0));
                                    emit updateRemainingMinutes(minutesLeft);
                                }
                            } // thread 0 GUI updates
                        } // for i in thread range
                    } // else start<end
                } // #pragma omp parallel
            } // else k >= 64
        } else {
            for (unsigned r = 0; r <= maxComb; ++r) {
                if (cancelled.load()) break;

                quint64 combCount = binomTable[k][r];
                if (combCount == 0) continue;
                #pragma omp parallel
                {
                    std::vector<quint64> localCodeword(blockCount, 0);

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
                                quint64* rowData = packed + i * blockCount;
                                for (quint64 b = 0; b < blockCount; ++b) localCodeword[b] ^= rowData[b];
                            }
                        }

                        quint64 weight = 0;
                        for (quint64 b = 0; b < blockCount; ++b)

                            weight += __popcnt64(localCodeword[b]);

                        #pragma omp atomic
                        ++spectrum[weight];

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
                                    for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = spectrum[(quint64)w];
                                }
                                emit updateSpectrumPTE(spectrumCopy);
                                emit updateSpectrumPlot(spectrumCopy);
                            }
                            if (now - lastTimeBar >= barMs) {
                                lastTimeBar = now;
                                int percent = int(100.0 * double(doneOps) / double(totalOps));
                                emit updateInfoPBR(percent);
                            }
                            if (now - lastEstimateTime >= std::chrono::seconds(10)) {
                                lastEstimateTime = now;
                                double elapsedSec = duration_cast<seconds>(now - startTime).count();
                                double speed = 1.0;

                                if (doneOps != 0)
                                    speed = double(doneOps) / elapsedSec;

                                quint64 remainingOps = totalOps > doneOps ? totalOps - doneOps : 0;
                                double estSec = speed > 0 ? remainingOps / speed : 0.0;

                                int minutesLeft = int(std::ceil(estSec / 60.0));
                                emit updateRemainingMinutes(minutesLeft);

                            }

                        } // if (omp_get_thread_num() == 0)

                    } // for (quint64 idx = 0; idx < combCount; ++idx)

                } // #pragma omp parallel

            } // for (unsigned r = 1; r <= maxComb; ++r)

        } // if( !USE_GRAY_CODE )
        

        if (cancelled.load()) {
            free( packed );
            freeBinomTable( binomTable, MAX_K );
            emit finished();
            emit updateInfoPBR(0);
            QVector<quint64> spectrumCopy( n + 1 );
            #pragma omp critical
            {
                for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = spectrum[(quint64)w];
            }
            emit updateSpectrumPTE(  spectrumCopy );
            emit updateSpectrumPlot( spectrumCopy );
            free(spectrum);
            return;
        }

        QVector<quint64> spectrumCopy(n + 1);
        {
            for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = spectrum[(quint64)w];
        }
        emit finished();
        emit updateInfoPBR(100);
        emit updateSpectrumPTE(  spectrumCopy );
        emit updateSpectrumPlot( spectrumCopy );
        freeBinomTable(binomTable, MAX_K);
        free(spectrum);
        free(packed);
    } //if (!USE_GPU)
}
