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

// Всё, что выводится из настроек и матрицы до начала расчёта.
//
// Раньше эти величины были локальными переменными computeSpectrumImpl и
// передавались в вычислительные функции поштучно — по четыре-семь аргументов,
// причём chunkSize при этом перекрывал одноимённое поле класса.
struct CodeGeometry
{
    // Матрица, по которой идёт перебор. Для дуального кода это проверочная
    // матрица, а не та, что ввёл пользователь.
    QStringList matrix;

    quint64 numOfRows    = 0;
    quint64 numOfCols    = 0;
    // Число 64-битных слов на одну строку
    quint64 wordsPerRow  = 0;
    quint64 matrixWords  = 0;
    // Длина спектра: веса от 0 до numOfCols включительно
    quint64 spectrumSize = 0;
    // Максимальное число складываемых строк при частичном переборе
    quint64 maxRows      = 0;
    // Число масок в одном чанке
    quint64 chunkSize    = 1 << 20;

    int  blocksGpu  = 1;
    int  threadsGpu = 1;

    bool useGpu     = false;
    // Длинные коды (больше 63 строк) считаются другими функциями: маска в одно
    // слово туда уже не помещается.
    bool isLongCode = false;
    // Матрица не влезла в константную память и лежит в глобальной
    bool matrixInGlobalMem = false;
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

    // Порог, ниже которого подбор сетки не окупается и не проводится.
    // Тесты ставят ноль, чтобы гонять подбор на маленьких матрицах.
    void setGridTuningThreshold(double seconds);

    // Печатать таблицу замеров подбора. Для режима --sweep в тестах.
    void setGridTuningVerbose(bool on);

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
    // Сигнал для обновления таймера. doneOps/totalOps идут вместе со временем:
    // на задачах в миллиарды слов один процент мало что говорит о масштабе.
    void updateRemainingMinutes( int elapsedSec, int minutesLeft, double speed,
                                 quint64 doneOps, quint64 totalOps );
    // Сигнал того, что надо показать значок сохранения
    void showSaveLBL();
    // Подобранная замером сетка запуска. Пользователю её стоит показать:
    // иначе непонятно, на чём считает программа.
    void gridTuned( int blocks, int threads );
private:
    /* Функции для работы с биноминальными коэффициентами */
    quint64   totalCombinations(quint64 k, quint64 maxComb) const;


    /* Подготовка расчёта */
    // Выводит размеры и режимы из настроек и матрицы.
    CodeGeometry describeTask() const;
    // Упаковывает матрицу из строк QStringList в биты и раскладывает буферы
    // по памяти хоста и устройства.
    void prepareBuffers(const CodeGeometry& g);
    // Замеряет несколько сеток на настоящем ядре и ставит в g лучшую.
    // Ничего не делает, если подбор не включён или не применим.
    void tuneGrid(CodeGeometry& g);
    // Выбирает вычислительную функцию по алгоритму, устройству и длине кода.
    void dispatchComputation(const CodeGeometry& g);
    // Финальная выгрузка спектра, сигналы и освобождение ресурсов.
    void finishComputation(const CodeGeometry& g, steady_clock::time_point startedAt);

    /* Функции для расчета спектра кода */
    void computeSpectrumGpuGrayShort  (const CodeGeometry& g);
    void computeSpectrumGpuNoGrayShort(const CodeGeometry& g);
    void computeSpectrumGpuNoGrayLong (const CodeGeometry& g);
    void computeSpectrumCpuGrayShort  (const CodeGeometry& g);
    void computeSpectrumCpuNoGrayShort(const CodeGeometry& g);
    void computeSpectrumCpuNoGrayLong (const CodeGeometry& g);

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
    // См. setGridTuningThreshold
    double                      tuneThresholdSec   = 10.0;
    bool                        tuneVerbose        = false;

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
