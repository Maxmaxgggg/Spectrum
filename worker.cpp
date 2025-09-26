#include "worker.h"
#include <QMetaObject>
#include <QThread>
#include <QDateTime>
#include <QDebug>
#include <cmath>




using namespace std::chrono;


Worker::Worker(QObject *parent)
    : QObject(parent)
{
}

Worker::~Worker()
{
}

// Вычисляет число сочетаний C(n,k)
quint64 Worker::combin(unsigned n, unsigned k)
{
    if (k > n) return 0;
    if (k == 0 || k == n) return 1;
    if (k > n/2) k = n - k;
    quint64 res = 1;
    for (quint64 i = 1; i <= k; ++i) {
        res = res * (n - k + i) / i;
    }
    return res;
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

// Генерирует битовую маску с индексом idx длины k с r единицами
quint64 Worker::generateBitMask(unsigned k, unsigned r, quint64 idx) {
    quint64 mask = 0;
    unsigned remaining = r;
    unsigned start = 0;
    while (remaining > 0) {
        for (unsigned j = start; j <= k - remaining; ++j) {
            quint64 c = combin(k - j - 1, remaining - 1);
            if (idx < c) {
                mask |= (1ULL << j);
                start = j + 1;
                --remaining;
                break;
            } else {
                idx -= c;
            }
        }
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

void Worker::computeSpectrum( QStringList rows, quint64 maxComb )
{
    if (rows.isEmpty()) {
        emit errorOccurred("Матрица пуста");
        emit finished();
        return;
    }
    quint64 k = (quint64)rows.size();
    quint64 n = (quint64)rows.first().length();
    for (const QString &r : rows) {
        if ((quint64)r.length() != n) {
            emit errorOccurred("Все строки должны быть одинаковой длины");
            emit finished();
            return;
        }
    }

    quint64* spectrum = (quint64*)calloc(  n + 1 , sizeof(quint64) );
    if (!spectrum) {
        emit errorOccurred("Ошибка выделения памяти");
        emit finished();
        return;
    }
    quint64 blockCount = (n + 63) / 64;
    quint64 wordsNeeded = k * blockCount;
    quint64* packed = (quint64*)calloc(wordsNeeded, sizeof(quint64));
    if (!packed) {
        emit errorOccurred("Ошибка выделения памяти");
        emit finished();
        free(spectrum);
        return;
    }

    for (quint64 i = 0; i < k; ++i) {
        quint64* rowData = packed + i * blockCount;
        const QString &row = rows[(int)i];
        for (quint64 j = 0; j < n; ++j) {
            if (row.at((int)j) == QLatin1Char('1')) {
                quint64 blockIdx = j / 64;
                quint64 bitIdx = j % 64;
                rowData[blockIdx] |= ( 1ull << bitIdx );
            }
        }
    }

    quint64 totalOps = sumCombinations( k, maxComb );
    quint64 doneOps = 0;
    if (totalOps == 0) {
        emit finished();
        free(packed);
        free(spectrum);
        return;
    }

    QSettings s;


    refreshPlotMs           = s.value( "refreshPlotMs"        ).toULongLong();
    refreshPteMs            = s.value( "refreshPteMs"         ).toULongLong();
    refreshProgressbarMs    = s.value( "refreshProgressbarMs" ).toULongLong();

    auto startTime        = steady_clock::now();
    auto lastTimePte      = startTime;
    auto lastTimePlot     = startTime;
    auto lastTimeBar      = startTime;
    auto lastEstimateTime = startTime;
    
    for (unsigned r = 0; r <= maxComb; ++r) {
        if (cancelled.load()) break;

        quint64 combCount = combin(k, r);
        if (combCount == 0) continue;
        #pragma omp parallel
        {
            std::vector<quint64> localCodeword(blockCount, 0);

            #pragma omp for schedule(dynamic)
            for (qint64 idx = 0; idx < combCount; ++idx) {
                if ( cancelled.load() ){
                    continue;
                }

                while ( paused.load() != 0 ) {
                    QThread::msleep(50);
                    if ( cancelled.load() ) break;
                }
                if ( cancelled.load() ) {
                    continue;
                }

                quint64 mask = generateBitMask(k, r, idx);
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
                    std::chrono::duration<quint64, std::milli> pteMs{  refreshPteMs.load()         };
                    std::chrono::duration<quint64, std::milli> plotMs{ refreshPlotMs.load()        };
                    std::chrono::duration<quint64, std::milli> barMs{  refreshProgressbarMs.load() };

                    auto now = steady_clock::now();
                    if ( now - lastTimePte >= pteMs ){
                        lastTimePte = now;
                        QVector<quint64> spectrumCopy(n + 1);
                        #pragma omp critical
                        {
                            for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = spectrum[(quint64)w];
                        }
                        emit updateSpectrumPTE(  spectrumCopy );
                    } // if ( now - lastTimePte >= pteMs )
                    if ( now - lastTimePlot >= plotMs ){
                        lastTimePlot = now;
                        QVector<quint64> spectrumCopy(n + 1);
                        #pragma omp critical
                        {
                            for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = spectrum[(quint64)w];
                        }
                        emit updateSpectrumPlot(  spectrumCopy );
                    } //if ( now - lastTimePlot >= plotMs )
                    if ( now - lastTimeBar >= barMs ){
                        lastTimeBar = now;
                        int percent = int(100.0 * double(doneOps) / double(totalOps));
                        emit updateInfoPBR(percent);
                    } // if ( now - lastTimeBar >= barMs )
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

                    } // if (now - lastEstimateTime >= std::chrono::seconds(10))

                } // if (omp_get_thread_num() == 0)

            } // for (quint64 idx = 0; idx < combCount; ++idx)

        } // #pragma omp parallel

    } // for (unsigned r = 1; r <= maxComb; ++r)

    if (cancelled.load()) {
        free(packed);
        emit finished();
        emit updateInfoPBR(0);
        QVector<quint64> spectrumCopy(n + 1);
        #pragma omp critical
        {
            for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = spectrum[(quint64)w];
        }
        emit updateSpectrumPTE(spectrumCopy);
        emit updateSpectrumPlot(spectrumCopy);
        free(spectrum);
        return;
    }

    QVector<quint64> spectrumCopy(n + 1);
    {
        for (quint64 w = 0; w <= n; ++w) spectrumCopy[(int)w] = spectrum[(quint64)w];
    }
    emit finished();
    emit updateInfoPBR(100);
    emit updateSpectrumPTE( spectrumCopy);
    emit updateSpectrumPlot(spectrumCopy);
    free(spectrum);
    free(packed);
}
