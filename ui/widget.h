#ifndef WIDGET_H
#define WIDGET_H

#include "qcustomplot.h"
#include "settingsdialog.h"
#include "worker.h"
#include "workwithmatrix.h"
#include "defines.h"
#include <qmainwindow.h>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class Worker;
class QCPAxisTickerText;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Подключаем воркера (после создания worker и workerThread)
    
protected:
    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        QVector<float> vec(yCache.size());
        for (int i = 0; i < yCache.size(); ++i) {
            vec[i] = (float) yCache.at(i);
        }
        updatePlot(vec);               // ваша быстрая функция обновления
    }

public: signals:
    void setInterfaceEnabled(bool enabled);
    void matrixChanged(int rows, int cols);

    void refreshSpectrumValueChanged(int);
    void refreshProgressbarValueChanged(int);
    void sendInitialSettingsToWorker( const QJsonObject&);
    void sendSettingsToWorker(        const QJsonObject&);
    void requestSettings();

private slots:
    void on_executePBN_clicked();
    void on_exitPBN_clicked();
    void on_settingsPBN_clicked();
    void on_cancelPBN_clicked();

    void handleStrValChanged();
    void handleUpdateInfoPBR(int percent);
    void handleUpdateSpectrumPlot( const QVector<float> spectrum ); // сигнал от воркера
    void handleUpdateSpectrumPTE(  const QStringList    spectrum );
    void handleError(const QString& message);
    void handleFinished(int);
    void handleUpdateRemainingMinutes(int, int);

    void handleMatrixChanged();

    bool eventFilter(QObject *watched, QEvent *event) override;

    void onAddMatrixTriggered();
private:

    Ui::MainWindow *ui;
    QSplitter      *splitter         = nullptr;
    Worker         *workerPtr        = nullptr;
    QThread        *workerThreadPtr  = nullptr;
    SettingsDialog *settings         = nullptr;
    QMenu* matrixMenu = nullptr;
    QMenu          *deleteMenu       = nullptr;
    QTimer         *matrixMenuTimer  = nullptr;
    // Переписать
    QPointer<QAction> pendingHover;

    // состояние выполнения: Idle / Running / Paused
    enum class RunState { Idle, Running, Paused };
    RunState runState = RunState::Idle;

    // Сохраняем старые значения при обновлении
    QCPBars* spectrumBars = nullptr;
    QCPItemText* msg = nullptr;
    QVector<double> xCache;
    QVector<double> yCache;
    QSharedPointer<QCPAxisTicker> tickerCache;
    int tickerStepCache = -1;
    int sizeCache = 0;

    int remainingMinutes = -1;


    void updatePlot(const QVector<float>& spectrum);
    QVector<QString> buildAxisLabels(int size, int step) const;
    void setWorker();
    void setMatrixMenu();
    void setMatrixActionsEnabled(bool);
    bool matrixActionsEnabled = true;
    void rebuildMatrixMenuActions();
    void setToolTips();
    void connectSettingsDialog();
    void applySettings();
    void applySpectrumColor(); 
    void saveSettings(); 
    void loadSettings(); 
    QString formatRemainingTime(int minutesTotal);



    QJsonArray matrices;
    void loadMatricesArray();
    void saveMatricesArray();
    void saveMatrixByName(const QString& name);
    bool removeMatrixByName(const QString& name);
    int  findMatrixIndexByName(const QString& name);
    QString getMatrixByName(const QString& name);
    QStringList listMatrixNames();
    QString defaultMatrixName();

};

#endif // WIDGET_H
