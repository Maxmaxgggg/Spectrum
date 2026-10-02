#include "spectrumplot.h"

#include "axisticks.h"

#include "qcustomplot.h"

#include <cmath>

namespace {

// Палитра столбцов. Выбор цвета из настроек пока не подключён — используется
// первый элемент.
const QStringList& palette()
{
    static const QStringList COLORS{ "Blue", "Green", "Red" };
    return COLORS;
}

// Целевое расстояние между подписями оси X в пикселях. Реже — ось выглядит
// пустой, чаще — подписи налезают друг на друга.
constexpr double LABEL_SPACING_PX = 30.0;

} // namespace

SpectrumPlot::SpectrumPlot(QCustomPlot* plot)
    : m_plot(plot)
    , m_barColor(palette().value(DefaultValues::SPECTRUM_COLOR))
{
    Q_ASSERT(plot);

    m_overflowMessage = new QCPItemText(plot);
    m_overflowMessage->position->setType(QCPItemPosition::ptAxisRectRatio);
    m_overflowMessage->position->setCoords(0.5, 0.5);
    m_overflowMessage->setPositionAlignment(Qt::AlignCenter);
    m_overflowMessage->setText(QObject::tr("Спектральные компоненты слишком велики\n"
                                           "Невозможно отобразить графически"));
    QFont messageFont;
    messageFont.setPointSize(12);
    messageFont.setBold(true);
    m_overflowMessage->setFont(messageFont);
    m_overflowMessage->setVisible(false);

    // Свой курсор обязателен. QMainWindow ставит курсор-разделитель на себя,
    // когда мышь над границей доков, и дочерние виджеты его наследуют, если
    // своего не задали. У текстовых полей курсор свой, а график донашивал
    // чужой: над ним показывалась стрелка растягивания, которая ничего не
    // делает.
    plot->setCursor(Qt::ArrowCursor);

    plot->xAxis->setTickLabelRotation(0);
    plot->xAxis->setLabel(QObject::tr("Вес кодового слова, w"));
    plot->yAxis->setLabel(QObject::tr("Число кодовых слов, A(w)"));
    // A(w) растёт экспоненциально с длиной кода, обычная запись нечитаема.
    plot->yAxis->setNumberFormat("eb");
    plot->yAxis->setNumberPrecision(2);

    m_bars = new QCPBars(plot->xAxis, plot->yAxis);
    m_bars->setPen(QPen(Qt::black));
    m_bars->setBrush(QBrush(m_barColor));
}

void SpectrumPlot::setSpectrum(const SpectrumFloat& spectrum)
{
    if (spectrum.isEmpty())
        return;

    const int size = spectrum.size();
    if (m_xValues.size() != size) {
        m_xValues.resize(size);
        for (int i = 0; i < size; ++i)
            m_xValues[i] = i;
        m_yValues.resize(size);

        // Подписи привязаны к числу столбцов — пересобрать в любом случае.
        m_ticker.clear();
        m_tickStep = -1;
    }

    m_firstNonZero = -1;
    m_lastNonZero  = -1;
    m_maxValue     = 0.0;
    m_hasNonFinite = false;

    for (int i = 0; i < size; ++i) {
        const double v = double(spectrum.at(i));
        if (std::isinf(v) || std::isnan(v))
            m_hasNonFinite = true;

        m_yValues[i] = v;

        if (v != 0.0) {
            if (m_firstNonZero == -1)
                m_firstNonZero = i;
            m_lastNonZero = i;
        }
        if (v > m_maxValue)
            m_maxValue = v;
    }

    // Закрытую панель не рисуем: данные сохранены, картинка соберётся при
    // показе. Иначе каждое обновление спектра тратилось бы на невидимое.
    if (!m_plot->isVisible()) {
        m_pendingData = true;
        return;
    }

    applyData();
    redrawGeometry();
}

void SpectrumPlot::refresh()
{
    if (m_yValues.isEmpty())
        return;

    if (m_pendingData) {
        m_pendingData = false;
        applyData();
    }

    // Только геометрия: при изменении размера спектр тот же, и перезаливать
    // точки незачем. Раньше здесь вызывался setData на каждое событие
    // изменения размера — а их при перетаскивании панели десятки в секунду,
    // и каждое копировало и сортировало весь контейнер точек заново.
    redrawGeometry();
}

void SpectrumPlot::updateTicker()
{
    const int size  = m_xValues.size();
    const int width = m_plot->width();

    // Ноль означает схлопнутый график: подписывать нечего. Вся арифметика
    // и её краевые случаи — в axisticks.h, там же и объяснение, почему это
    // отдельная функция.
    const int step = axisLabelStep(size, width, LABEL_SPACING_PX);
    if (step == 0)
        return;

    if (m_tickStep == step && !m_ticker.isNull())
        return;

    m_tickStep = step;

    QVector<double>  ticks;
    QVector<QString> labels;
    for (int i = 0; i < size; i += step) {
        ticks  << i;
        labels << QString::number(i);
    }

    QSharedPointer<QCPAxisTickerText> textTicker(new QCPAxisTickerText);
    textTicker->addTicks(ticks, labels);

    m_ticker = textTicker;
    m_plot->xAxis->setTicker(m_ticker);
}

// Сводит спектр к тому, что реально имеет смысл рисовать.
//
// Если весов не больше потолка — столбец на вес, один к одному. Иначе веса
// делятся на равные корзины и значения внутри складываются: график остаётся
// той же гистограммой, просто грубее. Складываются, а не усредняются, — тогда
// суммарная площадь по картинке равна общему числу кодовых слов.
void SpectrumPlot::applyData()
{
    if (m_hasNonFinite) {
        // Компоненты не поместились в float: рисовать нечего.
        m_overflowMessage->setVisible(true);
        m_bars->setVisible(false);
        return;
    }

    // Корзины раскладываются по занятому диапазону весов, а не по всей длине
    // спектра. Разница принципиальная: у частичного перебора длинного кода
    // спектр длиной в тысячу весов, а ненулевых из них восемь. Деление всей
    // тысячи на сто шестьдесят корзин загоняло эти восемь весов в две корзины
    // шириной по шесть весов — вместо графика получались два столбища во весь
    // экран.
    const int first = m_firstNonZero < 0 ? 0 : int(m_firstNonZero);
    const int last  = m_lastNonZero  < 0 ? m_yValues.size() - 1 : int(m_lastNonZero);
    const int span  = last - first + 1;

    // Весов на столбец — целое число, и одно на все столбцы. Дробный шаг
    // давал неравные корзины: при 162 весах и потолке 160 почти все корзины
    // выходили по одному весу, а две — по два. Такая корзина складывала пару
    // соседей и торчала пиком вдвое выше остальных. Сумма при этом сходилась,
    // но глазом это читалось как всплеск в спектре, которого нет.
    const int group = (m_maxBars > 0 && span > m_maxBars)
                    ? (span + m_maxBars - 1) / m_maxBars
                    : 1;
    const int bins  = (span + group - 1) / group;

    m_barX.resize(bins);
    m_barY.resize(bins);
    m_barMax   = 0.0;
    m_barWidth = group;

    for (int b = 0; b < bins; ++b) {
        // Последняя корзина может оказаться неполной: она приходится на хвост
        // диапазона, где значения нулевые, и на картинке этого не видно.
        const int from = first + b * group;
        const int to   = qMin(from + group, last + 1);

        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += m_yValues.at(i);

        // Точка ставится в середину корзины: столбец шириной m_barWidth тогда
        // накрывает ровно свой диапазон весов.
        m_barX[b] = (from + to - 1) / 2.0;
        m_barY[b] = sum;
        if (sum > m_barMax)
            m_barMax = sum;
    }

    m_overflowMessage->setVisible(false);
    m_bars->setVisible(true);
    m_bars->setWidth(m_barWidth);
    m_bars->setData(m_barX, m_barY);
    m_bars->setBrush(QBrush(m_barColor));
    m_bars->setPen(QPen(Qt::black));

    // Без сглаживания. Столбец занимает ровно свою корзину, но границы корзин
    // попадают на дробные доли пикселя, и сглаживание рисовало на стыке
    // полупрозрачную кромку — она и читалась как белый зазор между столбцами,
    // которые на самом деле идут вплотную.
    m_bars->setAntialiased(false);
}

void SpectrumPlot::setMaxBars(int limit)
{
    if (limit == m_maxBars)
        return;

    m_maxBars = limit;
    if (m_yValues.isEmpty())
        return;

    applyData();
    redrawGeometry();
}

void SpectrumPlot::redrawGeometry()
{
    updateTicker();

    if (m_hasNonFinite) {
        m_plot->replot();
        return;
    }

    // Хвосты нулей по краям не несут информации: показываем только занятый
    // диапазон весов, с запасом в один столбец с каждой стороны.
    if (m_firstNonZero == -1)
        m_plot->xAxis->setRange(0, m_xValues.size());
    else
        m_plot->xAxis->setRange(m_firstNonZero - 1, m_lastNonZero + 1);

    // Предел берётся по нарисованному: при группировке столбец — это сумма по
    // корзине, и она выше любого отдельного значения.
    m_plot->yAxis->setRange(0.0, m_barMax * 1.1);

    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void SpectrumPlot::setBarColor(const QColor& color)
{
    m_barColor = color;
    if (!m_bars)
        return;

    m_bars->setBrush(QBrush(m_barColor));
    m_plot->replot();
}
