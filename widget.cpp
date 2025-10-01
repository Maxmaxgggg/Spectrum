#include "widget.h"
#include "ui_widget.h"
#include "worker.h"
#include <QSettings>
#include <QMessageBox>
#include <QDateTime>
#include <QDebug>
#include "qcustomplot.h"
#include "defines.h"
QStringList Colors{ "Blue", "Green", "Red" };



Widget::Widget(QWidget *parent)
    : QWidget(parent),
    ui(new Ui::Widget)
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
    /********                      ********/
    if( settings == nullptr )
        settings = new SettingsDialog(this);

    // Инициализация QCustomPlot и QCPBars (предполагается, что в ui есть spectrumCPT)
    ui->spectrumCPT->setNoAntialiasingOnDrag(true);
    ui->spectrumCPT->xAxis->setTickLabelRotation(0);
    ui->spectrumCPT->yAxis->setNumberFormat("eb");
    ui->spectrumCPT->yAxis->setNumberPrecision(2);

    // Создаём QCPBars единожды (если в .ui Plottables уже нет)
    if ( spectrumBars == nullptr ) {
        spectrumBars = new QCPBars(ui->spectrumCPT->xAxis, ui->spectrumCPT->yAxis);
        spectrumBars->setPen(QPen(Qt::black));
        spectrumBars->setBrush(QBrush(Qt::blue));
    }
    connect( settings,      &SettingsDialog::settingsApplied,  this, &Widget::handleSettingsApplied   );
    connect( ui->matrixPTE, &FilterPlainTextEdit::textChanged, this,  &Widget::handleMatrixChanged    );
    loadSettings();
    ui->spectrumCPT->installEventFilter(this);
    Worker*  worker       = new Worker;
    QThread* workerThread = new QThread(this);
    setWorker( worker,workerThread );
    settings->setStringsSPBEnabled( true );
    settings->setUseGpuCHBEnabled(  true );
}

Widget::~Widget()
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
    this->setWindowTitle(MAIN_TITLE);
    delete ui;
}

void Widget::setWorker(Worker* worker, QThread* workerThread)
{
    if (!worker) return;
    workerPtr = worker;
    workerThreadPtr = workerThread;
    workerPtr->moveToThread(workerThread);

    connect( workerPtr,       &Worker::updateInfoPBR,          this, &Widget::handleUpdateInfoPBR,          Qt::QueuedConnection );
    connect( workerPtr,       &Worker::updateSpectrumPlot,     this, &Widget::handleUpdateSpectrumPlot,     Qt::QueuedConnection );
    connect( workerPtr,       &Worker::updateSpectrumPTE,      this, &Widget::handleUpdateSpectrumPTE,      Qt::QueuedConnection );
    connect( workerPtr,       &Worker::updateRemainingMinutes, this, &Widget::handleUpdateRemainingMinutes, Qt::QueuedConnection );
    connect( workerPtr,       &Worker::errorOccurred,          this, &Widget::handleError,                  Qt::QueuedConnection );
    connect( workerPtr,       &Worker::GPUnotFound,            this, &Widget::handleGPUnotFound,            Qt::QueuedConnection );
    connect( workerPtr,       &Worker::finished,               this, &Widget::handleFinished,               Qt::QueuedConnection );
}


void Widget::on_executePBN_clicked()
{
    switch (runState) {
        case RunState::Idle: {
            settings->setStringsSPBEnabled( false );
            settings->setUseGpuCHBEnabled(  false );
            ui->matrixPTE->setReadOnly(true);
            ui->infoLBL->setText("");
            ui->infoLBL->show();
            ui->infoPBR->setValue(0);
            QStringList rows = ui->matrixPTE->toPlainText().split('\n', Qt::SkipEmptyParts);
            if (rows.isEmpty()) {
                QMessageBox::warning(this, ERROR_TITLE, "Матрица пустая");
                return;
            }
            qint64 maxComb = 0;
            if (settings) maxComb = settings->stringValue();
            if (maxComb <= 0 || maxComb > rows.size()) maxComb = rows.size();
            if (!workerPtr) {
                QMessageBox::warning(this, ERROR_TITLE, "Worker не подключён");
                return;
            }
            int maxThreads = omp_get_max_threads();
            int workerThreads = std::max(1, maxThreads - 1);
            omp_set_num_threads(maxThreads);

            workerThreadPtr->start();
            QMetaObject::invokeMethod(workerPtr, "computeSpectrum", Qt::QueuedConnection,
                Q_ARG(QStringList, rows), Q_ARG(quint64, (qulonglong)maxComb));

            runState = RunState::Running;
            ui->executePBN->setText(PAUSE_TEXT);
        } break;

        case RunState::Running: {
            if (workerPtr)
                workerPtr->pause();

            runState = RunState::Paused;
            this->setWindowTitle(PAUSE_TEXT);
            ui->executePBN->setText(CONTINUE_TEXT);
        } break;

        case RunState::Paused: {
            if (workerPtr)
                workerPtr->resume();

            runState = RunState::Running;
            ui->executePBN->setText(PAUSE_TEXT);
            if (remainingMinutes != -1)
                this->setWindowTitle(formatRemainingTime(remainingMinutes));
            else
                this->setWindowTitle(MAIN_TITLE);
        } break;


    } 
}

void Widget::on_exitPBN_clicked()
{
    if( workerPtr ){
        workerPtr->cancel();
    }
    if( workerThreadPtr ){
        workerThreadPtr->quit();
        workerThreadPtr->wait();
    }
    settings->setStringsSPBEnabled(true);
    settings->setUseGpuCHBEnabled(true);
    saveSettings();
    ui->matrixPTE->setReadOnly(false);
    qApp->exit();
}

void Widget::on_settingsPBN_clicked()
{

    settings->exec();
    applySettings();
}

void Widget::handleSettingsApplied()
{
    if(workerPtr){
        QSettings s;
        quint64 refreshProgressbarMs    = s.value( PROGRESSBAR_MS,     1000 ).toULongLong();
        quint64 refreshSpectrumMs       = s.value( SPECTRUM_MS,        1000 ).toULongLong();
        bool    USE_GPU                 = s.value( USE_GPU_CHECKED,    true ).toBool();
        workerPtr->useGPU( USE_GPU );
        workerPtr->refreshSpectrumMs    = refreshSpectrumMs;
        workerPtr->refreshProgressbarMs = refreshProgressbarMs;
        
    }
}

void Widget::handleStrValChanged()
{
    if( workerPtr )
        workerPtr->cancel();
    if( workerThreadPtr ){
        workerThreadPtr->quit();
        workerThreadPtr->wait();
    }
    runState = RunState::Idle;
    ui->infoPBR->setValue(0);
    ui->executePBN->setText(START_TEXT);

}

//
// Worker signal handlers
//

void Widget::handleUpdateInfoPBR(int percent)
{
    ui->infoPBR->setValue(percent);
}

void Widget::handleUpdateSpectrumPlot(const QVector<quint64>& spectrum)
{
    updatePlotFast(spectrum);
}

void Widget::handleUpdateSpectrumPTE(const QVector<quint64> &spectrum)
{

    QString str = "";
    for (qint64 w = 0; w < spectrum.size(); ++w){
        if( spectrum.at(w) != 0){
            str += QString::number(w) + " - " + QString::number(spectrum.at(w)) + '\n';
           }
       }
    if (!str.isEmpty() && str.endsWith('\n'))
        str.chop(1);
    QScrollBar *vbar = ui->spectrumPTE->verticalScrollBar();
    int pos = vbar->value();
    ui->spectrumPTE->setPlainText( str );
    vbar->setValue(pos);
}
void Widget::handleUpdateRemainingMinutes(int minutesLeft)
{
    remainingMinutes = minutesLeft;
    ui->infoLBL->setText(tr("Осталось примерно %1").arg(formatRemainingTime(remainingMinutes)));
    this->setWindowTitle(formatRemainingTime(remainingMinutes));
    ui->infoLBL->show();
}
void Widget::handleMatrixChanged()
{
    QStringList rows = ui->matrixPTE->toPlainText().split('\n', Qt::SkipEmptyParts);
    settings->setStringsSPBMaxValue(rows.size());
}
void Widget::handleError(const QString& message)
{
    QMessageBox::critical(this, ERROR_TITLE, message);
    // reset UI
    runState = RunState::Idle;
    ui->executePBN->setText(START_TEXT);
}
void Widget::handleGPUnotFound()
{
    QMessageBox::warning(this, WARNING_TITLE, "GPU не найдено. Расчеты будут производиться на CPU");
    settings->setUseGpuCHBChecked( false );
}
void Widget::handleFinished()
{
    runState = RunState::Idle;
    if(workerThreadPtr->isRunning()){
        workerThreadPtr->quit();
        workerThreadPtr->wait();
    }

    settings->setStringsSPBEnabled(   true        );
    settings->setUseGpuCHBEnabled (   true        );
    ui->matrixPTE->setReadOnly(       false       );

    ui->executePBN->setText(          START_TEXT  );
    ui->infoLBL->setText(             READY_TEXT  );
    this->setWindowTitle(             MAIN_TITLE  );
}



bool Widget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->spectrumCPT && event->type() == QEvent::Resize) {
        QVector<quint64> vec(yCache.size());
        for (int i = 0; i < yCache.size(); ++i) {
            vec[i] = (quint64) yCache.at(i);
        }
        updatePlotFast(vec);
        return false;
    }

    // Для всех остальных событий — стандартная обработка
    return QWidget::eventFilter(watched, event);
}

void Widget::updatePlotFast(const QVector<quint64>& spectrum)
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
    if (width < 200) step = 10;
    else if (width < 800) step = 5;

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
    for (int i = 0; i < size; ++i) {
        double v = double(spectrum.at(i));
        yCache[i] = v;
        if (v != 0.0) {
            if (minIdx == -1) minIdx = i;
            maxIdx = i;
        }
        if (v > maxVal) maxVal = v;
    }

    // set data to bars
    if ( ui->spectrumCPT->plottableCount() > 0 ) {
        QCPBars *bars = qobject_cast<QCPBars*>(ui->spectrumCPT->plottable());
        if (bars) {
            bars->setData(xCache, yCache);
            // color from settings
            QSettings s;
            QColor c = Qt::blue;
            if (s.contains(SPECTRUM_COLOR))
                c = QColor(Colors[s.value(SPECTRUM_COLOR).toInt()]);
            c.setAlpha( ( 100-s.value(TRANSPARENCY_VALUE).toInt() )*255/100 );
            bars->setBrush(QBrush(c));
            bars->setPen(QPen(Qt::black));
        }
    }

    // axes range
    if (minIdx == -1) ui->spectrumCPT->xAxis->setRange(0, size);
    else ui->spectrumCPT->xAxis->setRange( minIdx - 1, maxIdx + 1 );

    ui->spectrumCPT->yAxis->setRange(0.0, maxVal * 1.1);
    ui->spectrumCPT->yAxis->setNumberFormat("eb");
    ui->spectrumCPT->yAxis->setNumberPrecision(2);

    ui->spectrumCPT->replot(QCustomPlot::rpQueuedReplot);
}

QVector<QString> Widget::buildAxisLabels(int size, int step) const
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
void Widget::applySettings()
{
    QSettings s;
    
    progressBarMs     = s.value( PROGRESSBAR_MS, progressBarMs     ).toInt();
    spectrumMs        = s.value( SPECTRUM_MS,    spectrumMs        ).toInt();
    applySpectrumColor();
}
// Применить цвет спектра
void Widget::applySpectrumColor()
{
    QSettings s;
    QColor c = Qt::blue;

    if (s.contains(SPECTRUM_COLOR)) c = QColor(Colors[s.value(SPECTRUM_COLOR).toInt()]);
    c.setAlpha( (100-s.value(TRANSPARENCY_VALUE).toInt())*255/100);
    if ( ui->spectrumCPT->plottableCount() > 0 ) {
        QCPBars *bars = qobject_cast<QCPBars*>(ui->spectrumCPT->plottable());
        if (bars) {
            bars->setBrush(QBrush(c));
            bars->setPen(QPen(Qt::black));
            ui->spectrumCPT->replot();
        }
    }
}
// Сохранить настройки в реестр
void Widget::saveSettings()
{
    QSettings s;
    if( splitter )
        s.setValue( SPLITTER_STATE,  this->splitter->saveState()    );
    s.setValue(     CODE_MATRIX,     ui->matrixPTE->toPlainText()   );
    s.setValue(     SPECTRUM_TEXT,   ui->spectrumPTE->toPlainText() );
    s.setValue(     WIDGET_GEOMETRY, this->saveGeometry()           );
    s.setValue(     PROGRESSBAR_MS,  progressBarMs                  );
    s.setValue(     SPECTRUM_MS,     spectrumMs                     );
    QVariantList values;
    for (double v : std::as_const(yCache))
        values << v;
    s.setValue( SPECTRUM_VALUES, values);
    s.sync();
}

// Загрузить настройки из реестра
void Widget::loadSettings()
{
    QSettings s;

    this->restoreGeometry(                    s.value( WIDGET_GEOMETRY                ).toByteArray()       );
    if( splitter ) splitter->restoreState(    s.value( SPLITTER_STATE                 ).toByteArray()       );
    ui->spectrumPTE->setPlainText(            s.value( SPECTRUM_TEXT                  ).toString()          );
    ui->matrixPTE->setPlainText(              s.value( CODE_MATRIX                    ).toString()          );
    progressBarMs     =                       s.value( PROGRESSBAR_MS, progressBarMs  ).toInt();
    spectrumMs        =                       s.value( SPECTRUM_MS,    spectrumMs     ).toInt();

    if (s.contains(SPECTRUM_VALUES)) {
        QVariantList values = s.value(SPECTRUM_VALUES).toList();
        QVector<quint64> spectrum;
        spectrum.reserve(values.size());
        for (const QVariant &v : values)
            spectrum.append(v.toULongLong());
        // сразу передаём в updatePlotFast
        updatePlotFast(spectrum);
    }
}

QString Widget::formatRemainingTime(int minutesTotal)
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
