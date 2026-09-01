#include "widget.h"
#include "fonticons.h"
#include "format.h"
#include "matrixlibrary.h"
#include "autosavedialog.h"
#include "format.h"
#include "matrixmenu.h"
#include "spectrumplot.h"
#include "ui_widget.h"





MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    /********      УБРАТЬ В UI     ********/
        splitter = new QSplitter(this);
        splitter->setOrientation(Qt::Horizontal);
        splitter->addWidget( ui->spectrumPTE );
        splitter->addWidget( ui->spectrumCPT );
        ui->verticalLayout->insertWidget( 3, splitter );
        ui->verticalLayout->setStretch(1, 1);
        ui->verticalLayout->setStretch(3, 1);
        splitter->setSizes({ 1, 1 });
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


    loadSettings();
    ui->spectrumCPT->installEventFilter(this);
    connectSettingsDialog();
    setWorker();
    setMatrixMenu();
    setToolTips();
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
    ui->matrixLBL->setContextMenuPolicy(Qt::CustomContextMenu);

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
    // handleMatrixChanged сбрасывает поднятое состояние — он для того и нужен,
    // чтобы ловить правку матрицы руками. Своя подстановка правкой не считается.
    applyingAutosave = true;
    ui->matrixPTE->setPlainText(matrix.join(QLatin1Char('\n')));
    applyingAutosave = false;

    // Настройки берутся из записи. Без этого «Продолжить» искал бы сохранение
    // другого алгоритма, не нашёл и молча начал бы с нуля.
    emit applySettingsFromAutosave(int(record.algorithm), int(record.enumType),
                                   record.maxRows);

    // Спектр показывается сырым — ровно так же, как во время расчёта: у
    // дуального кода преобразование Мак-Вильямс делается только в конце.
    SpectrumFloat plot;
    SpectrumText  text;
    for (int w = 0; w < record.state.spectrum.size(); ++w) {
        const quint64 value = record.state.spectrum.at(w);
        plot.append(float(value));
        if (value != 0)
            text.append(QString::number(w) + " - " + QString::number(value));
    }
    handleUpdateSpectrumPlot(plot);
    handleUpdateSpectrumPTE(text);

    const double total = totalOperations(record, matrix.size());
    const int percent = total > 0.0 ? int(100.0 * double(record.state.doneOps) / total) : 0;
    ui->infoPBR->setValue(qBound(0, percent, 100));
    ui->infoLBL->setText(tr("Загружено сохранение: перебрано %1 слов")
                             .arg(Format::count(record.state.doneOps)));
    ui->infoLBL->show();

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

void MainWindow::setToolTips() {
    ui->matrixLBL->setToolTip(UIStrings::MATRIX_TOOLTIP);
    ui->spectrumLBL->setToolTip(UIStrings::SPECTRUM_TOOLTIP);
    ui->cancelPBN->setToolTip(UIStrings::CANCEL_TOOLTIP);
    // Квадрат остановки у отмены не меняется, поэтому ставится один раз.
    ui->cancelPBN->setIcon(FluentIcons::icon(this, FluentIcons::STOP));
    updateExecuteButton();
    ui->exitPBN->setToolTip(UIStrings::EXIT_TOOLTIP);
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
    connect( settingsDialog, &SettingsDialog::sendSettingsToWidget,
        this, [this]( const QJsonObject& obj ) {
            settings = ComputationSettings::fromJson(obj);
            settings.matrix = ui->matrixPTE->toStringList();
            MainWindow::sendSettingsToWorker(settings.toJson());
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
    const Matrix rows = ui->matrixPTE->toStringList();

    if (rows.isEmpty())
        return tr("Матрица пустая");

    if (quint64(rows.size()) > Constants::MAX_ROWS)
        return tr("Число строк матрицы больше чем %1").arg(Constants::MAX_ROWS);

    const int cols = rows.first().length();
    for (const QString& row : rows) {
        if (row.length() != cols)
            return tr("Все строки должны быть одинаковой длины");
    }

    if (quint64(cols) > Constants::MAX_COLS)
        return tr("Число столбцов матрицы больше чем %1").arg(Constants::MAX_COLS);

    return QString();
}

void MainWindow::startComputation()
{
    const bool resuming = runState == RunState::Loaded;

    // Прошлый подбор к новому расчёту отношения не имеет.
    tunedGrid.clear();

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
    ui->cancelPBN->setEnabled(true);
    matrixMenu->setActionsEnabled(false);
    ui->infoLBL->setText("");
    ui->infoLBL->show();
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
    updateExecuteButton();
}

void MainWindow::resumeComputation()
{
    if (workerPtr)
        workerPtr->resume();

    runState = RunState::Running;
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
    QString str = spectrum.join('\n');
    if (!str.isEmpty() && str.endsWith('\n'))
        str.chop(1);
    QScrollBar *vbar = ui->spectrumPTE->verticalScrollBar();
    int pos = vbar->value();
    ui->spectrumPTE->setPlainText( str );
    vbar->setValue(pos);
}
// Сетку показываем: иначе при включённом автоподборе непонятно, на чём
// программа в итоге считает и почему время отличается от прошлого запуска.
void MainWindow::handleGridTuned(int blocks, int threads)
{
    tunedGrid = tr("Сетка запуска:      %1 блоков x %2 нитей (подобрана)")
                    .arg(blocks).arg(threads);
}

void MainWindow::handleUpdateRemainingMinutes(int elapsedSec, int minutesLeft, double speed,
                                              quint64 doneOps, quint64 totalOps)
{
    remainingMinutes = minutesLeft;

    const QString elapsedStr = Format::duration(elapsedSec);

    QString infoText =
        tr("Прошло времени:     %1").arg(elapsedStr) + "\n" +
        tr("Осталось времени:   %1").arg(Format::remainingTime(remainingMinutes)) + "\n" +
        tr("Средняя скорость:   %1").arg(Format::speed(speed)) + "\n" +
        // Проценты хороши для полоски, но масштаб задачи по ним не понять:
        // "43 %" ничего не говорит, а "1.6 из 3.8 трлн слов" — говорит.
        tr("Перебрано слов:     %1 из %2").arg(Format::count(doneOps), Format::count(totalOps));

    if (!tunedGrid.isEmpty())
        infoText += "\n" + tunedGrid;

    ui->infoLBL->setText(infoText);

    // Имя программы в заголовке остаётся: раньше он превращался просто в
    // "2 ч 15 мин", и в панели задач было непонятно, что это за окно.
    this->setWindowTitle(tr("%1 — осталось %2")
                             .arg(UIStrings::MAIN_TITLE, Format::remainingTime(remainingMinutes)));
    ui->infoLBL->show();
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
    ui->matrixLBL->setText(QString::fromUtf8("Матрица (%1,%2):").arg(maxLen).arg(rows.size()));
    
    
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
    ui->cancelPBN->setEnabled(  false );
    matrixMenu->setActionsEnabled(true);

    updateExecuteButton();
    this->setWindowTitle( UIStrings::MAIN_TITLE  );
    if ( workerPtr->isCancelled() ) {
        workerPtr->uncancel();
        ui->infoLBL->setText( UIStrings::CANCEL_TEXT );
        return;
    }
    // Меньше секунды — «0 с» выглядело бы как сбой замера.
    const QString elapsedStr = elapsedSec > 0 ? Format::duration(elapsedSec)
                                              : tr("< 1 с");
    ui->infoLBL->setText( UIStrings::READY_TEXT + elapsedStr );
}



bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // При изменение размера обновляем подписи под графиком
    if (watched == ui->spectrumCPT && event->type() == QEvent::Resize) {
        spectrumPlot->refresh();
        return false;
    }
    // Для всех остальных событий — стандартная обработка
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::hasCheckpoint() const
{
    AutosaveRecord record;
    if (!autosave.load(settings.matrix, settings.algorithmType, record))
        return false;

    // Запись может оказаться непригодной: она ушла дальше, чем просят сейчас.
    return canResume(record, settings);
}

// Применить настройки
void MainWindow::applySettings()
{
    // Точка, где настройки из диалога попадают в интерфейс. Сейчас
    // единственное, что сюда просилось, — цвет и прозрачность столбцов,
    // но эти поля в диалоге пока не подключены.
}

// Сохранить настройки в реестр
void MainWindow::saveSettings()
{
    QSettings s;
    if( splitter )
        s.setValue(SettingsKeys::SPLITTER_STATE,  this->splitter->saveState()    );
    s.setValue(SettingsKeys::CODE_MATRIX,     ui->matrixPTE->toPlainText()   );
    s.setValue(SettingsKeys::SPECTRUM_TEXT,   ui->spectrumPTE->toPlainText() );
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
    if( splitter ) splitter->restoreState(    s.value(SettingsKeys::SPLITTER_STATE                 ).toByteArray()       );
    ui->spectrumPTE->setPlainText(            s.value(SettingsKeys::SPECTRUM_TEXT                  ).toString()          );
    ui->matrixPTE->setPlainText(              s.value(SettingsKeys::CODE_MATRIX                    ).toString()          );
    if (s.contains(SettingsKeys::SPECTRUM_VALUES)) {
        QVariantList values = s.value(SettingsKeys::SPECTRUM_VALUES).toList();
        SpectrumFloat spectrum;
        spectrum.reserve(values.size());
        for (const QVariant &v : values)
            spectrum.append(v.toFloat());
        spectrumPlot->setSpectrum(spectrum);
    }
}
