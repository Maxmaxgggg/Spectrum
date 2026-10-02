#ifndef WIDGET_H
#define WIDGET_H

#include "qcustomplot.h"
#include "settingsdialog.h"
#include "worker.h"
#include "constants.h"

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

signals:
    void setInterfaceEnabled(bool enabled);
    void matrixChanged(int rows, int cols);

    void sendSettingsToWorker( const ComputationSettings& settings );
    void requestSettings();
    // Настройки расчёта из поднятого автосохранения.
    void applySettingsFromAutosave(int algorithm, int enumType, int rank, int weight,
                                   int componentAlgorithm);

private slots:

    void on_executePBN_clicked();
    void on_exitPBN_clicked();
    void on_cancelPBN_clicked();
    void handleUpdateInfoPBR(int percent);
    // Спектр от воркера: текст и график.
    void handleSpectrum(const SpectrumCounts& spectrum);
    void handleError(const QString& message);
    void handleFinished(int);
    void handleUpdateRemainingMinutes(int elapsedSec, int minutesLeft, double speed,
                                      quint64 doneOps, quint64 totalOps);
    void showSaveLBL();
    void handleGridTuned(int blocks, int threads);
    // План Брауэра–Циммермана: показать в панели и пометить в спектре веса,
    // которые перебор не гарантирует.
    void handlePlanReady(int sets, int rows, int exactUpToWeight);
    // Ход случайного поиска: панель и пометки «ещё не найдено» в спектре.
    void handleSearchEstimate(int weight, quint64 trialsDone, quint64 trialsTotal,
                              double missProbability, SpectrumFloat unseenByWeight);
    // Код произведения: строка о ходе и точности в панели.
    void handleProductPlan(const QString& text, int exactUpToWeight);
    void handleMatrixChanged();

    bool eventFilter(QObject *watched, QEvent *event) override;

    // Проверяет, есть ли для текущих настроек чекпоинт, если есть - предлагает загрузить его
    bool hasCheckpoint() const;
private:

    Ui::MainWindow *m_ui;
    // Настройки расчёта: из диалога, вместе с матрицами из редакторов.
    ComputationSettings m_settings;
    QDockWidget    *m_matrixDock       = nullptr;
    // Обе матрицы — страницами одной панели; вкладки живут в заголовке
    // панели, в одной строке с её кнопками. У произвольного кода страница
    // одна и вкладок не видно; у кода произведения — две, «Матрица 1» и
    // «Матрица 2», и на стыке вкладок кнопка «поменять местами».
    class QTabBar*             m_matrixTabBar = nullptr;
    class QStackedWidget*      m_matrixPages  = nullptr;
    class FilterPlainTextEdit* m_matrix2PTE   = nullptr;
    // Показать или спрятать вторую вкладку по алгоритму.
    void updateMatrixTabs();
    // Заголовок панели и вкладок: имя и размер.
    void updateMatrixTitles();
    QDockWidget    *m_spectrumDock     = nullptr;
    QDockWidget    *m_plotDock         = nullptr;
    QDockWidget    *m_statsDock        = nullptr;
    class StatsPanel *m_statsPanel     = nullptr;
    // Снимок раскладки сразу после сборки — по нему работает «Раскладка по
    // умолчанию». Собирать её заново расстановкой доков ненадёжно: Qt не
    // обещает, что повторное добавление даст те же пропорции.
    QByteArray      m_defaultLayout;

    // Перерисовка графика откладывается: при перетаскивании панели события
    // изменения размера идут десятками в секунду, и рисовать на каждое —
    // это и есть подлагивание. Ждём, пока размер перестанет меняться.
    QTimer*         m_plotRefreshTimer = nullptr;
    Worker         *m_workerPtr        = nullptr;
    QThread        *m_workerThreadPtr  = nullptr;
    SettingsDialog *m_settingsDialog   = nullptr;
    MatrixMenu     *m_matrixMenu       = nullptr;

    // Автосохранения расчёта на диске.
    AutosaveStore   m_autosave;
    QGraphicsOpacityEffect* m_saveLBLOpacityEffect;
    // состояние выполнения: Idle / Running / Paused
    // Loaded — из диалога поднято автосохранение: спектр и прогресс уже на
    // экране, кнопка предлагает продолжить, и спрашивать при запуске второй
    // раз незачем.
    enum class RunState { Idle, Running, Paused, Loaded };
    RunState m_runState = RunState::Idle;

    // Запись, поднятая во время расчёта: применить её можно только после того,
    // как воркер остановится, иначе он затрёт её своими обновлениями.
    bool           m_pendingAutosave = false;
    Matrix         m_pendingMatrix;
    AutosaveRecord m_pendingRecord;

    // График спектра. Создаётся в конструкторе, когда виджет из .ui готов.
    std::unique_ptr<SpectrumPlot> m_spectrumPlot;

    int m_remainingMinutes = -1;
    // Строка о подобранной сетке. Пустая, если подбор не проводился.



    QString matrixError() const;
    void startComputation();
    void pauseComputation();
    void resumeComputation();

    void setWorker();
    // Текст, подсказка и значок кнопки запуска — по текущему состоянию.
    // Одним местом: раньше эти три вещи выставлялись в шести, и стоило
    // добавить состояние, как подпись и значок разъезжались.
    void updateExecuteButton();

    // Текст спектра — строками «вес - число слов» по m_lastSpectrum.
    void showSpectrumText();

    // Три панели живут в доках: любую можно вытащить в отдельное окно и
    // закрыть, а вернуть из меню «Вид». Раскладка запоминается целиком.
    void setupDocks();
    void resetLayout();

    // Последний показанный спектр — его же и сохраняем между запусками.
    SpectrumCounts m_lastSpectrum;
    // До какого веса показанный спектр точен. -1 — весь спектр на равных
    // (обычный перебор); иначе строки тяжелее помечаются как неполные.

    // Случайный поиск: сколько слов каждого веса, по оценке, ещё не найдено.
    // Пусто — пометок нет.
    SpectrumFloat m_unseenByWeight;

    void showAutosaveDialog();
    void applyAutosave(const Matrix& matrix, const AutosaveRecord& record);
    // Собственно подстановка записи в окно. Отделена от applyAutosave, потому
    // что во время расчёта её приходится откладывать до остановки воркера.
    void applyAutosaveNow(const Matrix& matrix, const AutosaveRecord& record);
    // Пока матрица подставляется из записи, правкой её считать нельзя.
    bool m_applyingAutosave = false;
    void setMatrixMenu();
    void setToolTips();
    void connectSettingsDialog();
    void applySettings();
    void saveSettings(); 
    void loadSettings(); 

    // Прогресс на кнопке приложения в панели задач. Создаётся в конструкторе
    // после сборки окна: индикатору нужен готовый нативный дескриптор.
    std::unique_ptr<TaskbarProgress> m_taskbar;
};

#endif // WIDGET_H
