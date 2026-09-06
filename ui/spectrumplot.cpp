#include "spectrumplot.h"

#include "axisticks.h"

#include "qcustomplot.h"

#include <cmath>

namespace {

// Палитра столбцов. Выбор цвета из настроек пока не подключён — используется
// первый элемент.
const QStringList& palette()
{
    static const QStringList colors{ "Blue", "Green", "Red" };
    return colors;
}

// Целевое расстояние между подписями оси X в пикселях. Реже — ось выглядит
// пустой, чаще — подписи налезают друг на друга.
constexpr double LABEL_SPACING_PX = 30.0;

} // namespace

SpectrumPlot::SpectrumPlot(QCustomPlot* plot)
    : plot(plot)
    , barColor(palette().value(DefaultValues::SPECTRUM_COLOR))
{
    Q_ASSERT(plot);

    overflowMessage = new QCPItemText(plot);
    overflowMessage->position->setType(QCPItemPosition::ptAxisRectRatio);
    overflowMessage->position->setCoords(0.5, 0.5);
    overflowMessage->setPositionAlignment(Qt::AlignCenter);
    overflowMessage->setText(QObject::tr("Спектральные компоненты слишком велики\n"
                                         "Невозможно отобразить графически"));
    QFont messageFont;
    messageFont.setPointSize(12);
    messageFont.setBold(true);
    overflowMessage->setFont(messageFont);
    overflowMessage->setVisible(false);

    plot->xAxis->setTickLabelRotation(0);
    plot->xAxis->setLabel(QObject::tr("Вес кодового слова, w"));
    plot->yAxis->setLabel(QObject::tr("Число кодовых слов, A(w)"));
    // A(w) растёт экспоненциально с длиной кода, обычная запись нечитаема.
    plot->yAxis->setNumberFormat("eb");
    plot->yAxis->setNumberPrecision(2);

    bars = new QCPBars(plot->xAxis, plot->yAxis);
    bars->setPen(QPen(Qt::black));
    bars->setBrush(QBrush(barColor));
}

void SpectrumPlot::setSpectrum(const SpectrumFloat& spectrum)
{
    if (spectrum.isEmpty())
        return;

    const int size = spectrum.size();
    if (xValues.size() != size) {
        xValues.resize(size);
        for (int i = 0; i < size; ++i)
            xValues[i] = i;
        yValues.resize(size);

        // Подписи привязаны к числу столбцов — пересобрать в любом случае.
        ticker.clear();
        tickStep = -1;
    }

    firstNonZero = -1;
    lastNonZero  = -1;
    maxValue     = 0.0;
    hasNonFinite = false;

    for (int i = 0; i < size; ++i) {
        const double v = double(spectrum.at(i));
        if (std::isinf(v) || std::isnan(v))
            hasNonFinite = true;

        yValues[i] = v;

        if (v != 0.0) {
            if (firstNonZero == -1)
                firstNonZero = i;
            lastNonZero = i;
        }
        if (v > maxValue)
            maxValue = v;
    }

    applyData();
    redrawGeometry();
}

void SpectrumPlot::refresh()
{
    if (yValues.isEmpty())
        return;

    // Только геометрия: при изменении размера спектр тот же, и перезаливать
    // точки незачем. Раньше здесь вызывался setData на каждое событие
    // изменения размера — а их при перетаскивании панели десятки в секунду,
    // и каждое копировало и сортировало весь контейнер точек заново.
    redrawGeometry();
}

void SpectrumPlot::updateTicker()
{
    const int size  = xValues.size();
    const int width = plot->width();

    // Ноль означает схлопнутый график: подписывать нечего. Вся арифметика
    // и её краевые случаи — в axisticks.h, там же и объяснение, почему это
    // отдельная функция.
    const int step = axisLabelStep(size, width, LABEL_SPACING_PX);
    if (step == 0)
        return;

    if (tickStep == step && !ticker.isNull())
        return;

    tickStep = step;

    QVector<double>  ticks;
    QVector<QString> labels;
    for (int i = 0; i < size; i += step) {
        ticks  << i;
        labels << QString::number(i);
    }

    QSharedPointer<QCPAxisTickerText> textTicker(new QCPAxisTickerText);
    textTicker->addTicks(ticks, labels);

    ticker = textTicker;
    plot->xAxis->setTicker(ticker);
}

void SpectrumPlot::applyData()
{
    if (hasNonFinite) {
        // Компоненты не поместились в float: рисовать нечего.
        overflowMessage->setVisible(true);
        bars->setVisible(false);
        return;
    }

    overflowMessage->setVisible(false);
    bars->setVisible(true);
    bars->setData(xValues, yValues);
    bars->setBrush(QBrush(barColor));
    bars->setPen(QPen(Qt::black));
}

void SpectrumPlot::redrawGeometry()
{
    updateTicker();

    if (hasNonFinite) {
        plot->replot();
        return;
    }

    // Хвосты нулей по краям не несут информации: показываем только занятый
    // диапазон весов, с запасом в один столбец с каждой стороны.
    if (firstNonZero == -1)
        plot->xAxis->setRange(0, xValues.size());
    else
        plot->xAxis->setRange(firstNonZero - 1, lastNonZero + 1);

    plot->yAxis->setRange(0.0, maxValue * 1.1);

    plot->replot(QCustomPlot::rpQueuedReplot);
}

void SpectrumPlot::setBarColor(const QColor& color)
{
    barColor = color;
    if (!bars)
        return;

    bars->setBrush(QBrush(barColor));
    plot->replot();
}
