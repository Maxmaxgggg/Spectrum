#include "widget.h"
#include "ui_widget.h"


QStringList Colors{ "Blue", "Green", "Red" };



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
    /********                      ********/
    if( settings == nullptr )
        settings = new SettingsDialog(this);

    msg = new QCPItemText(ui->spectrumCPT);
    msg->position->setType(QCPItemPosition::ptAxisRectRatio);
    msg->position->setCoords(0.5, 0.5);

    msg->setPositionAlignment(Qt::AlignCenter);

    msg->setText(QString::fromUtf8(
        "Спектральные компоненты слишком велики\n"
        "Невозможно отобразить графически"));

    QFont f;
    f.setPointSize(12);
    f.setBold(true);
    msg->setFont(f);

    // Инициализация QCustomPlot и QCPBars (предполагается, что в ui есть spectrumCPT)
    ui->spectrumCPT->xAxis->setTickLabelRotation(0);
    ui->spectrumCPT->yAxis->setNumberFormat("eb");
    ui->spectrumCPT->yAxis->setNumberPrecision(2);
    // Создаём QCPBars единожды (если в .ui Plottables уже нет)
    if ( spectrumBars == nullptr ) {
        spectrumBars = new QCPBars(ui->spectrumCPT->xAxis, ui->spectrumCPT->yAxis);
        spectrumBars->setPen(QPen(Qt::black));
        spectrumBars->setBrush(QBrush(Qt::blue));
    }
    connect( ui->matrixPTE, &FilterPlainTextEdit::textChanged, this,  &MainWindow::handleMatrixChanged    );
    connect(ui->settingsACN, &QAction::triggered,
        this, [this]() {
            settings->exec();
            applySettings();
        });


    loadSettings();
    ui->spectrumCPT->installEventFilter(this);
    connectSettingsDialog();
    setWorker();
    setMatrixMenu();
    setToolTips();
    emit handleMatrixChanged();
    
    //emit requestInitialSettings();
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
    loadMatricesArray();
    QStringList names = listMatrixNames(); // текущие имена

    // 2) Удаляем любые динамические действия (все, кроме addMatrix и действия подменю deleteMenu)
    QAction* deleteMenuAction = ui->deleteMatrix ? ui->deleteMatrix->menuAction() : nullptr;
    const QList<QAction*> actsSnapshot = matrixMenu->actions();
    for (QAction* a : actsSnapshot) {
        if ( a == ui->addMatrixACN || a == deleteMenuAction )
            continue;
        matrixMenu->removeAction(a);
    }

    // 3) Очищаем подменю удаления
    if (ui->deleteMatrix)
        ui->deleteMatrix->clear();

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
                const QString code = getMatrixByName(nm);
                ui->matrixPTE->setPlainText(code);
                });

            matrixMenu->addAction(act);
        }

        // пункты в подменю удаления (родитель = deleteMenu)
        for (const QString& nm : names) {
            QAction* delAct = new QAction(nm, ui->deleteMatrix);
            delAct->setIcon(QIcon(":/ui/icons/newspaper.png"));
            delAct->setEnabled(matrixActionsEnabled); // учитываем флаг

            connect(delAct, &QAction::triggered, this, [this, nm]() {
                if (!removeMatrixByName(nm)) {
                    QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось удалить матрицу \"%1\"").arg(nm));
                    return;
                }

                loadMatricesArray();

                QAction* caller = qobject_cast<QAction*>(sender());
                if (caller) {
                    ui->deleteMatrix->removeAction(caller);
                    caller->deleteLater();
                }

                for (QAction* ma : matrixMenu->actions()) {
                    if (ma == ui->addMatrixACN) continue;
                    if (ma == ui->deleteMatrix->menuAction()) continue;
                    if (ma->data().toString() == nm || ma->text() == nm) {
                        matrixMenu->removeAction(ma);
                        ma->deleteLater();
                        break;
                    }
                }

                ui->deleteMatrix->setEnabled(matrixActionsEnabled && !ui->deleteMatrix->actions().isEmpty());
                });

            ui->deleteMatrix->addAction(delAct);
        }

        ui->deleteMatrix->setEnabled(matrixActionsEnabled && !ui->deleteMatrix->actions().isEmpty());
    }
    else {
        // нет сохранённых матриц
        if (ui->deleteMatrix)
            ui->deleteMatrix->setEnabled(false);
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


    connect( this,            &MainWindow::sendSettingsToWorker, workerPtr, &Worker::setSettings, Qt::QueuedConnection);


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
    ui->deleteMatrix->setIcon(QIcon(":/ui/icons/newspaper--minus.png"));
    // функция-утилита для обновления состояния доступности пунктов
    auto updateMenuEnabledState = [this]() {
        loadMatricesArray(); // обновим массив, чтобы проверить наличие
        ui->deleteMatrix->setEnabled(matrixActionsEnabled && !matrices.isEmpty());
        };

    // По умолчанию включаем/выключаем подменю — учитываем флаг matrixActionsEnabled
    loadMatricesArray();
    ui->deleteMatrix->setEnabled(matrixActionsEnabled && !matrices.isEmpty());

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
    QAction* deleteMenuAction = ui->deleteMatrix ? ui->deleteMatrix->menuAction() : nullptr;

    // 1) Обновим уже существующие динамические пункты (если они есть)
    for (QAction* a : matrixMenu->actions()) {
        if (a == ui->addMatrixACN || a == deleteMenuAction)
            continue;
        a->setEnabled(enabled);
    }

    // 2) Обновим действия внутри подменю "Удалить"
    if (ui->deleteMatrix) {
        for (QAction* a : ui->deleteMatrix->actions()) {
            a->setEnabled(enabled);
        }
        ui->deleteMatrix->setEnabled(enabled && !ui->deleteMatrix->actions().isEmpty());
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
    if (!settings) return;


    connect( this,     &MainWindow::matrixChanged,            settings, &SettingsDialog::handleMatrixChanged     );
    connect( this,     &MainWindow::setInterfaceEnabled,      settings, &SettingsDialog::setInterfaceEnabled     );
    connect( this,     &MainWindow::requestSettings,          settings, &SettingsDialog::handleSettingsRequested );
    connect( settings, &SettingsDialog::sendSettingsToWorker, this,     &MainWindow::sendSettingsToWorker        );
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
            QStringList rows = ui->matrixPTE->toPlainText().split('\n', Qt::SkipEmptyParts);
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
            //qint64 maxComb = 0;
            //// Получаем индекс максимального комбина, который умещается в quint64
            //qint64 maxCombInd = maxCombIndex(numOfRows);
            //if (maxComb <= 0 || maxComb > rows.size()) maxComb = rows.size();
            //if (maxComb > maxCombInd) maxComb = maxCombInd;
            workerThreadPtr->start();
            // Запрашиваем настройки для расчета
            emit requestSettings();
            // Начинаем расчет
            QMetaObject::invokeMethod(workerPtr, "computeSpectrum", Qt::QueuedConnection, Q_ARG(QStringList, rows) );

            
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
                this->setWindowTitle(formatRemainingTime(remainingMinutes));
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
    settings->exec();
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

void MainWindow::handleStrValChanged()
{
    if( workerPtr )
        workerPtr->cancel();
    if( workerThreadPtr ){
        workerThreadPtr->quit();
        workerThreadPtr->wait();
    }
    runState = RunState::Idle;
    ui->infoPBR->setValue(0);
    ui->executePBN->setText(UIStrings::START_TEXT);

}

//
// Worker signal handlers
//

void MainWindow::handleUpdateInfoPBR(int percent)
{
    ui->infoPBR->setValue(percent);
}

void MainWindow::handleUpdateSpectrumPlot(const QVector<float> spectrum)
{
    updatePlot(spectrum);
}

void MainWindow::handleUpdateSpectrumPTE(const QStringList spectrum)
{
    QString str = spectrum.join('\n');
    if (!str.isEmpty() && str.endsWith('\n'))
        str.chop(1);
    QScrollBar *vbar = ui->spectrumPTE->verticalScrollBar();
    int pos = vbar->value();
    ui->spectrumPTE->setPlainText( str );
    vbar->setValue(pos);
}
void MainWindow::handleUpdateRemainingMinutes(int elapsedSec, int minutesLeft)
{
    remainingMinutes = minutesLeft;
    int days = elapsedSec / 86400;
    int hours = (elapsedSec % 86400) / 3600;
    int minutes = (elapsedSec % 3600) / 60;
    int seconds = elapsedSec % 60;
    QStringList parts;
    if (days > 0)
        parts << QString("%1 д").arg(days);
    if (hours > 0)
        parts << QString("%1 ч").arg(hours);
    if (minutes > 0)
        parts << QString("%1 мин").arg(minutes);
    if (seconds > 0 || parts.isEmpty())
        parts << QString("%1 с").arg(seconds);

    QString elapsedStr = parts.join(' ');
    QString infoText = tr("Прошло времени    %1").arg(elapsedStr) + \
        "\n" +         tr("Осталось примерно %1").arg(formatRemainingTime(remainingMinutes));

    ui->infoLBL->setText(infoText);
    this->setWindowTitle(formatRemainingTime(remainingMinutes));
    ui->infoLBL->show();
}
void MainWindow::handleMatrixChanged()
{
    QStringList rows = ui->matrixPTE->toPlainText().split('\n', Qt::SkipEmptyParts);
    int maxLen = 0;
    for (const QString& row : rows)
        maxLen = qMax(maxLen, row.length());
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

    ui->executePBN->setText(UIStrings::START_TEXT  );
    ui->executePBN->setToolTip(UIStrings::START_TOOLTIP);
    this->setWindowTitle(UIStrings::MAIN_TITLE  );
    if ( workerPtr->isCancelled() ) {
        workerPtr->uncancel();
        ui->infoLBL->setText(UIStrings::CANCEL_TEXT );
        return;
    }
    int days = elapsedSec / 86400;
    int hours = (elapsedSec % 86400) / 3600;
    int minutes = (elapsedSec % 3600) / 60;
    int seconds = elapsedSec % 60;
    QStringList parts;
    if (days > 0)
        parts << QString("%1 д").arg(days);
    if (hours > 0)
        parts << QString("%1 ч").arg(hours);
    if (minutes > 0)
        parts << QString("%1 мин").arg(minutes);
    if (seconds > 0 )
        parts << QString("%1 с").arg(seconds);
    if (parts.isEmpty()) {
        ui->infoLBL->setText("Готово. Расчёт занял < 1 с");
        return;
    }
    QString elapsedStr = parts.join(' ');
    ui->infoLBL->setText("Готово. Расчёт занял " + elapsedStr);


}



bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // При изменение размера обновляем подписи под графиком
    if (watched == ui->spectrumCPT && event->type() == QEvent::Resize) {
        QVector<float> vec(yCache.size());
        for (int i = 0; i < yCache.size(); ++i) {
            vec[i] = (float) yCache.at(i);
        }
        updatePlot(vec);
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

    // Убедимся, что у нас актуальные данные
    loadMatricesArray();

    int idx = findMatrixIndexByName(name);
    if (idx >= 0) {
        // уже есть — спросим, перезаписать ли
        const auto resp = QMessageBox::question(this, tr("Перезапись"),
            tr("Матрица с именем \"%1\" уже существует. Перезаписать?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (resp != QMessageBox::Yes)
            return;
    }

    // Собираем объект и записываем в массив (перезапись или добавление)
    QJsonObject obj;
    obj["matrixName"] = name;
    obj["matrix"] = ui->matrixPTE->toPlainText();

    if (idx >= 0)
        matrices[idx] = obj;
    else
        matrices.append(obj);

    // Сохраняем в QSettings
    saveMatricesArray();

    // Включаем подменю удаления (если оно было выключено)
    if (deleteMenu)
        deleteMenu->setEnabled(!matrices.isEmpty());

    // Небольшое уведомление пользователю (опционально)
    // QMessageBox::information(this, tr("Сохранено"), tr("Матрица \"%1\" сохранена.").arg(name));

    // (опционально) если меню сейчас открыто и ты хочешь его обновить немедленно:
    // if (matrixMenu && matrixMenu->isVisible()) {
    //     // закроем и откроем заново, чтобы вызвать aboutToShow
    //     matrixMenu->hide();
    //     matrixMenu->show();
    // }
}

void MainWindow::updatePlot(const QVector<float>& spectrum)
{
    if (spectrum.isEmpty()) return;
    
    
    const int size = spectrum.size();
    if (sizeCache != size) {
        sizeCache = size;
        xCache.resize(size);
        for (int i = 0; i < size; ++i) xCache[i] = i;
        yCache.resize(size);
        tickerCache.clear();
        tickerStepCache = -1;
    }

    int step = 1;
    int width = ui->spectrumCPT->width();
    double pixelsPerBar = double(width) / double(size);
    step = int(ceil(30.0 / pixelsPerBar));

    if (tickerStepCache != step || tickerCache.isNull()) {
        tickerStepCache = step;
        QVector<double> ticks;
        QVector<QString> labels = buildAxisLabels(size, step);
        for (int i = 0; i < size; ++i) if (i % step == 0) ticks << i;
        QSharedPointer<QCPAxisTickerText> tt(new QCPAxisTickerText);
        tt->addTicks(ticks, labels);
        tickerCache = tt;
        ui->spectrumCPT->xAxis->setTicker(tickerCache);
    }

    // fill yCache and compute min/max non-zero
    qint64 minIdx = -1;
    qint64 maxIdx = -1;
    double maxVal = 0.0;
    bool invalidSpectrum = false;
    for (int i = 0; i < size; ++i) {
       
        double v = double(spectrum.at(i));
        if (std::isinf(v) || std::isnan(v)) {
            invalidSpectrum = true;
        }
        yCache[i] = v;
        if (v != 0.0) {
            if (minIdx == -1) minIdx = i;
            maxIdx = i;
        }
        if (v > maxVal) maxVal = v;
    }

    QCPBars* bars = nullptr;
    if (ui->spectrumCPT->plottableCount() > 0)
        bars = qobject_cast<QCPBars*>(ui->spectrumCPT->plottable());

    if (invalidSpectrum) {
        msg->setVisible(true);
        if (bars)
            bars->setVisible(false);
        ui->spectrumCPT->replot();
        return;
    }
    else {
        msg->setVisible(false);
        if (bars)
            bars->setVisible(true);
    }
    if (bars) {
        bars->setData(xCache, yCache);

        QColor c = Colors[DefaultValues::SPECTRUM_COLOR];
        //c = QColor(Colors[settings->getHistoColorValue()]);
        //c.setAlpha( ( 100- settings->getTransparencyValue() )*255/100 );
        bars->setBrush(QBrush(c));
        bars->setPen(QPen(Qt::black));
    }
    
    // axes range
    if (minIdx == -1) ui->spectrumCPT->xAxis->setRange(0, size);
    else ui->spectrumCPT->xAxis->setRange( minIdx - 1, maxIdx + 1 );

    ui->spectrumCPT->yAxis->setRange(0.0, maxVal * 1.1);
    ui->spectrumCPT->yAxis->setNumberFormat("eb");
    ui->spectrumCPT->yAxis->setNumberPrecision(2);

    ui->spectrumCPT->replot(QCustomPlot::rpQueuedReplot);
}

QVector<QString> MainWindow::buildAxisLabels(int size, int step) const
{
    QVector<QString> labels;
    labels.reserve(size);
    for (int i = 0; i < size; ++i) {
        if (i % step == 0) labels << QString::number(i);
        //else labels << QString();
    }
    return labels;
}



// Применить настройки
void MainWindow::applySettings()
{
    applySpectrumColor();
}
// Применить цвет спектра
void MainWindow::applySpectrumColor()
{
    QSettings s;
    QColor c = Qt::blue;

    
    //c = QColor(Colors[settings->getHistoColorValue()]);
    //c.setAlpha( ( 100-settings->getTransparencyValue() )*255/100);
    if ( ui->spectrumCPT->plottableCount() > 0 ) {
        QCPBars *bars = qobject_cast<QCPBars*>(ui->spectrumCPT->plottable());
        if (bars) {
            bars->setBrush(QBrush(c));
            //bars->setPen(QPen(Qt::black));
            ui->spectrumCPT->replot();
        }
    }
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
    for (double v : std::as_const(yCache))
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
        QVector<float> spectrum;
        spectrum.reserve(values.size());
        for (const QVariant &v : values)
            spectrum.append(v.toFloat());
        updatePlot(spectrum);
    }
}

QString MainWindow::formatRemainingTime(int minutesTotal)
{
    if (minutesTotal <= 0) {
        return QObject::tr("Меньше минуты");
    }
    
    int days    = minutesTotal / (60 * 24);
    int hours   = (minutesTotal % (60 * 24)) / 60;
    int minutes = minutesTotal % 60;

    QStringList parts;
    if (days > 0)
        parts << QObject::tr("%1 дн").arg(days);
    if (hours > 0)
        parts << QObject::tr("%1 ч").arg(hours);
    if (minutes > 0 && days == 0) // минуты показываем только если меньше суток
        parts << QObject::tr("%1 мин").arg(minutes);

    return parts.join(" ");
}

void MainWindow::loadMatricesArray()
{
    matrices = QJsonArray();

    QSettings s;
    const QByteArray data = s.value(SettingsKeys::MATRICES_JSON).toByteArray();
    if (data.isEmpty())
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return;

    matrices = doc.array();
}

void MainWindow::saveMatricesArray()
{
    QSettings s;
    QJsonDocument doc(matrices);
    s.setValue(SettingsKeys::MATRICES_JSON, doc.toJson(QJsonDocument::Compact));
    s.sync();
}

void MainWindow::saveMatrixByName(const QString& name)
{
    if (name.isEmpty())
        return;

    // Текст матрицы из интерфейса
    const QString matrixText = ui->matrixPTE->toPlainText();

    // Новый объект JSON для этой матрицы
    QJsonObject matrixObj;
    matrixObj["matrixName"] = name;
    matrixObj["matrix"] = matrixText;

    // Проверяем, есть ли уже матрица с таким именем
    bool updated = false;
    for (int i = 0; i < matrices.size(); ++i) {
        const QJsonObject obj = matrices.at(i).toObject();
        if (obj["matrixName"].toString() == name) {
            matrices[i] = matrixObj;  // заменяем существующий
            updated = true;
            break;
        }
    }

    // Если не нашли — добавляем новую
    if (!updated)
        matrices.append(matrixObj);

    // Сохраняем массив обратно в настройки
    saveMatricesArray();
}

bool MainWindow::removeMatrixByName(const QString& name)
{
    int idx = findMatrixIndexByName(name);
    if (idx < 0) return false;
    matrices.removeAt(idx);
    saveMatricesArray();
    return true;
}

int MainWindow::findMatrixIndexByName(const QString& name)
{
    for (int i = 0; i < matrices.size(); ++i) {
        if (!matrices.at(i).isObject()) continue;
        QJsonObject obj = matrices.at(i).toObject();
        if (obj.value("matrixName").toString() == name) return i;
    }
    return -1;
}

QString MainWindow::getMatrixByName(const QString& name)
{
    int idx = findMatrixIndexByName(name);
    if (idx < 0) return QString();
    return matrices.at(idx).toObject().value("matrix").toString();
}

QStringList MainWindow::listMatrixNames()
{
    QStringList out;
    for (const QJsonValue& v : matrices) {
        if (!v.isObject()) continue;
        out << v.toObject().value("matrixName").toString();
    }
    return out;
}

QString MainWindow::defaultMatrixName()
{
    QString text = ui->matrixPTE->toPlainText();
    QStringList rows = text.split('\n', Qt::SkipEmptyParts);
    int maxLen = 0;
    for (const QString& row : rows)
        maxLen = qMax(maxLen, row.length());
    if (text.isEmpty()) return "(0,0)";

    int cols = 0;
    QStringList lines = text.split('\n');
    for (const QString& line : lines) {
        // элементы через пробел, таб или что у тебя в формате матрицы
        int cnt = line.split(QRegExp("\\s+"), Qt::SkipEmptyParts).size();
        if (cnt > cols) cols = cnt;
    }
    return QString("Матрица (%1,%2)").arg(maxLen).arg(rows.size());
}
