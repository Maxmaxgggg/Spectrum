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
    
protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        QVector<quint64> vec(yCache.size());
        for (int i = 0; i < yCache.size(); ++i) {
            vec[i] = (quint64) yCache.at(i);
        }
        updatePlot(vec);               // ваша быстрая функция обновления
    }

public: signals:
    void toggleStringsSPB(bool);
    void toggleUseGpuCHB(bool);
    void toggleUseGrayCodeCHB(bool);
    void matrixChanged(int);
    void gpuNotFound();

    void useGpuToggled(bool);
    void useGrayCodeToggled(bool);
    void refreshSpectrumValueChanged(int);
    void refreshProgressbarValueChanged(int);
    void sendInitialSettingsToWorker(bool, bool, int, int);
    void requestInitialSettings();

private slots:
    void on_executePBN_clicked();
    void on_exitPBN_clicked();
    void on_settingsPBN_clicked();

    void handleStrValChanged();
    void handleUpdateInfoPBR(int percent);
    void handleUpdateSpectrumPlot( const QVector<quint64>& spectrum ); // сигнал от воркера
    void handleUpdateSpectrumPTE(  const QVector<quint64>& spectrum );
    void handleError(const QString& message);
    void handleGPUnotFound();
    void handleFinished();
    void handleUpdateRemainingMinutes(int);

    void handleMatrixChanged();

    bool eventFilter(QObject *watched, QEvent *event) override;

private:

    Ui::Widget     *ui;
    QSplitter      *splitter         = nullptr;
    Worker         *workerPtr        = nullptr;
    QThread        *workerThreadPtr  = nullptr;
    SettingsDialog *settings         = nullptr;

    // состояние выполнения: Idle / Running / Paused
    enum class RunState { Idle, Running, Paused };
    RunState runState = RunState::Idle;

    // Сохраняем старые значения при обновлении
    QCPBars* spectrumBars = nullptr;
    QVector<double> xCache;
    QVector<double> yCache;
    QSharedPointer<QCPAxisTicker> tickerCache;
    int tickerStepCache = -1;
    int sizeCache = 0;

    int remainingMinutes = -1;


    void updatePlot(const QVector<quint64>& spectrum);
    QVector<QString> buildAxisLabels(int size, int step) const;
    void setWorker(Worker* worker, QThread* workerThread);
    void connectSettingsDialog();
    void applySettings();
    void applySpectrumColor(); 
    void saveSettings(); 
    void loadSettings(); 
    QString formatRemainingTime(int minutesTotal);


};

#endif // WIDGET_H
