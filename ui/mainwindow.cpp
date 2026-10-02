#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "autosavedialog.h"
#include "docktitlebar.h"
#include "filterplaintextedit.h"
#include "fluenticons.h"
#include "format.h"
#include "infosets.h"
#include "matrixlibrary.h"
#include "matrixmenu.h"
#include "spectrumplot.h"
#include "spectrumtextedit.h"
#include "statspanel.h"
#include "tabswapbutton.h"

#include <QApplication>
#include <QDockWidget>
#include <QHeaderView>
#include <QStackedWidget>
#include <QTabBar>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
    m_ui(new Ui::MainWindow)
{
    m_ui->setupUi(this);
    // Значок сохранения: невидим, пока не мигнёт при записи (showSaveLBL).
    // Эффект прозрачности в .ui не задаётся — только кодом.
    m_saveLBLOpacityEffect = new QGraphicsOpacityEffect(m_ui->saveLBL);
    m_ui->saveLBL->setGraphicsEffect(m_saveLBLOpacityEffect);
    m_ui->saveLBL->setToolTip(UiStrings::SAVE_LBL_BASE_TOOLTIP);
    m_saveLBLOpacityEffect->setOpacity(0.0);
    m_settingsDialog = new SettingsDialog(this);
    m_spectrumPlot = std::make_unique<SpectrumPlot>(m_ui->spectrumCPT);
    connect( m_ui->matrixPTE, &FilterPlainTextEdit::textChanged, this,  &MainWindow::handleMatrixChanged    );
    connect(m_ui->autosaveACN, &QAction::triggered,
        this, &MainWindow::showAutosaveDialog);
    connect(m_ui->settingsACN, &QAction::triggered,
        this, [this]() {
            m_settingsDialog->exec();
            applySettings();
        });


    setupDocks();
    loadSettings();
    m_plotRefreshTimer = new QTimer(this);
    m_plotRefreshTimer->setSingleShot(true);
    m_plotRefreshTimer->setInterval(Constants::PLOT_REFRESH_DELAY_MS);
    connect(m_plotRefreshTimer, &QTimer::timeout, this, [this]() {
        // Пока кнопка мыши зажата, панель ещё тащат. Перерисовка на этом
        // месте переразмечает окно, и разделитель теряет захват мыши — со
        // стороны это выглядит как «тянется через раз». Ждём дальше.
        if (QApplication::mouseButtons() != Qt::NoButton) {
            m_plotRefreshTimer->start();
            return;
        }
        m_spectrumPlot->refresh();
    });

    m_ui->spectrumCPT->installEventFilter(this);
    connectSettingsDialog();
    setWorker();
    setMatrixMenu();
    setToolTips();
    // Настройки диалога нужны окну сразу: от алгоритма зависит, показывать ли
    // панель второй матрицы. Пустая матрица сигнала о смене не даёт.
    emit requestSettings();
    handleMatrixChanged();
    // Старые чекпоинты лежали в реестре, по мегабайту с матрицей на запись.
    // Переносим их в файлы один раз и вычищаем ветку.
    m_autosave.migrateFromRegistry();

    m_taskbar = std::make_unique<TaskbarProgress>(this);
}

MainWindow::~MainWindow()
{
    saveSettings();
    if (m_workerPtr){
        m_workerPtr->cancel();
    }
    if(m_workerThreadPtr->isRunning()){
        m_workerThreadPtr->quit();
        m_workerThreadPtr->wait();
    }
    if(m_workerPtr){
        delete m_workerPtr;
        m_workerPtr = nullptr;
    }
    delete m_ui;
}

void MainWindow::setMatrixMenu()
{

    m_matrixMenu = new MatrixMenu(m_ui->loadMatrixACN, m_ui->saveMatrixACN, m_ui->createMatrixACN, this);
    // Загрузка и сохранение — в ту матрицу, чья вкладка открыта: у кода
    // произведения их две.
    auto currentEditor = [this]() -> QPlainTextEdit* {
        return (m_matrixPages && m_matrixPages->currentIndex() == 1) ? static_cast<QPlainTextEdit*>(m_matrix2PTE)
                                                                  : static_cast<QPlainTextEdit*>(m_ui->matrixPTE);
    };
    m_matrixMenu->setMatrixSource([currentEditor]() { return currentEditor()->toPlainText(); });
    connect(m_matrixMenu, &MatrixMenu::matrixChosen, this, [currentEditor](const QString& text) {
        currentEditor()->setPlainText(text);
    });
}

void MainWindow::showAutosaveDialog()
{
    AutosaveDialog dialog(&m_autosave, this);
    connect(&dialog, &AutosaveDialog::entryChosen, this, &MainWindow::applyAutosave);
    dialog.exec();
}

// Поднимает состояние из записи: матрицу, настройки расчёта, накопленный
// спектр и прогресс. Дальше кнопка предлагает продолжить с этого места.
void MainWindow::applyAutosave(const Matrix& matrix, const AutosaveRecord& record)
{
    // Пока идёт расчёт, чужое состояние поднимать нельзя. Воркер считает
    // прежний код и продолжит слать свой спектр и свой прогресс поверх
    // загруженных — на экране получится смесь двух расчётов: подпись от одной
    // записи, цифры от другой. Поэтому сначала остановка, а подстановка —
    // после неё, из handleFinished.
    if (m_runState == RunState::Running || m_runState == RunState::Paused) {
        const auto reply = QMessageBox::question(this,
            tr("Идёт расчёт"),
            tr("Чтобы загрузить сохранение, текущий расчёт придётся остановить.\n"
               "Его состояние сохранится, и продолжить можно будет позже.\n\n"
               "Остановить и загрузить?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;

        m_pendingMatrix   = matrix;
        m_pendingRecord   = record;
        m_pendingAutosave = true;
        on_cancelPBN_clicked();
        return;
    }

    applyAutosaveNow(matrix, record);
}

void MainWindow::applyAutosaveNow(const Matrix& matrix, const AutosaveRecord& record)
{
    // handleMatrixChanged сбрасывает поднятое состояние — он для того и нужен,
    // чтобы ловить правку матрицы руками. Своя подстановка правкой не считается.
    m_applyingAutosave = true;
    if (record.algorithm == ComputationSettings::ProductCode
        && record.productRows1 > 0 && record.productRows1 < matrix.size()) {
        // В записи произведения обе компоненты подряд — по своим панелям.
        m_ui->matrixPTE->setPlainText(matrix.mid(0, record.productRows1).join(QLatin1Char('\n')));
        m_matrix2PTE->setPlainText(matrix.mid(record.productRows1).join(QLatin1Char('\n')));
    } else {
        m_ui->matrixPTE->setPlainText(matrix.join(QLatin1Char('\n')));
    }
    m_applyingAutosave = false;

    // Настройки берутся из записи. Без этого «Продолжить» искал бы сохранение
    // другого алгоритма, не нашёл и молча начал бы с нуля.
    // У произведения в полях записи — свой вес и ранг; диалогу они уходят
    // через те же два числа.
    const bool product = record.algorithm == ComputationSettings::ProductCode;
    int weight = record.bzWeight;
    if (record.algorithm == ComputationSettings::RandomInfoSets) weight = record.leonWeight;
    if (product)                                                  weight = record.productWeight;
    emit applySettingsFromAutosave(int(record.algorithm), int(record.enumType),
                                   product ? record.productRank : record.maxRows, weight,
                                   product ? (record.productMissExponent > 0
                                                  ? int(ComputationSettings::RandomInfoSets)
                                                  : int(ComputationSettings::BrouwerZimmermann))
                                           : 0);

    m_unseenByWeight.clear();
    // Как и по ходу расчёта: Брауэр–Циммерман — только до заказанного веса.
    const int shownUpTo = record.algorithm == ComputationSettings::BrouwerZimmermann
                              ? record.bzWeight : -1;   // -1 — весь спектр записи

    // Спектр показывается сырым — ровно так же, как во время расчёта: у
    // дуального кода преобразование Мак-Вильямс делается только в конце.
    SpectrumCounts spectrum;
    spectrum.counts = record.state.spectrum;
    if (shownUpTo >= 0)
        for (int w = shownUpTo + 1; w < spectrum.size(); ++w)
            spectrum.counts[w] = 0;
    handleSpectrum(spectrum);

    const double total = totalOperations(record, matrix.size());
    const int percent = total > 0.0 ? int(100.0 * double(record.state.doneOps) / total) : 0;
    m_ui->infoPBR->setValue(qBound(0, percent, 100));
    m_statsPanel->showState(tr("Загружено сохранение: перебрано %1 слов")
                             .arg(Format::count(record.state.doneOps)));

    m_runState = RunState::Loaded;
    updateExecuteButton();
}

void MainWindow::updateExecuteButton()
{
    switch (m_runState) {
        case RunState::Running:
            m_ui->executePBN->setText(UiStrings::PAUSE_TEXT);
            m_ui->executePBN->setToolTip(UiStrings::PAUSE_TOOLTIP);
            m_ui->executePBN->setIcon(FluentIcons::icon(this, FluentIcons::PAUSE));
            break;

        case RunState::Paused:
        case RunState::Loaded:
            m_ui->executePBN->setText(UiStrings::CONTINUE_TEXT);
            m_ui->executePBN->setToolTip(UiStrings::CONTINUE_TOOLTIP);
            m_ui->executePBN->setIcon(FluentIcons::icon(this, FluentIcons::PLAY));
            break;

        case RunState::Idle:
            m_ui->executePBN->setText(UiStrings::START_TEXT);
            m_ui->executePBN->setToolTip(UiStrings::START_TOOLTIP);
            m_ui->executePBN->setIcon(FluentIcons::icon(this, FluentIcons::PLAY));
            break;
    }
}

void MainWindow::handleSpectrum(const SpectrumCounts& spectrum)
{
    m_lastSpectrum = spectrum;
    showSpectrumText();
    m_spectrumPlot->setSpectrum(spectrum.plotValues());
}

// Спектр строками «вес - число» для ненулевых весов.
//
// Веса, которых не заказывали, сюда не приходят — воркер режет спектр по
// заказанному весу сам, и помечать на экране нечего.
void MainWindow::showSpectrumText()
{
    // Позиция прокрутки сохраняется: спектр обновляется раз в секунду, и без
    // этого список дёргался бы в начало на каждом обновлении.
    QScrollBar* const bar = m_ui->spectrumPTE->verticalScrollBar();
    const int scroll = bar->value();

    // Числа — с разбивкой по три цифры; копируются они без неё
    // (SpectrumTextEdit).
    QStringList shown;
    for (int w = 0; w < m_lastSpectrum.size(); ++w) {
        if (m_lastSpectrum.counts.at(w) == 0)
            continue;
        QString line = QString::number(w) + QStringLiteral(" - ")
                     + SpectrumTextEdit::grouped(m_lastSpectrum.decimal(w));
        // Случайный поиск: у весов, где по словам, пойманным по одному разу,
        // видно недобор, дописывается оценка — сколько ещё не найдено.
        if (w < m_unseenByWeight.size() && m_unseenByWeight.at(w) >= 0.5f)
            line += tr("   (осталось ≈%1)")
                        .arg(SpectrumTextEdit::grouped(
                            QString::number(qRound64(double(m_unseenByWeight.at(w))))));
        shown.append(line);
    }

    m_ui->spectrumPTE->setPlainText(shown.join(QLatin1Char('\n')));
    bar->setValue(scroll);
}

void MainWindow::setupDocks()
{
    auto makeDock = [this](QWidget* content, const QString& title,
                           const QString& tip, const char* name) {
        QDockWidget* const dock = new QDockWidget(title, this);
        dock->setObjectName(QLatin1String(name));   // без имени Qt не сохранит раскладку
        dock->setWidget(content);
        dock->setToolTip(tip);
        // Свой заголовок вместо системной рамки: без него вытащенная панель
        // получает оформление Windows и красный крестик вместо привычной
        // серой полосы. Подробности в docktitlebar.h.
        dock->setTitleBarWidget(new DockTitleBar(dock));
        return dock;
    };

    m_matrix2PTE   = new FilterPlainTextEdit(this);
    m_matrix2PTE->setFont(m_ui->matrixPTE->font());
    // Как у первой: строка матрицы не переносится, а уходит за край с
    // прокруткой — перенесённая строка нулей и единиц нечитаема.
    m_matrix2PTE->setLineWrapMode(m_ui->matrixPTE->lineWrapMode());
    connect(m_matrix2PTE, &FilterPlainTextEdit::textChanged, this, [this]() { updateMatrixTitles(); });

    m_matrixPages = new QStackedWidget(this);
    m_matrixPages->addWidget(m_ui->matrixPTE);
    m_matrixPages->addWidget(m_matrix2PTE);
    m_matrixDock   = makeDock(m_matrixPages, tr("Матрица"),
                            UiStrings::MATRIX_TOOLTIP,   "matrixDock");

    // Вкладки — в заголовке панели, в одной строке с её кнопками.
    m_matrixTabBar = new QTabBar;
    m_matrixTabBar->addTab(tr("Матрица 1"));
    m_matrixTabBar->addTab(tr("Матрица 2"));
    m_matrixTabBar->setTabToolTip(1, UiStrings::MATRIX2_TOOLTIP);
    connect(m_matrixTabBar, &QTabBar::currentChanged, m_matrixPages, &QStackedWidget::setCurrentIndex);
    static_cast<DockTitleBar*>(m_matrixDock->titleBarWidget())->setTabBar(m_matrixTabBar);

    // На стыке вкладок — кнопка «поменять местами»: компоненты произведения
    // легко загрузить не в те вкладки. На спектр порядок не влияет.
    auto* const swap = new TabSwapButton(m_matrixTabBar, tr("Поменять матрицы местами"));
    connect(swap, &TabSwapButton::clicked, this, [this]() {
        const QString first  = m_ui->matrixPTE->toPlainText();
        const QString second = m_matrix2PTE->toPlainText();
        m_ui->matrixPTE->setPlainText(second);
        m_matrix2PTE->setPlainText(first);
    });
    m_spectrumDock = makeDock(m_ui->spectrumPTE, tr("Спектр кодовых слов"),
                            UiStrings::SPECTRUM_TOOLTIP, "spectrumDock");
    m_plotDock     = makeDock(m_ui->spectrumCPT, tr("График спектра"),
                            UiStrings::PLOT_TOOLTIP,     "plotDock");

    m_statsPanel = new StatsPanel(this);
    m_statsDock  = makeDock(m_statsPanel, tr("Ход расчёта"),
                          UiStrings::STATS_TOOLTIP, "statsDock");

    // Швартуется только первый док; остальные добавляет splitDockWidget. Если
    // добавить все три через addDockWidget, они складываются в одну область
    // стопкой, и последующее деление даёт не то, что нужно.
    addDockWidget(Qt::TopDockWidgetArea, m_matrixDock);
    // Панель показали снова — догоняем всё, что накопилось, пока её не было.
    connect(m_plotDock, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown)
            m_spectrumPlot->refresh();
    });

    // Порядок делений важен. Сначала окно делится по высоте, и только потом
    // верхняя половина — по ширине: иначе панель хода отрезает себе колонку во
    // всю высоту окна и встаёт не рядом с матрицей, а сбоку от всего сразу.
    splitDockWidget(m_matrixDock,   m_spectrumDock, Qt::Vertical);
    splitDockWidget(m_matrixDock,   m_statsDock,    Qt::Horizontal);
    splitDockWidget(m_spectrumDock, m_plotDock,     Qt::Horizontal);

    // Ширины задаются явно. Сам Qt делит место по sizeHint, а у графика он
    // крошечный, у текстовых полей — во всю строку, и график получал узкую
    // полоску у правого края. Числа относительные, Qt подгоняет их под окно;
    // левая колонка шире — матрице и спектру нужна ширина под строки цифр.
    resizeDocks({ m_matrixDock,   m_statsDock }, { 600, 400 }, Qt::Horizontal);
    resizeDocks({ m_spectrumDock, m_plotDock  }, { 600, 400 }, Qt::Horizontal);

    // Fixed, а не Maximum: Maximum разрешает сжаться до нуля, и полоса с
    // кнопками исчезала, отдав всю высоту панелям.
    m_ui->centralwidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_defaultLayout = saveState(Constants::LAYOUT_VERSION);
    updateMatrixTabs();

    // Меню «Вид»: галочки Qt делает сам, они же возвращают закрытую панель.
    m_ui->viewMNU->addAction(m_matrixDock->toggleViewAction());
    m_ui->viewMNU->addAction(m_spectrumDock->toggleViewAction());
    m_ui->viewMNU->addAction(m_plotDock->toggleViewAction());
    m_ui->viewMNU->addAction(m_statsDock->toggleViewAction());
    m_ui->viewMNU->addSeparator();
    m_ui->viewMNU->addAction(UiStrings::VIEW_RESET_TEXT, this, &MainWindow::resetLayout);
}

void MainWindow::updateMatrixTabs()
{
    if (!m_matrixTabBar)
        return;
    const bool product = m_settings.algorithmType == ComputationSettings::ProductCode;
    m_matrixTabBar->setTabVisible(1, product);
    m_matrixTabBar->setVisible(product);
    if (!product) {
        m_matrixTabBar->setCurrentIndex(0);
        m_matrixPages->setCurrentIndex(0);
    }
    updateMatrixTitles();
}

void MainWindow::resetLayout()
{
    restoreState(m_defaultLayout, Constants::LAYOUT_VERSION);

    // restoreState возвращает положение, но закрытую панель не открывает.
    m_matrixDock->show();
    m_spectrumDock->show();
    m_plotDock->show();
}

void MainWindow::setToolTips() {
    m_ui->cancelPBN->setToolTip(UiStrings::CANCEL_TOOLTIP);
    // Квадрат остановки у отмены не меняется, поэтому ставится один раз.
    m_ui->cancelPBN->setIcon(FluentIcons::icon(this, FluentIcons::STOP));
    updateExecuteButton();
    m_ui->exitPBN->setToolTip(UiStrings::EXIT_TOOLTIP);
    m_ui->exitPBN->setIcon(FluentIcons::icon(this, FluentIcons::EXIT));
}

void MainWindow::setWorker()
{
    m_workerPtr = new Worker;
    m_workerThreadPtr = new QThread(this);
    if (!m_workerPtr) return;

    m_workerPtr->moveToThread(m_workerThreadPtr);

    connect( m_workerPtr,       &Worker::updateInfoPBR,                  this,      &MainWindow::handleUpdateInfoPBR,                  Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::spectrumUpdated,                this,      &MainWindow::handleSpectrum,                       Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::updateRemainingMinutes,         this,      &MainWindow::handleUpdateRemainingMinutes,         Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::errorOccurred,                  this,      &MainWindow::handleError,                          Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::finished,                       this,      &MainWindow::handleFinished,                       Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::showSaveLBL,                    this,      &MainWindow::showSaveLBL,                          Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::gridTuned,                      this,      &MainWindow::handleGridTuned,                      Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::planReady,                      this,      &MainWindow::handlePlanReady,                      Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::searchEstimate,                 this,      &MainWindow::handleSearchEstimate,                 Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::productPlan,                    this,      &MainWindow::handleProductPlan,                    Qt::QueuedConnection );
    connect( m_workerPtr,       &Worker::updateRateMeasured,             m_settingsDialog, &SettingsDialog::applyMeasuredRate,            Qt::QueuedConnection );
    // Проба останавливается тем же способом, которым пользователь останавливает
    // расчёт. Отсчёт начинается по сигналу воркера, а не с самой просьбы: перед
    // замером может пройти подбор сетки, и он занимает секунды.
    connect( m_workerPtr, &Worker::updateRateProbeStarted, this, [this]() {
        QTimer::singleShot(Constants::PROBE_DURATION_MS, this, [this]() {
            if (m_workerPtr)
                m_workerPtr->cancel();
        });
    }, Qt::QueuedConnection );

    connect( this,            &MainWindow::sendSettingsToWorker,       m_workerPtr, &Worker::setSettings,                           Qt::QueuedConnection );
}

void MainWindow::connectSettingsDialog()
{
    if (!m_settingsDialog) return;


    connect( this,     &MainWindow::matrixChanged,            m_settingsDialog, &SettingsDialog::handleMatrixChanged     );
    connect( this,     &MainWindow::setInterfaceEnabled,      m_settingsDialog, &SettingsDialog::setInterfaceEnabled     );
    connect( this,     &MainWindow::requestSettings,          m_settingsDialog, &SettingsDialog::handleSettingsRequested );
    connect( this,     &MainWindow::applySettingsFromAutosave, m_settingsDialog, &SettingsDialog::applyFromAutosave       );

    // Записываем матрицу при получении
    // Замер потолка обновления: короткий расчёт на настройках, которые сейчас
    // выставлены в диалоге, — не на тех, что подтверждены кнопкой.
    connect( m_settingsDialog, &SettingsDialog::measureUpdateRateRequested,
        this, [this]( const ComputationSettings& requested ) {
            if (!m_workerPtr)
                return;
            // Без матрицы пробе не с чем работать, а описание задачи на пустой
            // матрице лезет за её первую строку.
            const QString error = matrixError();
            if (!error.isEmpty()) {
                QMessageBox::warning(this, UiStrings::ERROR_TITLE, error);
                m_settingsDialog->applyMeasuredRate(0.0);
                return;
            }

            ComputationSettings probe = requested;
            probe.matrix  = m_ui->matrixPTE->toStringList();
            probe.matrix2 = m_matrix2PTE->toStringList();

            m_workerThreadPtr->start();
            QMetaObject::invokeMethod(m_workerPtr, "setSettings", Qt::QueuedConnection,
                                      Q_ARG(ComputationSettings, probe));
            QMetaObject::invokeMethod(m_workerPtr, "measureUpdateRate", Qt::QueuedConnection);
        });

    connect( m_settingsDialog, &SettingsDialog::sendSettingsToWidget,
        this, [this]( const ComputationSettings& fromDialog ) {
            const ComputationSettings::Algorithm before = m_settings.algorithmType;
            m_settings = fromDialog;
            m_settings.matrix  = m_ui->matrixPTE->toStringList();
            m_settings.matrix2 = m_matrix2PTE->toStringList();
            m_spectrumPlot->setMaxBars(m_settings.maxPlotBars);
            updateMatrixTabs();

            // Поднятая запись — про свой алгоритм. Сменили алгоритм — кнопка
            // «Продолжить» больше не про неё: иначе расчёт стартовал бы как
            // продолжение и тащил бы за собой состояние прежнего показа.
            if (m_runState == RunState::Loaded && m_settings.algorithmType != before) {
                m_runState = RunState::Idle;
                updateExecuteButton();
            }
            emit sendSettingsToWorker(m_settings);

            // Идущему расчёту настройки через очередь не доходят: воркер до
            // самого конца не возвращается в свой цикл событий. Живые интервалы
            // передаются напрямую.
            if (m_workerPtr && (m_runState == RunState::Running || m_runState == RunState::Paused))
                m_workerPtr->setLiveIntervals(m_settings.timeIntSet.updateSpectrumInterval,
                                            m_settings.timeIntSet.saveSpectrumInterval);
        });
}


void MainWindow::on_executePBN_clicked()
{
    // Одна и та же кнопка запускает, ставит на паузу и продолжает расчёт.
    switch (m_runState) {
        case RunState::Idle:
        case RunState::Loaded:  startComputation();  break;
        case RunState::Running: pauseComputation();  break;
        case RunState::Paused:  resumeComputation(); break;
    }
}

// Проверяет матрицу перед запуском. Пустая строка — всё в порядке.
QString MainWindow::matrixError() const
{
    auto check = [this](const Matrix& rows, const QString& who) -> QString {
        if (rows.isEmpty())
            return tr("%1 пустая").arg(who);

        if (quint64(rows.size()) > Constants::MAX_ROWS)
            return tr("%1: число строк больше чем %2").arg(who).arg(Constants::MAX_ROWS);

        const int cols = rows.first().length();
        for (const QString& row : rows) {
            if (row.length() != cols)
                return tr("%1: все строки должны быть одинаковой длины").arg(who);
        }

        if (quint64(cols) > Constants::MAX_COLS)
            return tr("%1: число столбцов больше чем %2").arg(who).arg(Constants::MAX_COLS);
        return QString();
    };

    const QString first = check(m_ui->matrixPTE->toStringList(),
                                m_settings.algorithmType == ComputationSettings::ProductCode
                                    ? tr("Матрица 1") : tr("Матрица"));
    if (!first.isEmpty())
        return first;
    // Код произведения: компоненты проверяются каждая сама по себе, само
    // произведение в памяти не строится, и его размер ничем не ограничен.
    if (m_settings.algorithmType == ComputationSettings::ProductCode)
        return check(m_matrix2PTE->toStringList(), tr("Матрица 2 (вторая компонента)"));
    return QString();
}

void MainWindow::startComputation()
{
    const bool resuming = m_runState == RunState::Loaded;

    if (!m_workerPtr) {
        QMessageBox::warning(this, UiStrings::ERROR_TITLE, tr("Worker не подключён"));
        return;
    }

    // Проверка идёт до блокировки интерфейса: иначе при ошибке в матрице он
    // успевал погаснуть и тут же зажечься, а в строке состояния оставалось
    // «Готово» о расчёте, которого не было.
    const QString error = matrixError();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, UiStrings::ERROR_TITLE, error);
        return;
    }

    emit setInterfaceEnabled(false);
    m_ui->matrixPTE->setReadOnly(true);
    m_matrix2PTE->setReadOnly(true);
    m_ui->cancelPBN->setEnabled(true);
    m_matrixMenu->setActionsEnabled(false);
    m_statsPanel->showState(tr("Идёт расчёт"));
    m_ui->infoPBR->setValue(0);

    // Поток нужен уже сейчас: настройки уходят воркеру через очередь событий.
    m_workerThreadPtr->start();
    emit requestSettings();

    // Запись подняли из диалога — пользователь уже сказал, что продолжает,
    // и спрашивать второй раз незачем.
    LoadMode mode = LoadMode::Reset;
    if (resuming) {
        mode = LoadMode::FromCheckpoint;
    }
    else if (hasCheckpoint()) {
        const auto reply = QMessageBox::question(this,
            tr("Найден спектр"),
            tr("Для текущих настроек обнаружен сохранённый спектр\n"
               "Продолжить вычисление с сохранённого состояния?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes)
            mode = LoadMode::FromCheckpoint;
    }

    // Режим задаётся всегда, а не только при найденном сохранении: иначе
    // воркер начинал бы с того состояния, что осталось от прошлого запуска.
    QMetaObject::invokeMethod(m_workerPtr, "initializeRunState", Qt::QueuedConnection,
                              Q_ARG(LoadMode, mode));

    // Настройки к этому моменту уже пришли от диалога по requestSettings.
    m_statsPanel->showTask(m_settings);
    m_statsPanel->clearProgress();
    m_unseenByWeight.clear();

    QMetaObject::invokeMethod(m_workerPtr, "computeSpectrum", Qt::QueuedConnection);

    m_runState = RunState::Running;
    updateExecuteButton();
}

void MainWindow::pauseComputation()
{
    if (m_workerPtr)
        m_workerPtr->pause();

    m_runState = RunState::Paused;
    setWindowTitle(UiStrings::PAUSE_TEXT);
    m_statsPanel->showState(tr("Пауза"));
    updateExecuteButton();
}

void MainWindow::resumeComputation()
{
    if (m_workerPtr)
        m_workerPtr->resume();

    m_runState = RunState::Running;
    m_statsPanel->showState(tr("Идёт расчёт"));
    updateExecuteButton();

    // Оценка времени с прошлого запуска ещё актуальна — возвращаем её
    // в заголовок вместо «Пауза».
    setWindowTitle(m_remainingMinutes != -1 ? Format::remainingTime(m_remainingMinutes)
                                          : UiStrings::MAIN_TITLE);
}

void MainWindow::on_exitPBN_clicked()
{
    if( m_workerPtr ){
        m_workerPtr->cancel();
    }
    if( m_workerThreadPtr ){
        m_workerThreadPtr->quit();
        m_workerThreadPtr->wait();
    }
    
    emit setInterfaceEnabled(   true  );
    saveSettings();
    m_ui->matrixPTE->setReadOnly( false );
    m_matrix2PTE->setReadOnly( false );
    qApp->exit();
}

void MainWindow::on_cancelPBN_clicked()
{
    if (m_workerPtr) {
        m_workerPtr->cancel();
    }
    if (m_workerThreadPtr) {
        m_workerThreadPtr->quit();
        m_workerThreadPtr->wait();
    }

    emit setInterfaceEnabled(   true  );
    m_ui->matrixPTE->setReadOnly( false );
    m_matrix2PTE->setReadOnly( false );
    m_ui->cancelPBN->setEnabled(  false );
}

//
// Worker signal handlers
//

void MainWindow::handleUpdateInfoPBR(int percent)
{
    // Обновляем прогрессбар в ui
    m_ui->infoPBR->setValue(percent);

    m_taskbar->setPercent(percent);
}

// Сетку показываем: иначе при включённом автоподборе непонятно, на чём
// программа в итоге считает и почему время отличается от прошлого запуска.
void MainWindow::handleGridTuned(int blocks, int threads)
{
    m_statsPanel->showGrid(blocks, threads);
}

void MainWindow::handlePlanReady(int sets, int rows, int exactUpToWeight)
{
    // Строки «Гарантия» в панели больше нет: план виден в записи расчёта и
    // в тестовом харнессе, на экране от него просили только вес.
    Q_UNUSED(sets); Q_UNUSED(rows); Q_UNUSED(exactUpToWeight);
}

void MainWindow::handleSearchEstimate(int weight, quint64 trialsDone, quint64 trialsTotal,
                                      double missProbability, SpectrumFloat unseenByWeight)
{
    Q_UNUSED(weight); Q_UNUSED(trialsDone); Q_UNUSED(trialsTotal); Q_UNUSED(missProbability);
    this->m_unseenByWeight = unseenByWeight;
    showSpectrumText();
}

void MainWindow::handleProductPlan(const QString& text, int exactUpToWeight)
{
    // Что сейчас считается — в строке состояния: другой строки под это нет.
    m_statsPanel->showState(text);
    Q_UNUSED(exactUpToWeight);
}

void MainWindow::handleUpdateRemainingMinutes(int elapsedSec, int minutesLeft, double speed,
                                              quint64 doneOps, quint64 totalOps)
{
    m_remainingMinutes = minutesLeft;

    const QString elapsedStr = Format::duration(elapsedSec);

    m_statsPanel->showProgress(elapsedSec, minutesLeft, speed, doneOps, totalOps);

    // Имя программы в заголовке остаётся: раньше он превращался просто в
    // "2 ч 15 мин", и в панели задач было непонятно, что это за окно.
    this->setWindowTitle(tr("%1 — осталось %2")
                             .arg(UiStrings::MAIN_TITLE, Format::remainingTime(m_remainingMinutes)));
}
void MainWindow::showSaveLBL()
{
    m_ui->saveLBL->setToolTip(UiStrings::SAVE_LBL_ICON_TOOLTIP);
    QPropertyAnimation* anim = new QPropertyAnimation(m_saveLBLOpacityEffect, "opacity", this);
    if (m_ui->saveLBL->underMouse()) {
        QToolTip::showText(QCursor::pos(), UiStrings::SAVE_LBL_ICON_TOOLTIP, m_ui->saveLBL);
    }

    anim->setDuration(1000); // общая длительность
    anim->setStartValue(0.0);
    anim->setKeyValueAt(0.5, 1.0); // середина — полностью видно
    anim->setEndValue(0.0);
    connect(anim, &QPropertyAnimation::finished, this, [this]() {
        // Возвращаем базовый tooltip
        m_ui->saveLBL->setToolTip(UiStrings::SAVE_LBL_BASE_TOOLTIP);
        if (m_ui->saveLBL->underMouse()) {
            QToolTip::showText(QCursor::pos(), UiStrings::SAVE_LBL_BASE_TOOLTIP, m_ui->saveLBL);
        }
        });

    anim->start(QAbstractAnimation::DeleteWhenStopped);
}
void MainWindow::handleMatrixChanged()
{
    // Матрицу правят руками — поднятое сохранение к ней больше не относится.
    if (m_runState == RunState::Loaded && !m_applyingAutosave) {
        m_runState = RunState::Idle;
        updateExecuteButton();
    }

    Matrix rows = m_ui->matrixPTE->toStringList();
    int maxLen = 0;
    for (const QString& row : rows)
        maxLen = qMax(maxLen, row.length());
    // Размеры матрицы изменились — слот диалога настроек пересчитает и отправит настройки
    if (rows.size() != 0 && maxLen != 0)
        emit matrixChanged( rows.size(), maxLen );
    // Размеры показывает заголовок дока — отдельной подписи над редактором
    // больше нет.
    updateMatrixTitles();
}

// «Матрица (n,k)» у произвольного кода; у произведения панель зовётся
// «Матрицы», а размеры — на вкладках.
void MainWindow::updateMatrixTitles()
{
    if (!m_matrixDock || !m_matrixTabBar)
        return;
    auto size = [](const Matrix& rows) {
        int maxLen = 0;
        for (const QString& row : rows) maxLen = qMax(maxLen, row.length());
        return QStringLiteral(" (%1,%2)").arg(maxLen).arg(rows.size());
    };
    const bool product = m_settings.algorithmType == ComputationSettings::ProductCode;
    m_matrixDock->setWindowTitle(product ? tr("Матрицы")
                                       : tr("Матрица") + size(m_ui->matrixPTE->toStringList()));
    m_matrixTabBar->setTabText(0, tr("Матрица 1") + size(m_ui->matrixPTE->toStringList()));
    m_matrixTabBar->setTabText(1, tr("Матрица 2") + size(m_matrix2PTE->toStringList()));
}
void MainWindow::handleError(const QString& message)
{
    QMessageBox::critical(this, UiStrings::ERROR_TITLE, message);
    // reset UI
    m_runState = RunState::Idle;
    updateExecuteButton();
}

void MainWindow::handleFinished(int elapsedSec)
{
    m_runState = RunState::Idle;
    
    if(m_workerThreadPtr->isRunning()){
        m_workerThreadPtr->quit();
        m_workerThreadPtr->wait();
    }
    m_workerPtr->resume();
    emit setInterfaceEnabled(   true  );
    m_ui->matrixPTE->setReadOnly( false );
    m_matrix2PTE->setReadOnly( false );
    m_ui->cancelPBN->setEnabled(  false );
    m_matrixMenu->setActionsEnabled(true);

    updateExecuteButton();
    this->setWindowTitle( UiStrings::MAIN_TITLE  );
    if ( m_workerPtr->isCancelled() ) {
        m_workerPtr->uncancel();
        // Расчёт останавливали ради загрузки сохранения — вот теперь можно.
        if (m_pendingAutosave) {
            m_pendingAutosave = false;
            applyAutosaveNow(m_pendingMatrix, m_pendingRecord);
            return;
        }
        m_statsPanel->showState( UiStrings::CANCEL_TEXT );
        return;
    }
    // Меньше секунды — «0 с» выглядело бы как сбой замера.
    const QString elapsedStr = elapsedSec > 0 ? Format::duration(elapsedSec)
                                              : tr("< 1 с");
    m_statsPanel->showState( UiStrings::READY_TEXT + elapsedStr );

    // Расчёт успел добежать до конца, пока пользователь выбирал запись.
    if (m_pendingAutosave) {
        m_pendingAutosave = false;
        applyAutosaveNow(m_pendingMatrix, m_pendingRecord);
    }
}



bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // При изменение размера обновляем подписи под графиком
    if (watched == m_ui->spectrumCPT && event->type() == QEvent::Resize) {
        // Не перерисовываем сразу: пока панель тащат, размер меняется
        // непрерывно. Таймер сбрасывается на каждом событии и срабатывает
        // один раз, когда размер устоялся.
        m_plotRefreshTimer->start();
        return false;
    }
    // Для всех остальных событий — стандартная обработка
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::hasCheckpoint() const
{
    AutosaveRecord record;
    const Matrix key = m_settings.algorithmType == ComputationSettings::ProductCode
                           ? m_settings.matrix + m_settings.matrix2 : m_settings.matrix;
    if (!m_autosave.load(key, m_settings.algorithmType, record))
        return false;

    // Запись может оказаться непригодной: она ушла дальше, чем просят сейчас.
    return canResume(record, m_settings);
}

// Применить настройки
void MainWindow::applySettings()
{
    m_spectrumPlot->setMaxBars(m_settings.maxPlotBars);

    // Точка, где настройки из диалога попадают в интерфейс. Сейчас
    // единственное, что сюда просилось, — цвет и прозрачность столбцов,
    // но эти поля в диалоге пока не подключены.
}

// Сохранить настройки в реестр
void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(SettingsKeys::WINDOW_STATE,    this->saveState(Constants::LAYOUT_VERSION));
    s.setValue(SettingsKeys::CODE_MATRIX,     m_ui->matrixPTE->toPlainText()   );
    s.setValue(SettingsKeys::CODE_MATRIX2,    m_matrix2PTE->toPlainText()      );
    s.setValue(SettingsKeys::SPECTRUM_TEXT,   m_lastSpectrum.lines().join(QLatin1Char('\n')) );
    s.setValue(SettingsKeys::WIDGET_GEOMETRY, this->saveGeometry()           );
    QVariantList values;
    for (double v : m_spectrumPlot->values())
        values << v;
    s.setValue(SettingsKeys::SPECTRUM_VALUES, values);
    s.sync();
}

// Загрузить настройки из реестра
void MainWindow::loadSettings()
{
    QSettings s;

    this->restoreGeometry(                    s.value(SettingsKeys::WIDGET_GEOMETRY                ).toByteArray()       );
    // Номер раскладки: при изменении набора панелей restoreState вернёт false
    // и останется та, что собрана по умолчанию, а не каша от прошлой версии.
    restoreState(s.value(SettingsKeys::WINDOW_STATE).toByteArray(), Constants::LAYOUT_VERSION);
    // Спектр хранится строками «вес - число» — как и в прежних версиях.
    m_lastSpectrum = SpectrumCounts::fromLines(s.value(SettingsKeys::SPECTRUM_TEXT).toString()
                                                 .split(QLatin1Char('\n'), Qt::SkipEmptyParts));
    showSpectrumText();
    m_ui->matrixPTE->setPlainText(              s.value(SettingsKeys::CODE_MATRIX                    ).toString()          );
    m_matrix2PTE->setPlainText(                 s.value(SettingsKeys::CODE_MATRIX2                   ).toString()          );
    if (s.contains(SettingsKeys::SPECTRUM_VALUES)) {
        QVariantList values = s.value(SettingsKeys::SPECTRUM_VALUES).toList();
        SpectrumFloat spectrum;
        spectrum.reserve(values.size());
        for (const QVariant &v : values)
            spectrum.append(v.toFloat());
        m_spectrumPlot->setSpectrum(spectrum);
    }
}
