#include "widget.h"
#include "format.h"
#include "matrixlibrary.h"
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
void MainWindow::rebuildMatrixMenuActions()
{
    if (!matrixMenu) return;

    // 1) Подгружаем актуальные данные
    matrixLibrary.load();
    QStringList names = matrixLibrary.names(); // текущие имена

    // 2) Удаляем любые динамические действия (все, кроме addMatrix и действия подменю deleteMenu)
    QAction* deleteMenuAction = ui->deleteMatrixMNU ? ui->deleteMatrixMNU->menuAction() : nullptr;
    const QList<QAction*> actsSnapshot = matrixMenu->actions();
    for (QAction* a : actsSnapshot) {
        if ( a == ui->addMatrixACN || a == deleteMenuAction )
            continue;
        matrixMenu->removeAction(a);
    }

    // 3) Очищаем подменю удаления
    if (ui->deleteMatrixMNU)
        ui->deleteMatrixMNU->clear();

    // 4) Если есть имена — добавляем динамические пункты и включаем deleteMenu (учитываем флаг)
    if (!names.isEmpty()) {
        matrixMenu->addSeparator();

        // пункты в основном меню — загрузка матрицы
        for (const QString& nm : names) {
            QAction* act = new QAction(nm, matrixMenu);
            act->setData(nm);
            act->setIcon(QIcon(":/ui/icons/newspaper.png"));
            act->setEnabled(matrixActionsEnabled); // учитываем флаг

            connect(act, &QAction::triggered, this, [this, nm]() {
                const QString code = matrixLibrary.matrix(nm);
                ui->matrixPTE->setPlainText(code);
                });

            matrixMenu->addAction(act);
        }

        // пункты в подменю удаления (родитель = deleteMenu)
        for (const QString& nm : names) {
            QAction* delAct = new QAction(nm, ui->deleteMatrixMNU);
            delAct->setIcon(QIcon(":/ui/icons/newspaper.png"));
            delAct->setEnabled(matrixActionsEnabled); // учитываем флаг

            connect(delAct, &QAction::triggered, this, [this, nm]() {
                if (!matrixLibrary.remove(nm)) {
                    QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось удалить матрицу \"%1\"").arg(nm));
                    return;
                }

                matrixLibrary.load();

                QAction* caller = qobject_cast<QAction*>(sender());
                if (caller) {
                    ui->deleteMatrixMNU->removeAction(caller);
                    caller->deleteLater();
                }

                for (QAction* ma : matrixMenu->actions()) {
                    if (ma == ui->addMatrixACN) continue;
                    if (ma == ui->deleteMatrixMNU->menuAction()) continue;
                    if (ma->data().toString() == nm || ma->text() == nm) {
                        matrixMenu->removeAction(ma);
                        ma->deleteLater();
                        break;
                    }
                }

                ui->deleteMatrixMNU->setEnabled(matrixActionsEnabled && !ui->deleteMatrixMNU->actions().isEmpty());
                });

            ui->deleteMatrixMNU->addAction(delAct);
        }

        ui->deleteMatrixMNU->setEnabled(matrixActionsEnabled && !ui->deleteMatrixMNU->actions().isEmpty());
    }
    else {
        // нет сохранённых матриц
        if (ui->deleteMatrixMNU)
            ui->deleteMatrixMNU->setEnabled(false);
    }
}
void MainWindow::setToolTips() {
    ui->matrixLBL->setToolTip(UIStrings::MATRIX_TOOLTIP);
    ui->spectrumLBL->setToolTip(UIStrings::SPECTRUM_TOOLTIP);
    ui->executePBN->setToolTip(UIStrings::START_TOOLTIP);
    ui->cancelPBN->setToolTip(UIStrings::CANCEL_TOOLTIP);
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

    connect( this, static_cast<void (MainWindow::*)(const QJsonObject&)>( &MainWindow::sendSettingsToWorker ), workerPtr, &Worker::setSettings, Qt::QueuedConnection);


    // DirectConnection для того, чтобы частоту обновления можно было изменять в реальном времени
    //connect( this,            &MainWindow::refreshProgressbarValueChanged, workerPtr, &Worker::handleRefreshProgressbarValueChanged, Qt::DirectConnection );
    //connect( this,            &MainWindow::refreshSpectrumValueChanged,    workerPtr, &Worker::handleRefreshSpectrumValueChanged,    Qt::DirectConnection );
}

void MainWindow::setMatrixMenu()
{
    ui->matrixLBL->setContextMenuPolicy(Qt::CustomContextMenu);

    // Создаём меню и базовые действия один раз
    matrixMenu = ui->matrixMNU;

    connect(ui->addMatrixACN, &QAction::triggered, this, &MainWindow::onAddMatrixTriggered);
    ui->addMatrixACN->setIcon(QIcon(":/ui/icons/newspaper--plus.png"));
    ui->deleteMatrixMNU->setIcon(QIcon(":/ui/icons/newspaper--minus.png"));
    // функция-утилита для обновления состояния доступности пунктов
    auto updateMenuEnabledState = [this]() {
        matrixLibrary.load(); // обновим массив, чтобы проверить наличие
        ui->deleteMatrixMNU->setEnabled(matrixActionsEnabled && !matrixLibrary.isEmpty());
        };

    // По умолчанию включаем/выключаем подменю — учитываем флаг matrixActionsEnabled
    matrixLibrary.load();
    ui->deleteMatrixMNU->setEnabled(matrixActionsEnabled && !matrixLibrary.isEmpty());

    matrixMenu->setMouseTracking(true);

    // Таймер для отложенного открытия подменю
    matrixMenuTimer = new QTimer(this);
    matrixMenuTimer->setSingleShot(true);
    matrixMenuTimer->setInterval(120);

    connect(matrixMenu, &QMenu::hovered, this, [this](QAction* act) {
        pendingHover = act;
        if (pendingHover && pendingHover->menu() && matrixActionsEnabled)
            matrixMenuTimer->start();
        else
            matrixMenuTimer->stop();
        });

    connect(matrixMenuTimer, &QTimer::timeout, this, [this]() {
        if (!matrixMenu || !pendingHover || !pendingHover->menu()) return;
        QPoint global = QCursor::pos();
        QPoint local = matrixMenu->mapFromGlobal(global);
        QAction* under = matrixMenu->actionAt(local);
        if (under == pendingHover) {
            matrixMenu->setActiveAction(pendingHover);
            return;
        }
        QRect rect = matrixMenu->actionGeometry(pendingHover);
        if (!rect.isNull()) {
            const int margin = 6;
            QRect expanded = rect.adjusted(-margin, -margin, margin, margin);
            if (expanded.contains(local))
                matrixMenu->setActiveAction(pendingHover);
        }
        });

    // Обновляем только динамическую часть перед показом
    connect(matrixMenu, &QMenu::aboutToShow, this, [this]() {
        rebuildMatrixMenuActions();
    });
    // синхронизируем стартовое состояние (на случай, если matrixActionsEnabled уже false)
    updateMenuEnabledState();
}

void MainWindow::setMatrixActionsEnabled(bool enabled)
{
    matrixActionsEnabled = enabled;

    if (!matrixMenu) return;

    // действие, которое представляет подменю удаления
    QAction* deleteMenuAction = ui->deleteMatrixMNU ? ui->deleteMatrixMNU->menuAction() : nullptr;

    // 1) Обновим уже существующие динамические пункты (если они есть)
    for (QAction* a : matrixMenu->actions()) {
        if (a == ui->addMatrixACN || a == deleteMenuAction)
            continue;
        a->setEnabled(enabled);
    }

    // 2) Обновим действия внутри подменю "Удалить"
    if (ui->deleteMatrixMNU) {
        for (QAction* a : ui->deleteMatrixMNU->actions()) {
            a->setEnabled(enabled);
        }
        ui->deleteMatrixMNU->setEnabled(enabled && !ui->deleteMatrixMNU->actions().isEmpty());
    }

    // 3) Если меню видно — перестроим динамику прямо сейчас, чтобы новые enabled/disabled вступили в силу.
    if (matrixMenu->isVisible()) {
        if (!enabled) {
            // если отключаем — безопаснее закрыть меню, чтобы не было неконсистентных взаимодействий
            matrixMenu->close();
        }
        else {
            // если включаем — перестроим пункты (rebuild сделает act->setEnabled(matrixActionsEnabled) для новых)
            rebuildMatrixMenuActions();
            // возможно, стоит обновить вид: matrixMenu->update(); но обычно rebuild достаточно
        }
    }
}

void MainWindow::connectSettingsDialog()
{
    if (!settingsDialog) return;


    connect( this,     &MainWindow::matrixChanged,            settingsDialog, &SettingsDialog::handleMatrixChanged     );
    connect( this,     &MainWindow::setInterfaceEnabled,      settingsDialog, &SettingsDialog::setInterfaceEnabled     );
    connect( this,     &MainWindow::requestSettings,          settingsDialog, &SettingsDialog::handleSettingsRequested );

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
    switch (runState) {
        case RunState::Idle: {
            
            // Блокируем интерфейс
            emit setInterfaceEnabled(   false );
            ui->matrixPTE->setReadOnly( true  );
            ui->cancelPBN->setEnabled(  true  );
            setMatrixActionsEnabled(    false );
            ui->infoLBL->setText("");
            ui->infoLBL->show();
            ui->infoPBR->setValue(0);

            if (!workerPtr) {
                QMessageBox::warning(this, UIStrings::ERROR_TITLE, QString::fromUtf8("Worker не подключён"));
                handleFinished(-1);
                return;
            }
            Matrix rows = ui->matrixPTE->toStringList();
            if ( rows.isEmpty()) {
                QMessageBox::warning(this, UIStrings::ERROR_TITLE, QString::fromUtf8("Матрица пустая"));
                handleFinished(-1);
                return;
            }
            quint64 numOfRows = rows.size();
            if ( numOfRows > Constants::MAX_ROWS ) {
                QMessageBox::warning(this, UIStrings::ERROR_TITLE, QString("Число строк матрицы больше чем %1").arg(Constants::MAX_ROWS));
                handleFinished(-1);
                return;
            }
            quint64 numOfCols = (quint64)rows.first().length();
            for (const QString& r : rows) {
                if ((quint64)r.length() != numOfCols) {
                    QMessageBox::warning(this, UIStrings::ERROR_TITLE, QString("Все строки должны быть одинаковой длины"));
                    handleFinished(-1);
                    return;
                }
            }
            if ( numOfCols > Constants::MAX_COLS ) {
                QMessageBox::warning(this, UIStrings::ERROR_TITLE, QString("Число столбцов матрицы больше чем %1").arg(Constants::MAX_COLS));
                handleFinished(-1);
                return;
            }
            // Запускаем поток, чтобы отправить в него настройки
            workerThreadPtr->start();
            // Запрашиваем настройки для расчета
            emit requestSettings();

            // Если есть чекпоинт для данных настроек - выводим диалог
            if (hasCheckpoint()) {
                QMessageBox::StandardButton reply;
                reply = QMessageBox::question(
                    this,
                    "Найден спектр",
                    "Для текущих настроек обнаружен сохранённый спектр\nПродолжить вычисление с сохранённого состояния?",
                    QMessageBox::Yes | QMessageBox::No
                );
                // Загружаем RunState в зависимости от выбора пользователя
                QMetaObject::invokeMethod(
                    workerPtr,
                    "initializeRunState",
                    Qt::QueuedConnection,
                    Q_ARG(LoadMode, reply == QMessageBox::Yes
                        ? LoadMode::FromCheckpoint
                        : LoadMode::Reset)
                );
            }
            // Начинаем расчет
            QMetaObject::invokeMethod(workerPtr, "computeSpectrum", Qt::QueuedConnection );

            
            runState = RunState::Running;
            ui->executePBN->setText(UIStrings::PAUSE_TEXT    );
            ui->executePBN->setToolTip(UIStrings::PAUSE_TOOLTIP );
        } break;

        case RunState::Running: {
            if (workerPtr)
                workerPtr->pause();

            runState = RunState::Paused;
            this->setWindowTitle(UIStrings::PAUSE_TEXT);
            ui->executePBN->setText(UIStrings::CONTINUE_TEXT);
            ui->executePBN->setToolTip(UIStrings::CONTINUE_TOOLTIP);
        } break;

        case RunState::Paused: {
            if (workerPtr)
                workerPtr->resume();

            runState = RunState::Running;
            ui->executePBN->setText(UIStrings::PAUSE_TEXT);
            ui->executePBN->setToolTip(UIStrings::PAUSE_TOOLTIP);
            if (remainingMinutes != -1)
                this->setWindowTitle(Format::remainingTime(remainingMinutes));
            else
                this->setWindowTitle(UIStrings::MAIN_TITLE);
        } break;


    } 
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
    ui->executePBN->setText(UIStrings::START_TEXT);
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
    setMatrixActionsEnabled(    true  );

    ui->executePBN->setText( UIStrings::START_TEXT  );
    ui->executePBN->setToolTip( UIStrings::START_TOOLTIP );
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

void MainWindow::onAddMatrixTriggered()
{
    // Формируем дефолтное имя по текущему содержимому
    const QString defName = defaultMatrixName();

    // Создаём QInputDialog вручную, чтобы убрать кнопку "?" в заголовке
    QInputDialog dlg(this);
    dlg.setWindowTitle(tr("Сохранить матрицу"));
    dlg.setLabelText(tr("Имя матрицы:"));
    dlg.setTextValue(defName);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    if (dlg.exec() != QDialog::Accepted)
        return; // пользователь нажал Отмена

    const QString name = dlg.textValue().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Имя не может быть пустым"));
        return;
    }

    // Настройки мог поменять второй запущенный экземпляр — перечитываем.
    matrixLibrary.load();

    if (matrixLibrary.contains(name)) {
        const auto resp = QMessageBox::question(this, tr("Перезапись"),
            tr("Матрица с именем \"%1\" уже существует. Перезаписать?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (resp != QMessageBox::Yes)
            return;
    }

    matrixLibrary.save(name, ui->matrixPTE->toPlainText());

    // Включаем подменю удаления (если оно было выключено)
    if (deleteMenu)
        deleteMenu->setEnabled(!matrixLibrary.isEmpty());
}

bool MainWindow::hasCheckpoint() const
{
    // Считаем хеш текущих настроек
    quint64 hash = settings.computeHash();
    // Хеш - имя чекпоинта
    QString group = QString("checkpoints/%1").arg(hash);

    QSettings s;
    s.beginGroup(group);

    // Проверяем, есть ли чекпоинт вообще
    if (!s.contains("settings")) {
        s.endGroup();
        return false;
    }

    // Защита от коллизий
    QJsonObject savedSettingsObj = s.value("settings").toJsonObject();
    ComputationSettings saved = ComputationSettings::fromJson(savedSettingsObj);
    // Переписать
    if (!(saved == settings)/* && !(saved <= settings) */) {
        s.endGroup();
        return false;
    }
    return true;
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

QString MainWindow::defaultMatrixName()
{
    const Matrix rows = ui->matrixPTE->toStringList();
    if (rows.isEmpty())
        return QStringLiteral("(0,0)");

    // Столбцов столько, сколько символов в самой длинной строке: разделителей
    // в формате матрицы нет, каждый символ — отдельный бит.
    int cols = 0;
    for (const QString& row : rows)
        cols = qMax(cols, row.length());

    return tr("Матрица (%1,%2)").arg(cols).arg(rows.size());
}
