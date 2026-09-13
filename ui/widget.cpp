#include "widget.h"
#include "infosets.h"
#include "filterplaintextedit.h"
#include "tabswapbutton.h"

#include <QStackedWidget>
#include <QTabBar>
#include "docktitlebar.h"
#include "fonticons.h"

#include <QHeaderView>
#include <QApplication>
#include <QDockWidget>
#include "format.h"
#include "matrixlibrary.h"
#include "autosavedialog.h"
#include "format.h"
#include "matrixmenu.h"
#include "spectrumplot.h"
#include "statspanel.h"
#include "ui_widget.h"





MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    /********      УБРАТЬ В UI     ********/
        saveLBLOpacityEffect = new QGraphicsOpacityEffect(ui->saveLBL);
        ui->saveLBL->setGraphicsEffect(saveLBLOpacityEffect);
        ui->saveLBL->setToolTip("В момент сохранения спектра тут появится значок");
        saveLBLOpacityEffect->setOpacity(0.0);
    /********                      ********/
    if( settingsDialog == nullptr )
        settingsDialog = new SettingsDialog(this);
    spectrumPlot = std::make_unique<SpectrumPlot>(ui->spectrumCPT);
    connect( ui->matrixPTE, &FilterPlainTextEdit::textChanged, this,  &MainWindow::handleMatrixChanged    );
    connect(ui->autosaveACN, &QAction::triggered,
        this, &MainWindow::showAutosaveDialog);
    connect(ui->settingsACN, &QAction::triggered,
        this, [this]() {
            settingsDialog->exec();
            applySettings();
        });


    setupDocks();
    loadSettings();
    plotRefreshTimer = new QTimer(this);
    plotRefreshTimer->setSingleShot(true);
    plotRefreshTimer->setInterval(Constants::PLOT_REFRESH_DELAY_MS);
    connect(plotRefreshTimer, &QTimer::timeout, this, [this]() {
        // Пока кнопка мыши зажата, панель ещё тащат. Перерисовка на этом
        // месте переразмечает окно, и разделитель теряет захват мыши — со
        // стороны это выглядит как «тянется через раз». Ждём дальше.
        if (QApplication::mouseButtons() != Qt::NoButton) {
            plotRefreshTimer->start();
            return;
        }
        spectrumPlot->refresh();
    });

    ui->spectrumCPT->installEventFilter(this);
    connectSettingsDialog();
    setWorker();
    setMatrixMenu();
    setToolTips();
    // Настройки диалога нужны окну сразу: от алгоритма зависит, показывать ли
    // панель второй матрицы. Пустая матрица сигнала о смене не даёт.
    emit requestSettings();
    emit handleMatrixChanged();
    // Старые чекпоинты лежали в реестре, по мегабайту с матрицей на запись.
    // Переносим их в файлы один раз и вычищаем ветку.
    autosave.migrateFromRegistry();

    taskbar = std::make_unique<TaskbarProgress>(this);
}

MainWindow::~MainWindow()
{
    saveSettings();
    if (workerPtr){
        workerPtr->cancel();
    }
    if(workerThreadPtr->isRunning()){
        workerThreadPtr->quit();
        workerThreadPtr->wait();
    }
    if(workerPtr){
        delete workerPtr;
        workerPtr = nullptr;
    }
    this->setWindowTitle(UIStrings::MAIN_TITLE);
    delete ui;
}

void MainWindow::setMatrixMenu()
{

    matrixMenu = new MatrixMenu(ui->matrixMNU, ui->deleteMatrixMNU, ui->addMatrixACN, this);
    matrixMenu->setMatrixSource([this]() { return ui->matrixPTE->toPlainText(); });
    connect(matrixMenu, &MatrixMenu::matrixChosen, this, [this](const QString& text) {
        ui->matrixPTE->setPlainText(text);
    });
}

void MainWindow::showAutosaveDialog()
{
    AutosaveDialog dialog(&autosave, this);
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
    if (runState == RunState::Running || runState == RunState::Paused) {
        const auto reply = QMessageBox::question(this,
            tr("Идёт расчёт"),
            tr("Чтобы загрузить сохранение, текущий расчёт придётся остановить.\n"
               "Его состояние сохранится, и продолжить можно будет позже.\n\n"
               "Остановить и загрузить?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;

        pendingMatrix   = matrix;
        pendingRecord   = record;
        pendingAutosave = true;
        on_cancelPBN_clicked();
        return;
    }

    applyAutosaveNow(matrix, record);
}

void MainWindow::applyAutosaveNow(const Matrix& matrix, const AutosaveRecord& record)
{
    // handleMatrixChanged сбрасывает поднятое состояние — он для того и нужен,
    // чтобы ловить правку матрицы руками. Своя подстановка правкой не считается.
    applyingAutosave = true;
    if (record.algorithm == ComputationSettings::ProductCode
        && record.productRows1 > 0 && record.productRows1 < matrix.size()) {
        // В записи произведения обе компоненты подряд — по своим панелям.
        ui->matrixPTE->setPlainText(matrix.mid(0, record.productRows1).join(QLatin1Char('\n')));
        matrix2PTE->setPlainText(matrix.mid(record.productRows1).join(QLatin1Char('\n')));
    } else {
        ui->matrixPTE->setPlainText(matrix.join(QLatin1Char('\n')));
    }
    applyingAutosave = false;

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

    unseenByWeight.clear();
    // Как и по ходу расчёта: Брауэр–Циммерман — только до заказанного веса.
    const int shownUpTo = record.algorithm == ComputationSettings::BrouwerZimmermann
                              ? record.bzWeight : -1;   // -1 — весь спектр записи

    // Спектр показывается сырым — ровно так же, как во время расчёта: у
    // дуального кода преобразование Мак-Вильямс делается только в конце.
    SpectrumFloat plot;
    SpectrumText  text;
    for (int w = 0; w < record.state.spectrum.size(); ++w) {
        const quint64 value = shownUpTo >= 0 && w > shownUpTo ? 0 : record.state.spectrum.at(w);
        plot.append(float(value));
        if (value != 0)
            text.append(QString::number(w) + " - " + QString::number(value));
    }
    handleUpdateSpectrumPlot(plot);
    handleUpdateSpectrumPTE(text);

    const double total = totalOperations(record, matrix.size());
    const int percent = total > 0.0 ? int(100.0 * double(record.state.doneOps) / total) : 0;
    ui->infoPBR->setValue(qBound(0, percent, 100));
    statsPanel->showState(tr("Загружено сохранение: перебрано %1 слов")
                             .arg(Format::count(record.state.doneOps)));

    runState = RunState::Loaded;
    updateExecuteButton();
}

void MainWindow::updateExecuteButton()
{
    switch (runState) {
        case RunState::Running:
            ui->executePBN->setText(UIStrings::PAUSE_TEXT);
            ui->executePBN->setToolTip(UIStrings::PAUSE_TOOLTIP);
            ui->executePBN->setIcon(FluentIcons::icon(this, FluentIcons::PAUSE));
            break;

        case RunState::Paused:
        case RunState::Loaded:
            ui->executePBN->setText(UIStrings::CONTINUE_TEXT);
            ui->executePBN->setToolTip(UIStrings::CONTINUE_TOOLTIP);
            ui->executePBN->setIcon(FluentIcons::icon(this, FluentIcons::PLAY));
            break;

        case RunState::Idle:
            ui->executePBN->setText(UIStrings::START_TEXT);
            ui->executePBN->setToolTip(UIStrings::START_TOOLTIP);
            ui->executePBN->setIcon(FluentIcons::icon(this, FluentIcons::PLAY));
            break;
    }
}

// Спектр выводится так же, как его присылает воркер: строками «вес - число».
//
// Веса, которых не заказывали, сюда не приходят — воркер режет спектр по
// заказанному весу сам, и помечать на экране нечего.
void MainWindow::setSpectrumRows(const SpectrumText& lines)
{
    lastSpectrum = lines;

    // Позиция прокрутки сохраняется: спектр обновляется раз в секунду, и без
    // этого список дёргался бы в начало на каждом обновлении.
    QScrollBar* const bar = ui->spectrumPTE->verticalScrollBar();
    const int scroll = bar->value();

    SpectrumText shown = lines;
    // Случайный поиск: у весов, где по словам, пойманным по одному разу,
    // видно недобор, дописывается оценка — сколько ещё не найдено.
    if (!unseenByWeight.isEmpty()) {
        for (QString& line : shown) {
            const int weight = line.section(QStringLiteral(" - "), 0, 0).toInt();
            if (weight >= 0 && weight < unseenByWeight.size()
                && unseenByWeight.at(weight) >= 0.5f)
                line += tr("   (осталось ≈%1)")
                            .arg(qRound64(double(unseenByWeight.at(weight))));
        }
    }

    ui->spectrumPTE->setPlainText(shown.join(QLatin1Char('\n')));
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

    matrix2PTE   = new FilterPlainTextEdit(this);
    matrix2PTE->setFont(ui->matrixPTE->font());
    // Как у первой: строка матрицы не переносится, а уходит за край с
    // прокруткой — перенесённая строка нулей и единиц нечитаема.
    matrix2PTE->setLineWrapMode(ui->matrixPTE->lineWrapMode());
    connect(matrix2PTE, &FilterPlainTextEdit::textChanged, this, [this]() { updateMatrixTitles(); });

    matrixPages = new QStackedWidget(this);
    matrixPages->addWidget(ui->matrixPTE);
    matrixPages->addWidget(matrix2PTE);
    matrixDock   = makeDock(matrixPages, tr("Матрица"),
                            UIStrings::MATRIX_TOOLTIP,   "matrixDock");

    // Вкладки — в заголовке панели, в одной строке с её кнопками.
    matrixTabBar = new QTabBar;
    matrixTabBar->addTab(tr("Матрица 1"));
    matrixTabBar->addTab(tr("Матрица 2"));
    matrixTabBar->setTabToolTip(1, UIStrings::MATRIX2_TOOLTIP);
    connect(matrixTabBar, &QTabBar::currentChanged, matrixPages, &QStackedWidget::setCurrentIndex);
    static_cast<DockTitleBar*>(matrixDock->titleBarWidget())->setTabBar(matrixTabBar);

    // На стыке вкладок — кнопка «поменять местами»: компоненты произведения
    // легко загрузить не в те вкладки. На спектр порядок не влияет.
    auto* const swap = new TabSwapButton(matrixTabBar, tr("Поменять матрицы местами"));
    connect(swap, &TabSwapButton::clicked, this, [this]() {
        const QString first  = ui->matrixPTE->toPlainText();
        const QString second = matrix2PTE->toPlainText();
        ui->matrixPTE->setPlainText(second);
        matrix2PTE->setPlainText(first);
    });
    spectrumDock = makeDock(ui->spectrumPTE, tr("Спектр кодовых слов"),
                            UIStrings::SPECTRUM_TOOLTIP, "spectrumDock");
    plotDock     = makeDock(ui->spectrumCPT, tr("График спектра"),
                            UIStrings::PLOT_TOOLTIP,     "plotDock");

    statsPanel = new StatsPanel(this);
    statsDock  = makeDock(statsPanel, tr("Ход расчёта"),
                          UIStrings::STATS_TOOLTIP, "statsDock");

    // Швартуется только первый док; остальные добавляет splitDockWidget. Если
    // добавить все три через addDockWidget, они складываются в одну область
    // стопкой, и последующее деление даёт не то, что нужно.
    addDockWidget(Qt::TopDockWidgetArea, matrixDock);
    // Панель показали снова — догоняем всё, что накопилось, пока её не было.
    connect(plotDock, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown)
            spectrumPlot->refresh();
    });

    // Порядок делений важен. Сначала окно делится по высоте, и только потом
    // верхняя половина — по ширине: иначе панель хода отрезает себе колонку во
    // всю высоту окна и встаёт не рядом с матрицей, а сбоку от всего сразу.
    splitDockWidget(matrixDock,   spectrumDock, Qt::Vertical);
    splitDockWidget(matrixDock,   statsDock,    Qt::Horizontal);
    splitDockWidget(spectrumDock, plotDock,     Qt::Horizontal);

    // Ширины задаются явно. Сам Qt делит место по sizeHint, а у графика он
    // крошечный, у текстовых полей — во всю строку, и график получал узкую
    // полоску у правого края. Числа относительные, Qt подгоняет их под окно;
    // левая колонка шире — матрице и спектру нужна ширина под строки цифр.
    resizeDocks({ matrixDock,   statsDock }, { 600, 400 }, Qt::Horizontal);
    resizeDocks({ spectrumDock, plotDock  }, { 600, 400 }, Qt::Horizontal);

    // Fixed, а не Maximum: Maximum разрешает сжаться до нуля, и полоса с
    // кнопками исчезала, отдав всю высоту панелям.
    ui->centralwidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    defaultLayout = saveState(Constants::LAYOUT_VERSION);
    updateMatrixTabs();

    // Меню «Вид»: галочки Qt делает сам, они же возвращают закрытую панель.
    ui->viewMNU->addAction(matrixDock->toggleViewAction());
    ui->viewMNU->addAction(spectrumDock->toggleViewAction());
    ui->viewMNU->addAction(plotDock->toggleViewAction());
    ui->viewMNU->addAction(statsDock->toggleViewAction());
    ui->viewMNU->addSeparator();
    ui->viewMNU->addAction(UIStrings::VIEW_RESET_TEXT, this, &MainWindow::resetLayout);
}

void MainWindow::updateMatrixTabs()
{
    if (!matrixTabBar)
        return;
    const bool product = settings.algorithmType == ComputationSettings::ProductCode;
    matrixTabBar->setTabVisible(1, product);
    matrixTabBar->setVisible(product);
    if (!product) {
        matrixTabBar->setCurrentIndex(0);
        matrixPages->setCurrentIndex(0);
    }
    updateMatrixTitles();
}

void MainWindow::resetLayout()
{
    restoreState(defaultLayout, Constants::LAYOUT_VERSION);

    // restoreState возвращает положение, но закрытую панель не открывает.
    matrixDock->show();
    spectrumDock->show();
    plotDock->show();
}

void MainWindow::setToolTips() {
    ui->cancelPBN->setToolTip(UIStrings::CANCEL_TOOLTIP);
    // Квадрат остановки у отмены не меняется, поэтому ставится один раз.
    ui->cancelPBN->setIcon(FluentIcons::icon(this, FluentIcons::STOP));
    updateExecuteButton();
    ui->exitPBN->setToolTip(UIStrings::EXIT_TOOLTIP);
    ui->exitPBN->setIcon(FluentIcons::icon(this, FluentIcons::EXIT));
}

void MainWindow::setWorker()
{
    workerPtr = new Worker;
    workerThreadPtr = new QThread(this);
    if (!workerPtr) return;

    workerPtr->moveToThread(workerThreadPtr);

    connect( workerPtr,       &Worker::updateInfoPBR,                  this,      &MainWindow::handleUpdateInfoPBR,                  Qt::QueuedConnection );
    connect( workerPtr,       &Worker::updateSpectrumPlot,             this,      &MainWindow::handleUpdateSpectrumPlot,             Qt::QueuedConnection );
    connect( workerPtr,       &Worker::updateSpectrumPTE,              this,      &MainWindow::handleUpdateSpectrumPTE,              Qt::QueuedConnection );
    connect( workerPtr,       &Worker::updateRemainingMinutes,         this,      &MainWindow::handleUpdateRemainingMinutes,         Qt::QueuedConnection );
    connect( workerPtr,       &Worker::errorOccurred,                  this,      &MainWindow::handleError,                          Qt::QueuedConnection );
    connect( workerPtr,       &Worker::finished,                       this,      &MainWindow::handleFinished,                       Qt::QueuedConnection );
    connect( workerPtr,       &Worker::showSaveLBL,                    this,      &MainWindow::showSaveLBL,                          Qt::QueuedConnection );
    connect( workerPtr,       &Worker::gridTuned,                      this,      &MainWindow::handleGridTuned,                      Qt::QueuedConnection );
    connect( workerPtr,       &Worker::planReady,                      this,      &MainWindow::handlePlanReady,                      Qt::QueuedConnection );
    connect( workerPtr,       &Worker::searchEstimate,                 this,      &MainWindow::handleSearchEstimate,                 Qt::QueuedConnection );
    connect( workerPtr,       &Worker::productPlan,                    this,      &MainWindow::handleProductPlan,                    Qt::QueuedConnection );
    connect( workerPtr,       &Worker::updateRateMeasured,             settingsDialog, &SettingsDialog::applyMeasuredRate,            Qt::QueuedConnection );
    // Проба останавливается тем же способом, которым пользователь останавливает
    // расчёт. Отсчёт начинается по сигналу воркера, а не с самой просьбы: перед
    // замером может пройти подбор сетки, и он занимает секунды.
    connect( workerPtr, &Worker::updateRateProbeStarted, this, [this]() {
        QTimer::singleShot(Constants::PROBE_DURATION_MS, this, [this]() {
            if (workerPtr)
                workerPtr->cancel();
        });
    }, Qt::QueuedConnection );

    connect( this, static_cast<void (MainWindow::*)(const QJsonObject&)>( &MainWindow::sendSettingsToWorker ), workerPtr, &Worker::setSettings, Qt::QueuedConnection);


    // DirectConnection для того, чтобы частоту обновления можно было изменять в реальном времени
    //connect( this,            &MainWindow::refreshProgressbarValueChanged, workerPtr, &Worker::handleRefreshProgressbarValueChanged, Qt::DirectConnection );
    //connect( this,            &MainWindow::refreshSpectrumValueChanged,    workerPtr, &Worker::handleRefreshSpectrumValueChanged,    Qt::DirectConnection );
}

void MainWindow::connectSettingsDialog()
{
    if (!settingsDialog) return;


    connect( this,     &MainWindow::matrixChanged,            settingsDialog, &SettingsDialog::handleMatrixChanged     );
    connect( this,     &MainWindow::setInterfaceEnabled,      settingsDialog, &SettingsDialog::setInterfaceEnabled     );
    connect( this,     &MainWindow::requestSettings,          settingsDialog, &SettingsDialog::handleSettingsRequested );
    connect( this,     &MainWindow::applySettingsFromAutosave, settingsDialog, &SettingsDialog::applyFromAutosave       );

    // Записываем матрицу при получении
    // Замер потолка обновления: короткий расчёт на настройках, которые сейчас
    // выставлены в диалоге, — не на тех, что подтверждены кнопкой.
    connect( settingsDialog, &SettingsDialog::measureUpdateRateRequested,
        this, [this]( const QJsonObject& obj ) {
            if (!workerPtr)
                return;
            // Без матрицы пробе не с чем работать, а описание задачи на пустой
            // матрице лезет за её первую строку.
            const QString error = matrixError();
            if (!error.isEmpty()) {
                QMessageBox::warning(this, UIStrings::ERROR_TITLE, error);
                settingsDialog->applyMeasuredRate(0.0);
                return;
            }

            ComputationSettings probe = ComputationSettings::fromJson(obj);
            probe.matrix  = ui->matrixPTE->toStringList();
            probe.matrix2 = matrix2PTE->toStringList();

            workerThreadPtr->start();
            QMetaObject::invokeMethod(workerPtr, "setSettings", Qt::QueuedConnection,
                                      Q_ARG(QJsonObject, probe.toJson()));
            QMetaObject::invokeMethod(workerPtr, "measureUpdateRate", Qt::QueuedConnection);
        });

    connect( settingsDialog, &SettingsDialog::sendSettingsToWidget,
        this, [this]( const QJsonObject& obj ) {
            const ComputationSettings::Algorithm before = settings.algorithmType;
            settings = ComputationSettings::fromJson(obj);
            settings.matrix  = ui->matrixPTE->toStringList();
            settings.matrix2 = matrix2PTE->toStringList();
            spectrumPlot->setMaxBars(settings.maxPlotBars);
            updateMatrixTabs();

            // Поднятая запись — про свой алгоритм. Сменили алгоритм — кнопка
            // «Продолжить» больше не про неё: иначе расчёт стартовал бы как
            // продолжение и тащил бы за собой состояние прежнего показа.
            if (runState == RunState::Loaded && settings.algorithmType != before) {
                runState = RunState::Idle;
                updateExecuteButton();
            }
            MainWindow::sendSettingsToWorker(settings.toJson());

            // Идущему расчёту настройки через очередь не доходят: воркер до
            // самого конца не возвращается в свой цикл событий. Живые интервалы
            // передаются напрямую.
            if (workerPtr && (runState == RunState::Running || runState == RunState::Paused))
                workerPtr->setLiveIntervals(settings.timeIntSet.updateSpectrumInterval,
                                            settings.timeIntSet.saveSpectrumInterval);
        });
}


void MainWindow::on_executePBN_clicked()
{
    // Одна и та же кнопка запускает, ставит на паузу и продолжает расчёт.
    switch (runState) {
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

    const QString first = check(ui->matrixPTE->toStringList(),
                                settings.algorithmType == ComputationSettings::ProductCode
                                    ? tr("Матрица 1") : tr("Матрица"));
    if (!first.isEmpty())
        return first;
    // Код произведения: компоненты проверяются каждая сама по себе, само
    // произведение в памяти не строится, и его размер ничем не ограничен.
    if (settings.algorithmType == ComputationSettings::ProductCode)
        return check(matrix2PTE->toStringList(), tr("Матрица 2 (вторая компонента)"));
    return QString();
}

void MainWindow::startComputation()
{
    const bool resuming = runState == RunState::Loaded;

    if (!workerPtr) {
        QMessageBox::warning(this, UIStrings::ERROR_TITLE, tr("Worker не подключён"));
        return;
    }

    // Проверка идёт до блокировки интерфейса: иначе при ошибке в матрице он
    // успевал погаснуть и тут же зажечься, а в строке состояния оставалось
    // «Готово» о расчёте, которого не было.
    const QString error = matrixError();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, UIStrings::ERROR_TITLE, error);
        return;
    }

    emit setInterfaceEnabled(false);
    ui->matrixPTE->setReadOnly(true);
    matrix2PTE->setReadOnly(true);
    ui->cancelPBN->setEnabled(true);
    matrixMenu->setActionsEnabled(false);
    statsPanel->showState(tr("Идёт расчёт"));
    ui->infoPBR->setValue(0);

    // Поток нужен уже сейчас: настройки уходят воркеру через очередь событий.
    workerThreadPtr->start();
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
    QMetaObject::invokeMethod(workerPtr, "initializeRunState", Qt::QueuedConnection,
                              Q_ARG(LoadMode, mode));

    // Настройки к этому моменту уже пришли от диалога по requestSettings.
    statsPanel->showTask(settings);
    statsPanel->clearProgress();
    unseenByWeight.clear();

    QMetaObject::invokeMethod(workerPtr, "computeSpectrum", Qt::QueuedConnection);

    runState = RunState::Running;
    updateExecuteButton();
}

void MainWindow::pauseComputation()
{
    if (workerPtr)
        workerPtr->pause();

    runState = RunState::Paused;
    setWindowTitle(UIStrings::PAUSE_TEXT);
    statsPanel->showState(tr("Пауза"));
    updateExecuteButton();
}

void MainWindow::resumeComputation()
{
    if (workerPtr)
        workerPtr->resume();

    runState = RunState::Running;
    statsPanel->showState(tr("Идёт расчёт"));
    updateExecuteButton();

    // Оценка времени с прошлого запуска ещё актуальна — возвращаем её
    // в заголовок вместо «Пауза».
    setWindowTitle(remainingMinutes != -1 ? Format::remainingTime(remainingMinutes)
                                          : UIStrings::MAIN_TITLE);
}

void MainWindow::on_exitPBN_clicked()
{
    if( workerPtr ){
        workerPtr->cancel();
    }
    if( workerThreadPtr ){
        workerThreadPtr->quit();
        workerThreadPtr->wait();
    }
    
    emit setInterfaceEnabled(   true  );
    saveSettings();
    ui->matrixPTE->setReadOnly( false );
    matrix2PTE->setReadOnly( false );
    qApp->exit();
}

void MainWindow::on_settingsPBN_clicked()
{
    settingsDialog->exec();
    applySettings();
}

void MainWindow::on_cancelPBN_clicked()
{
    if (workerPtr) {
        workerPtr->cancel();
    }
    if (workerThreadPtr) {
        workerThreadPtr->quit();
        workerThreadPtr->wait();
    }

    emit setInterfaceEnabled(   true  );
    ui->matrixPTE->setReadOnly( false );
    matrix2PTE->setReadOnly( false );
    ui->cancelPBN->setEnabled(  false );
}

//
// Worker signal handlers
//

void MainWindow::handleUpdateInfoPBR(int percent)
{
    // Обновляем прогрессбар в ui
    ui->infoPBR->setValue(percent);

    taskbar->setPercent(percent);
}

void MainWindow::sendSettingsToWorker()
{
    MainWindow::sendSettingsToWorker(settings.toJson());
}

void MainWindow::handleUpdateSpectrumPlot(const SpectrumFloat spectrum)
{
    spectrumPlot->setSpectrum(spectrum);
}

void MainWindow::handleUpdateSpectrumPTE( const SpectrumText spectrum )
{
    setSpectrumRows(spectrum);
}
// Сетку показываем: иначе при включённом автоподборе непонятно, на чём
// программа в итоге считает и почему время отличается от прошлого запуска.
void MainWindow::handleGridTuned(int blocks, int threads)
{
    statsPanel->showGrid(blocks, threads);
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
    this->unseenByWeight = unseenByWeight;
    setSpectrumRows(lastSpectrum);
}

void MainWindow::handleProductPlan(const QString& text, int exactUpToWeight)
{
    // Что сейчас считается — в строке состояния: другой строки под это нет.
    statsPanel->showState(text);
    Q_UNUSED(exactUpToWeight);
}

void MainWindow::handleUpdateRemainingMinutes(int elapsedSec, int minutesLeft, double speed,
                                              quint64 doneOps, quint64 totalOps)
{
    remainingMinutes = minutesLeft;

    const QString elapsedStr = Format::duration(elapsedSec);

    statsPanel->showProgress(elapsedSec, minutesLeft, speed, doneOps, totalOps);

    // Имя программы в заголовке остаётся: раньше он превращался просто в
    // "2 ч 15 мин", и в панели задач было непонятно, что это за окно.
    this->setWindowTitle(tr("%1 — осталось %2")
                             .arg(UIStrings::MAIN_TITLE, Format::remainingTime(remainingMinutes)));
}
void MainWindow::showSaveLBL()
{
    ui->saveLBL->setToolTip(UIStrings::SAVE_LBL_ICON_TOOLTIP);
    QPropertyAnimation* anim = new QPropertyAnimation(saveLBLOpacityEffect, "opacity", this);
    if (ui->saveLBL->underMouse()) {
        QToolTip::showText(QCursor::pos(), UIStrings::SAVE_LBL_ICON_TOOLTIP, ui->saveLBL);
    }

    anim->setDuration(1000); // общая длительность
    anim->setStartValue(0.0);
    anim->setKeyValueAt(0.5, 1.0); // середина — полностью видно
    anim->setEndValue(0.0);
    connect(anim, &QPropertyAnimation::finished, this, [this]() {
        // Возвращаем базовый tooltip
        ui->saveLBL->setToolTip(UIStrings::SAVE_LBL_BASE_TOOLTIP);
        if (ui->saveLBL->underMouse()) {
            QToolTip::showText(QCursor::pos(), UIStrings::SAVE_LBL_BASE_TOOLTIP, ui->saveLBL);
        }
        });

    anim->start(QAbstractAnimation::DeleteWhenStopped);
}
void MainWindow::handleMatrixChanged()
{
    // Матрицу правят руками — поднятое сохранение к ней больше не относится.
    if (runState == RunState::Loaded && !applyingAutosave) {
        runState = RunState::Idle;
        updateExecuteButton();
    }

    Matrix rows = ui->matrixPTE->toStringList();
    int maxLen = 0;
    for (const QString& row : rows)
        maxLen = qMax(maxLen, row.length());
    // При изменении размеров матрицы автоматически вызовется слот в settingsDialog-е, который отправит новые настройки
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
    if (!matrixDock || !matrixTabBar)
        return;
    auto size = [](const Matrix& rows) {
        int maxLen = 0;
        for (const QString& row : rows) maxLen = qMax(maxLen, row.length());
        return QStringLiteral(" (%1,%2)").arg(maxLen).arg(rows.size());
    };
    const bool product = settings.algorithmType == ComputationSettings::ProductCode;
    matrixDock->setWindowTitle(product ? tr("Матрицы")
                                       : tr("Матрица") + size(ui->matrixPTE->toStringList()));
    matrixTabBar->setTabText(0, tr("Матрица 1") + size(ui->matrixPTE->toStringList()));
    matrixTabBar->setTabText(1, tr("Матрица 2") + size(matrix2PTE->toStringList()));
}
void MainWindow::handleError(const QString& message)
{
    QMessageBox::critical(this, UIStrings::ERROR_TITLE, message);
    // reset UI
    runState = RunState::Idle;
    updateExecuteButton();
}

void MainWindow::handleFinished(int elapsedSec)
{
    runState = RunState::Idle;
    
    if(workerThreadPtr->isRunning()){
        workerThreadPtr->quit();
        workerThreadPtr->wait();
    }
    workerPtr->resume();
    emit setInterfaceEnabled(   true  );
    ui->matrixPTE->setReadOnly( false );
    matrix2PTE->setReadOnly( false );
    ui->cancelPBN->setEnabled(  false );
    matrixMenu->setActionsEnabled(true);

    updateExecuteButton();
    this->setWindowTitle( UIStrings::MAIN_TITLE  );
    if ( workerPtr->isCancelled() ) {
        workerPtr->uncancel();
        // Расчёт останавливали ради загрузки сохранения — вот теперь можно.
        if (pendingAutosave) {
            pendingAutosave = false;
            applyAutosaveNow(pendingMatrix, pendingRecord);
            return;
        }
        statsPanel->showState( UIStrings::CANCEL_TEXT );
        return;
    }
    // Меньше секунды — «0 с» выглядело бы как сбой замера.
    const QString elapsedStr = elapsedSec > 0 ? Format::duration(elapsedSec)
                                              : tr("< 1 с");
    statsPanel->showState( UIStrings::READY_TEXT + elapsedStr );

    // Расчёт успел добежать до конца, пока пользователь выбирал запись.
    if (pendingAutosave) {
        pendingAutosave = false;
        applyAutosaveNow(pendingMatrix, pendingRecord);
    }
}



bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // При изменение размера обновляем подписи под графиком
    if (watched == ui->spectrumCPT && event->type() == QEvent::Resize) {
        // Не перерисовываем сразу: пока панель тащат, размер меняется
        // непрерывно. Таймер сбрасывается на каждом событии и срабатывает
        // один раз, когда размер устоялся.
        plotRefreshTimer->start();
        return false;
    }
    // Для всех остальных событий — стандартная обработка
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::hasCheckpoint() const
{
    AutosaveRecord record;
    const Matrix key = settings.algorithmType == ComputationSettings::ProductCode
                           ? settings.matrix + settings.matrix2 : settings.matrix;
    if (!autosave.load(key, settings.algorithmType, record))
        return false;

    // Запись может оказаться непригодной: она ушла дальше, чем просят сейчас.
    return canResume(record, settings);
}

// Применить настройки
void MainWindow::applySettings()
{
    spectrumPlot->setMaxBars(settings.maxPlotBars);

    // Точка, где настройки из диалога попадают в интерфейс. Сейчас
    // единственное, что сюда просилось, — цвет и прозрачность столбцов,
    // но эти поля в диалоге пока не подключены.
}

// Сохранить настройки в реестр
void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(SettingsKeys::WINDOW_STATE,    this->saveState(Constants::LAYOUT_VERSION));
    s.setValue(SettingsKeys::CODE_MATRIX,     ui->matrixPTE->toPlainText()   );
    s.setValue(SettingsKeys::CODE_MATRIX2,    matrix2PTE->toPlainText()      );
    s.setValue(SettingsKeys::SPECTRUM_TEXT,   lastSpectrum.join(QLatin1Char('\n')) );
    s.setValue(SettingsKeys::WIDGET_GEOMETRY, this->saveGeometry()           );
    QVariantList values;
    for (double v : spectrumPlot->values())
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
    setSpectrumRows( s.value(SettingsKeys::SPECTRUM_TEXT).toString()
                          .split(QLatin1Char('\n'), Qt::SkipEmptyParts) );
    ui->matrixPTE->setPlainText(              s.value(SettingsKeys::CODE_MATRIX                    ).toString()          );
    matrix2PTE->setPlainText(                 s.value(SettingsKeys::CODE_MATRIX2                   ).toString()          );
    if (s.contains(SettingsKeys::SPECTRUM_VALUES)) {
        QVariantList values = s.value(SettingsKeys::SPECTRUM_VALUES).toList();
        SpectrumFloat spectrum;
        spectrum.reserve(values.size());
        for (const QVariant &v : values)
            spectrum.append(v.toFloat());
        spectrumPlot->setSpectrum(spectrum);
    }
}
