#ifndef WIDGET_H
#define WIDGET_H

#include "qcustomplot.h"
#include "settingsdialog.h"
#include "worker.h"
#include "workwithmatrix.h"
#include "defines.h"

#include <qmainwindow.h>

#ifdef Q_OS_WIN
    #include <windows.h>
    #include <shobjidl.h>
#endif

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
    ComputationSettings settings;
    // Подключаем воркера (после создания worker и workerThread)
    
protected:
    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        SpectrumFloat vec(yCache.size());
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
    void sendInitialSettingsToWorker( QJsonObject& );
    
    void sendSettingsToWorker( const QJsonObject& );
    void requestSettings();

private slots:

    void on_executePBN_clicked();
    void on_exitPBN_clicked();
    void on_settingsPBN_clicked();
    void on_cancelPBN_clicked();
    void on_saveSpectrumACN_triggered();
    void handleStrValChanged();
    void handleUpdateInfoPBR(int percent);
    void sendSettingsToWorker();
    void handleUpdateSpectrumPlot( const SpectrumFloat   spectrum ); // сигнал от воркера
    void handleUpdateSpectrumPTE(  const SpectrumText    spectrum );
    void handleError(const QString& message);
    void handleFinished(int);
    void handleUpdateRemainingMinutes(int, int, double);
    void showSaveLBL();
    void handleMatrixChanged();

    bool eventFilter(QObject *watched, QEvent *event) override;

    void onAddMatrixTriggered();

    // Проверяет, есть ли для текущих настроек чекпоинт, если есть - предлагает загрузить его
    bool hasCheckpoint() const;
private:

    Ui::MainWindow *ui;
    QSplitter      *splitter         = nullptr;
    Worker         *workerPtr        = nullptr;
    QThread        *workerThreadPtr  = nullptr;
    SettingsDialog *settingsDialog   = nullptr;
    QMenu* matrixMenu = nullptr;
    QMenu          *deleteMenu       = nullptr;
    QTimer         *matrixMenuTimer  = nullptr;
    // Переписать
    QPointer<QAction>       pendingHover;
    QGraphicsOpacityEffect* saveLBLOpacityEffect;
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


    void updatePlot(const SpectrumFloat& spectrum);
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
    QString formatSpeed(double speed);


    QJsonArray matrices;
    void loadMatricesArray();
    void saveMatricesArray();
    void saveMatrixByName(const QString& name);
    bool removeMatrixByName(const QString& name);
    int  findMatrixIndexByName(const QString& name);
    QString getMatrixByName(const QString& name);
    QStringList listMatrixNames();
    QString defaultMatrixName();


    // Атрибуты, нужные для отображения прогресс бара под иконкой приложения
    #ifdef Q_OS_WIN
        ITaskbarList3* taskbar = nullptr;
        HWND hwnd = nullptr;
        bool taskbarAvailable = false;
    #endif
};

#endif // WIDGET_H
