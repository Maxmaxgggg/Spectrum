#ifndef WORKER_H
#define WORKER_H

#include <QObject>
#include <QVector>
#include <QAtomicInteger>
#include <QMutex>
#include <QSettings>
#include <atomic>
#include <omp.h>

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


signals:
    void updateInfoPBR(       int percent                       );
    void updateSpectrumPTE(   const QVector<quint64>& spectrum  );
    void updateSpectrumPlot(  const QVector<quint64>& spectrum  );
    void errorOccurred(       const QString& message            );
    void finished();
    void updateRemainingMinutes(int minutesLeft);

private:
    // helpers for combination counting / unranking
    static quint64 combin(unsigned int n, unsigned int k);
    static quint64 generateBitMask(unsigned k, unsigned r, quint64 idx);
    static quint64 sumCombinations(quint64 k, quint64 maxComb);

public:
    std::atomic<quint64>     refreshProgressbarMs;
    std::atomic<quint64>     refreshPlotMs;
    std::atomic<quint64>     refreshPteMs;

private:
    std::atomic<int> paused    { 0 };
    std::atomic<int> cancelled { 0 };

    quint64 lastDoneOps = 0;

};

#endif // WORKER_H
