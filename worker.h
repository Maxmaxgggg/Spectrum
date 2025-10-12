#ifndef WORKER_H
#define WORKER_H

#include <QObject>
#include <QVector>
#include <QAtomicInteger>
#include <QMutex>
#include <QSettings>
#include <atomic>
#include <omp.h>
#include "defines.h"
#include <cuda_runtime.h>

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

public slots:
    void computeSpectrum(  QStringList rows, quint64 maxComb  );
    void pause();
    void resume();
    void cancel();

    
    void handleRefreshSpectrumValueChanged(int);
    void handleRefreshProgressbarValueChanged(int);
    void handleUseGpuToggled(bool);
    void handleUseGrayCodeToggled(bool);
    void setInitialSettings(bool, bool, int, int);


signals:
    void updateInfoPBR(       int percent                       );
    void updateSpectrumPTE(   const QVector<quint64>& spectrum  );
    void updateSpectrumPlot(  const QVector<quint64>& spectrum  );
    void errorOccurred(       const QString& message            );
    void finished();
    void updateRemainingMinutes(int minutesLeft);
    void GPUnotFound();

private:
    quint64 generateBitMask(unsigned k, unsigned r, quint64 idx, quint64** C);
    static  quint64 sumCombinations(quint64 k, quint64 maxComb);
    void    freeBinomTable(quint64** C, unsigned maxN);
    quint64** buildBinomTable(unsigned maxN);
    Algorithm chooseAlgorithm(bool useGpu, bool useGray) const;

    void computeSpectrumGpuGray(   quint64 k, quint64 n, quint64 blockCount, quint64 chunkSize, int maxBlocks, int threadsPerBlock );
    void computeSpectrumGpuNoGray( quint64 k, quint64 n, quint64 blockCount, quint64 chunkSize, int maxBlocks, int threadsPerBlock, quint64 maxComb );
    void computeSpectrumCpuGray(   quint64 k, quint64 n, quint64 blockCount );
    void computeSpectrumCpuNoGray( quint64 k, quint64 n, quint64 blockCount, quint64 maxComb );

    quint64** binomTable;
    std::atomic<int> paused    { 0 };
    std::atomic<int> cancelled { 0 };
    
    std::atomic<quint64> refreshProgressbarMs = DEFAULT_PROGRESSBAR_MS;
    std::atomic<quint64> refreshSpectrumMs    = DEFAULT_SPECTRUM_MS;
    bool                 useGpu               = DEFAULT_USE_GPU;
    bool                 useGrayCode          = DEFAULT_USE_GRAY_CODE;


    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastTimeSpectrum;
    std::chrono::steady_clock::time_point lastTimeBar;
    std::chrono::steady_clock::time_point lastEstimateTime;



    cudaStream_t  stream = nullptr;
    cudaEvent_t   ev     = nullptr;

    quint64* h_spectrum = nullptr;
    quint64* d_spectrum = nullptr;
    quint64* h_matrix   = nullptr;
};

#endif // WORKER_H
