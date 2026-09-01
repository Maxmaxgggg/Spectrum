#ifndef WIDGET_H
#define WIDGET_H

#include "qcustomplot.h"
#include "settingsdialog.h"
#include "worker.h"
#include "workwithmatrix.h"
#include "defines.h"

#include "autosavestore.h"
#include "matrixmenu.h"
#include "spectrumplot.h"
#include "taskbarprogress.h"

#include <qmainwindow.h>

#include <memory>

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

public: signals:
    void setInterfaceEnabled(bool enabled);
    void matrixChanged(int rows, int cols);

    void refreshSpectrumValueChanged(int);
    void refreshProgressbarValueChanged(int);
    void sendInitialSettingsToWorker( QJsonObject& );
    
    void sendSettingsToWorker( const QJsonObject& );
    void requestSettings();
    // Настройки расчёта из поднятого автосохранения.
    void applySettingsFromAutosave(int algorithm, int enumType, int maxRows);

private slots:

    void on_executePBN_clicked();
    void on_exitPBN_clicked();
    void on_settingsPBN_clicked();
    void on_cancelPBN_clicked();
    void handleUpdateInfoPBR(int percent);
    void sendSettingsToWorker();
    void handleUpdateSpectrumPlot( const SpectrumFloat   spectrum ); // сигнал от воркера
    void handleUpdateSpectrumPTE(  const SpectrumText    spectrum );
    void handleError(const QString& message);
    void handleFinished(int);
    void handleUpdateRemainingMinutes(int elapsedSec, int minutesLeft, double speed,
                                      quint64 doneOps, quint64 totalOps);
    void showSaveLBL();
    void handleGridTuned(int blocks, int threads);
    void handleMatrixChanged();

    bool eventFilter(QObject *watched, QEvent *event) override;

    // Проверяет, есть ли для текущих настроек чекпоинт, если есть - предлагает загрузить его
    bool hasCheckpoint() const;
private:

    Ui::MainWindow *ui;
    QSplitter      *splitter         = nullptr;
    Worker         *workerPtr        = nullptr;
    QThread        *workerThreadPtr  = nullptr;
    SettingsDialog *settingsDialog   = nullptr;
    MatrixMenu     *matrixMenu       = nullptr;

    // Автосохранения расчёта на диске.
    AutosaveStore   autosave;
    QGraphicsOpacityEffect* saveLBLOpacityEffect;
    // состояние выполнения: Idle / Running / Paused
    // Loaded — из диалога поднято автосохранение: спектр и прогресс уже на
    // экране, кнопка предлагает продолжить, и спрашивать при запуске второй
    // раз незачем.
    enum class RunState { Idle, Running, Paused, Loaded };
    RunState runState = RunState::Idle;

    // График спектра. Создаётся в конструкторе, когда виджет из .ui готов.
    std::unique_ptr<SpectrumPlot> spectrumPlot;

    int remainingMinutes = -1;
    // Строка о подобранной сетке. Пустая, если подбор не проводился.
    QString tunedGrid;


    QString matrixError() const;
    void startComputation();
    void pauseComputation();
    void resumeComputation();

    void setWorker();
    void showAutosaveDialog();
    void applyAutosave(const Matrix& matrix, const AutosaveRecord& record);
    // Пока матрица подставляется из записи, правкой её считать нельзя.
    bool applyingAutosave = false;
    void setMatrixMenu();
    void setToolTips();
    void connectSettingsDialog();
    void applySettings();
    void saveSettings(); 
    void loadSettings(); 

    // Прогресс на кнопке приложения в панели задач. Создаётся в конструкторе
    // после сборки окна: индикатору нужен готовый нативный дескриптор.
    std::unique_ptr<TaskbarProgress> taskbar;
};

#endif // WIDGET_H
