#ifndef WORKER_H
#define WORKER_H

#include <atomic>
#include <QThread>
#include <cmath>
#include <omp.h>
#include <cuda_runtime.h>
#include <qjsonobject.h>

#include "defines.h"
#include "dualcode.h"
#include "computeSpectrumKernel.cuh"
#include "bitmask.h"

using namespace std::chrono;

class Worker : public QObject
{
    Q_OBJECT
public:
    explicit Worker(QObject *parent = nullptr);
    ~Worker() override;

    enum class Algorithm {
        GpuGray,
        GpuNoGray,
        CpuGray,
        CpuNoGray
    };
    void pause();
    void resume();
    void cancel();
    void uncancel();
    bool isCancelled();

public slots:
    void computeSpectrum(  QStringList rows, quint64 maxComb  );
    
    
    
    void handleRefreshSpectrumValueChanged(int);
    void handleRefreshProgressbarValueChanged(int);
    void handleUseGpuToggled(bool);
    void handleUseGrayCodeToggled(bool);
    void handleUseDualCodeToggled(bool);
    void setInitialSettings(const QJsonObject&);


signals:
    void updateInfoPBR(       int percent                       );
    void updateSpectrumPTE(   const QVector<quint64> spectrum  );
    void updateSpectrumPlot(  const QVector<quint64> spectrum  );
    void errorOccurred(       const QString& message            );
    void finished( int );
    void updateRemainingMinutes(int elapsedSec, int minutesLeft);
    void GPUnotFound();

private:
    static  quint64 sumCombinations(quint64 k, quint64 maxComb);
    static void CUDART_CB spectrumHostCallback(void* userData);
    void    freeBinomTable(quint64** C, unsigned maxN);
    quint64** buildBinomTable(unsigned maxN, unsigned maxComb);
    Algorithm chooseAlgorithm(bool useGpu, bool useGray) const;
    void computeSpectrumGpuGray(   quint64 k, quint64 n, quint64 wordsPerRow, quint64 chunkSize, int blockCount, int threadsPerBlock );
    void computeSpectrumGpuNoGray(
        quint64 numOfRows,
        quint64 numOfCols,
        quint64 wordsPerRow,
        //quint64 chunkSize,
        int blockCount,
        int threadsPerBlock,
        quint64 maxComb);
    void computeSpectrumCpuGray(   quint64 k, quint64 n, quint64 wordsPerRow );
    void computeSpectrumCpuNoGray( quint64 k, quint64 n, quint64 wordsPerRow, quint64 maxComb );

    quint64** binomTable;
    std::atomic<int> paused    { 0 };
    std::atomic<int> cancelled { 0 };
    
    std::atomic<quint64> refreshProgressbarMs = DEFAULT_PROGRESSBAR_MS;
    std::atomic<quint64> refreshSpectrumMs    = DEFAULT_SPECTRUM_MS;
    bool                 useGpu               = DEFAULT_USE_GPU;
    bool                 useGrayCode          = DEFAULT_USE_GRAY_CODE;
    bool                 useDualCode          = DEFAULT_USE_DUAL_CODE;


    steady_clock::time_point startTime;
    steady_clock::time_point lastTimeSpectrum;
    steady_clock::time_point lastTimeBar;
    steady_clock::time_point lastEstimateTime;



    cudaStream_t  stream = nullptr;
    cudaEvent_t   ev     = nullptr;

    quint64* h_spectrum = nullptr;
    quint64* d_spectrum = nullptr;
    quint64* h_matrix   = nullptr;
};

#endif // WORKER_H
