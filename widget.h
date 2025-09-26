#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QVector>
#include <QTimer>
#include <QSettings>
#include <QSharedPointer>
#include <QElapsedTimer>
#include "qcustomplot.h"
#include "settingsdialog.h"

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class Worker;
class QCPAxisTickerText;

class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = nullptr);
    ~Widget() override;

    // Подключаем воркера (после создания worker и workerThread)
    void setWorker(Worker* worker, QThread* workerThread);
protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        QVector<quint64> vec(yCache.size());
        for (int i = 0; i < yCache.size(); ++i) {
            vec[i] = (quint64) yCache.at(i);
        }
        updatePlotFast(vec);               // ваша быстрая функция обновления
    }
private slots:
    // UI handlers (имя кнопки ожидается "executePBN" в .ui)
    void on_executePBN_clicked();
    void on_exitPBN_clicked();
    void on_settingsPBN_clicked();
    void handleSettingsApplied();
    void handleStrValChanged();

    // слоты от воркера
    void handleUpdateInfoPBR(int percent);
    void handleUpdateSpectrumPlot( const QVector<quint64>& spectrum ); // сигнал от воркера
    void handleUpdateSpectrumPTE(  const QVector<quint64>& spectrum );
    void handleError(const QString& message);
    void handleFinished();
    void handleUpdateRemainingMinutes(int);

    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Ui::Widget     *ui;
    QSplitter      *splitter         = nullptr;
    Worker         *workerPtr        = nullptr;
    QThread        *workerThreadPtr  = nullptr;
    SettingsDialog *settings         = nullptr;
    QTimer         *remainingTimer   = nullptr;
    int             remainingMinutes = -1;
    // состояние выполнения: Idle / Running / Paused
    enum class RunState { Idle, Running, Paused };
    RunState runState = RunState::Idle;

    // Кеш текущего/последнего снимка (GUI-поток)
    //QVector<quint64> pendingSpectrum; // последний полученный снимок от воркера
    //bool havePendingSpectrum = false;

    int requestIntervalMs = 1000; // default 1s
    int plotIntervalMs    = 200;  // default 200 ms
    int pteIntervalMs     = 500;  // default 500 ms

    // Plot caches and state (optimize updatePlot)
    QCPBars* spectrumBars = nullptr;
    QVector<double> xCache;
    QVector<double> yCache;
    QSharedPointer<QCPAxisTicker> tickerCache;
    int tickerStepCache = -1;
    int sizeCache = 0;



    // helpers
    void updatePlotFast(const QVector<quint64>& spectrum);
    QVector<QString> buildAxisLabels(int size, int step) const;
    // settings helpers
    void applySettings(); // читает QSettings и применяет (timers, color)
    void applySpectrumColor(); // применяет цвет к QCPBars
    void saveSettings(); // сохраняет текущие интервалы/цветы в QSettings
    void loadSettings(); // загружает и применяет (вызывается в ctor)
    QString formatRemainingTime(int minutesTotal);


};

#endif // WIDGET_H
