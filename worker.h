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

class Worker : public QObject
{
    Q_OBJECT
public:
    explicit Worker(QObject *parent = nullptr);
    ~Worker() override;

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

    quint64**                                binomTable;

private:
    std::atomic<int> paused    { 0 };
    std::atomic<int> cancelled { 0 };
    quint64** buildBinomTable(unsigned maxN);
    std::atomic<quint64> refreshProgressbarMs = DEFAULT_PROGRESSBAR_MS;
    std::atomic<quint64> refreshSpectrumMs    = DEFAULT_SPECTRUM_MS;
    bool                 USE_GPU              = DEFAULT_USE_GPU;
    bool                 USE_GRAY_CODE        = DEFAULT_USE_GRAY_CODE;
};

#endif // WORKER_H
