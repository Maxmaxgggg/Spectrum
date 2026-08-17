#ifndef WORKER_H
#define WORKER_H

#include <atomic>
#include <QThread>
#include <cmath>
#include <omp.h>
#include <cuda_runtime.h>
#include <qjsonobject.h>
#include <qsettings.h>

#include "defines.h"
#include "dualcode.h"
#include "computeSpectrumKernel.cuh"
#include "settings.h"
#include "progresstracker.h"
#include "binomtable.h"
// Переопределяет CUDA_CALL из .cuh: там макрос звал abort(), здесь бросает.
#include "cudabuffers.h"

using namespace std::chrono;
enum LoadMode {
    Reset,
    FromCheckpoint,
    FromSave
};

Q_DECLARE_METATYPE(LoadMode)
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

    // Воспроизводимое прерывание для тестов возобновления: сохранять чекпоинт
    // каждые everyOps операций и остановиться, дойдя до stopAfterOps.
    // Прерывание по таймеру для этого не годится — точка обрыва каждый раз
    // разная, и результаты двух запусков не сравнить.
    // Оба нуля — обычный режим работы.
    void setCheckpointOpsPolicy(quint64 everyOps, quint64 stopAfterOps);

public slots:
    void computeSpectrum( );
    void setSettings( const QJsonObject& jsonSettings );
    void initializeRunState(LoadMode lm);
signals:
    // Сигнал для обновления progressbar-а
    void updateInfoPBR(       int percent                       );
    // Сигнал для обновления текстового спектра
    void updateSpectrumPTE(   const SpectrumText spectrum       );
    // Сигнал для обновления графического спектра
    void updateSpectrumPlot(  const SpectrumFloat spectrum      );
    // Сигнал, посылаемый при возникновении ошибки
    void errorOccurred(       const QString& message            );
    // Сигнал, посылаемый при окончании расчета спектра
    void finished( int );
    // Сигнал для обновления таймера
    void updateRemainingMinutes( int elapsedSec, int minutesLeft, double speed );
    // Сигнал того, что надо показать значок сохранения
    void showSaveLBL();
private:
    /* Функции для работы с биноминальными коэффициентами */
    static    quint64 sumCombinations(quint64 k, quint64 maxComb);


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
        int blockCount,
        int threadsPerBlock,
        quint64 maxComb
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
    void    makeCheckpoint(int numOfCols);
    // Чекпоинт для GPU-путей: спектр надо забрать с устройства синхронно.
    // Поток передаётся явно — длинный путь работает на собственном, а не на
    // потоке класса. false — ошибка CUDA, расчёт продолжать нельзя.
    bool    saveGpuCheckpoint(cudaStream_t s, int numOfCols,
                              quint64 rOffset, quint64 chunkOffset);
    // Чекпоинт для CPU-путей: спектр уже лежит в h_spectrum.
    void    saveCpuCheckpoint(int numOfCols, quint64 rOffset, quint64 chunkOffset);
    // Останавливает расчёт, если задан порог из setCheckpointOpsPolicy.
    void    stopIfOpsLimitReached();

    /* Отчёт о ходе расчёта — общий для всех шести вычислительных путей */
    void    reportEstimate();
    void    reportProgressBar();

    // Ждёт снятия паузы. Возвращает false, если расчёт отменили.
    bool    waitWhilePaused();


    std::atomic<int> paused    { 0 };
    std::atomic<int> cancelled { 0 };
    std::atomic_bool requestRunState = false;

    ComputationSettings         settings;
    RunState                    runState;
    ProgressTracker             progress;

    bool                        exportSpectrum = false;
    // 0 — обычный режим; см. setCheckpointOpsPolicy
    quint64                     stopAfterOps   = 0;
    quint64                     checkpointEveryOps = 0;




    // Число масок в одном чанке
    quint64 chunkSize = 1 << 20;

    // Все ресурсы владеющие: освобождаются вместе с объектом, каким бы путём
    // ни завершился расчёт — успехом, отменой или исключением.
    CudaStream           stream;
    CudaEvent            ev;

    HostBuffer<quint64>  h_spectrum;
    HostBuffer<quint64>  h_matrix;
    BinomTable           binomTable;

    DeviceBuffer<quint64> d_spectrum;
    DeviceBuffer<quint64> d_matrix;
    DeviceBuffer<quint64> d_binomTable;

    // Тело расчёта. Отделено от computeSpectrum(), чтобы та могла обернуть
    // его в try/catch и превратить исключение в сигнал об ошибке.
    void computeSpectrumImpl();
    // Освобождает всё, что выделено под расчёт.
    void releaseResources();
};

#endif // WORKER_H
