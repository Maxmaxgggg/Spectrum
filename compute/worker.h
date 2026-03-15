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
#include "settings.h"
using namespace std::chrono;

class Worker : public QObject
{
    Q_OBJECT
public:
    explicit Worker(QObject *parent = nullptr);
    ~Worker() override;

    void pause();
    void resume();
    void cancel();
    void uncancel();
    bool isCancelled();

public slots:
    void computeSpectrum(  QStringList rows );
    void setSettings( const QJsonObject& jsonSettings );
signals:
    // Сигнал для обновления progressbar-а
    void updateInfoPBR(       int percent                       );
    // Сигнал для обновления текстового спектра
    void updateSpectrumPTE(   const QStringList spectrum        );
    // Сигнал для обновления графического спектра
    void updateSpectrumPlot(  const QVector<float> spectrum     );
    // Сигнал, посылаемый при возникновении ошибки
    void errorOccurred(       const QString& message            );
    // Сигнал, посылаемый при окончании расчета спектра
    void finished( int );
    // Сигнал для обновления таймера
    void updateRemainingMinutes(int elapsedSec, int minutesLeft);

private:
    /* Функции для работы с биноминальными коэффициентами */
    static    quint64 sumCombinations(quint64 k, quint64 maxComb);
    void      freeBinomTable(quint64** C, unsigned maxN);
    quint64** buildBinomTable(unsigned maxN, unsigned maxComb);


    /* Функции для расчета спектра кода */
    void      computeSpectrumGpuGrayShort(  
        quint64 numOfRows,
        quint64 numOfCols,
        quint64 wordsPerRow, 
        quint64 chunkSize, 
        int blockCount, 
        int threadsPerBlock
    );
    void      computeSpectrumGpuNoGrayLong(
        quint64 numOfRows,
        quint64 numOfCols,
        quint64 wordsPerRow,
        //quint64 chunkSize,
        int blockCount,
        int threadsPerBlock,
        quint64 maxComb,
        quint64* d_matrix = nullptr
    );
    void computeSpectrumGpuNoGrayShort(
        quint64 numOfRows,
        quint64 numOfCols,
        quint64 wordsPerRow,
        quint64 chunkSize,
        int blockCount,
        int threadsPerBlock,
        quint64 maxComb
    );
    void computeSpectrumCpuNoGrayLong(
        quint64 numOfRows,
        quint64 numOfCols,
        quint64 wordsPerRow,
        quint64 maxComb
    );
    void computeSpectrumCpuGrayShort(   
        quint64 numOfRows,
        quint64 numOfCols,
        quint64 wordsPerRow
    );
    void computeSpectrumCpuNoGrayShort( 
        quint64 numOfRows, 
        quint64 numOfCols, 
        quint64 wordsPerRow, 
        quint64 maxComb 
    );

    /* Функции, посылающие сигнал для обновления интерфейса */
    void updateSpectrum(int numOfCols);
    void updateSpectrumDual( int numOfCols, int numOfRows );


    /* Функции для работы с чекпоинтами */
    void    makeCheckpoint();
    void    loadCheckpoint();

    
    std::atomic<int> paused    { 0 };
    std::atomic<int> cancelled { 0 };
    
    std::atomic<quint64> refreshProgressbarMs = DefaultValues::PROGRESSBAR_MS;
    std::atomic<quint64> refreshSpectrumMs    = DefaultValues::SPECTRUM_MS;

    ComputationSettings      settings;
    steady_clock::time_point startTime;
    steady_clock::time_point lastTimeSpectrum;
    steady_clock::time_point lastTimeBar;
    steady_clock::time_point lastEstimateTime;




    // Индекс стартовой маски
    quint64 startChunkInd = 0;
    // Число масок между двумя чекпоинтами
    quint64 chunkSize = 1 << 20;
    // Текущий спектр
    QVector<quint64> spectrum;

    cudaStream_t  stream   = nullptr;
    cudaEvent_t   ev       = nullptr;

    quint64*  h_spectrum   = nullptr;
    quint64*  h_matrix     = nullptr;
    quint64** h_binomTable = nullptr;

    quint64*  d_spectrum   = nullptr;
    quint64*  d_matrix     = nullptr;
    quint64*  d_binomTable = nullptr;
    
};

#endif // WORKER_H
