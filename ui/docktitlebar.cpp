#include "docktitlebar.h"

#include <QDockWidget>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QStyle>
#include <QStyleOptionDockWidget>
#include <QStylePainter>
#include <QToolButton>

DockTitleBar::DockTitleBar(QDockWidget* owner)
    : QWidget(owner), dock(owner)
{
    const int margin = style()->pixelMetric(QStyle::PM_DockWidgetTitleBarButtonMargin, nullptr, this);

    auto* const row = new QHBoxLayout(this);
    row->setContentsMargins(margin, margin, margin, margin);
    row->setSpacing(0);
    row->addStretch(1);

    auto makeButton = [this, row](QStyle::StandardPixmap pixmap, const QString& tip) {
        auto* const button = new QToolButton(this);
        button->setAutoRaise(true);          // как у штатных кнопок дока: рамка только под курсором
        button->setFocusPolicy(Qt::NoFocus);
        button->setIcon(style()->standardIcon(pixmap, nullptr, this));
        button->setToolTip(tip);
        row->addWidget(button);
        return button;
    };

    floatButton = makeButton(QStyle::SP_TitleBarNormalButton, tr("В отдельное окно"));
    closeButton = makeButton(QStyle::SP_TitleBarCloseButton,  tr("Закрыть панель"));

    connect(floatButton, &QToolButton::clicked, this,
            [this]() { dock->setFloating(!dock->isFloating()); });
    connect(closeButton, &QToolButton::clicked, dock, &QDockWidget::close);

    connect(dock, &QDockWidget::featuresChanged, this, [this]() { updateButtons(); });
    connect(dock, &QDockWidget::windowTitleChanged, this, [this](const QString&) { update(); });
    connect(dock, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        floatButton->setToolTip(floating ? tr("Вернуть на место") : tr("В отдельное окно"));
        update();
    });
    // Сложили с другой панелью или разложили — название то появляется на
    // вкладке, то нет; перерисовать.
    connect(dock, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea) { update(); });
    connect(dock, &QDockWidget::visibilityChanged,   this, [this](bool) { update(); });

    updateButtons();
}

int DockTitleBar::buttonExtent() const
{
    const int icon   = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    const int margin = style()->pixelMetric(QStyle::PM_DockWidgetTitleBarButtonMargin, nullptr, this);

    // Две трети от того, что насчитал стиль: его размер рассчитан на заголовок
    // окна, а здесь полоса тонкая, и кнопки в полный рост её распирают. Дробь
    // одна на кнопку и значок — крутится отсюда.
    return (icon + 2 * margin) * 2 / 3;
}

void DockTitleBar::updateButtons()
{
    const QDockWidget::DockWidgetFeatures features = dock->features();
    const int extent = buttonExtent();

    for (QToolButton* button : { floatButton, closeButton }) {
        // Значок занимает две трети кнопки, остальное — поле, по которому под
        // курсором видно рамку.
        button->setIconSize(QSize(extent * 2 / 3, extent * 2 / 3));
        button->setFixedSize(extent, extent);
    }

    floatButton->setVisible(features & QDockWidget::DockWidgetFloatable);
    closeButton->setVisible(features & QDockWidget::DockWidgetClosable);

    updateGeometry();
    update();
}

void DockTitleBar::initStyleOption(QStyleOptionDockWidget* option) const
{
    option->initFrom(this);
    option->rect = rect();

    const QDockWidget::DockWidgetFeatures features = dock->features();
    option->closable  = features & QDockWidget::DockWidgetClosable;
    option->floatable = features & QDockWidget::DockWidgetFloatable;
    option->movable   = features & QDockWidget::DockWidgetMovable;
    // Вертикальных заголовков в окне нет: панели делятся только по горизонтали
    // и вертикали, боком названия не встают.
    option->verticalTitleBar = false;

    // Стиль рисует название по всей отданной ему ширине и на длинном заезжает
    // под кнопки. Место под них считаем сами и обрезаем название по нему.
    int reserved = 0;
    for (const QToolButton* button : { floatButton, closeButton }) {
        if (button->isVisible())
            reserved += button->width();
    }
    reserved += 2 * style()->pixelMetric(QStyle::PM_DockWidgetTitleBarButtonMargin, nullptr, this);

    option->title = tabified() ? QString()
                               : fontMetrics().elidedText(dock->windowTitle(), Qt::ElideRight,
                                                          qMax(0, width() - reserved));
}

bool DockTitleBar::tabified() const
{
    const auto* window = qobject_cast<const QMainWindow*>(dock->parentWidget());
    if (!window || dock->isFloating())
        return false;
    // Спрятанный сосед по группе в списке остаётся, а вкладок при нём нет —
    // считаем только видимых.
    const QList<QDockWidget*> neighbours = window->tabifiedDockWidgets(dock);
    for (const QDockWidget* other : neighbours)
        if (other->isVisible())
            return true;
    return false;
}

void DockTitleBar::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QStylePainter painter(this);
    QStyleOptionDockWidget option;
    initStyleOption(&option);
    painter.drawControl(QStyle::CE_DockWidgetTitle, option);
}

QSize DockTitleBar::sizeHint() const
{
    const int margin = style()->pixelMetric(QStyle::PM_DockWidgetTitleBarButtonMargin, nullptr, this);
    const int height = qMax(buttonExtent(), fontMetrics().height()) + 2 * margin;

    // Ширина берётся по названию с местом под кнопки; растягивать заголовок
    // будет раскладка панели.
    const int width = fontMetrics().horizontalAdvance(dock->windowTitle())
                    + 2 * buttonExtent() + 4 * margin;
    return QSize(width, height);
}

QSize DockTitleBar::minimumSizeHint() const
{
    return QSize(2 * buttonExtent(), sizeHint().height());
}

// Событий мыши здесь нет намеренно. QWidget их не принимает, и нажатие на
// пустом месте заголовка доходит до самого дока — тем и работают перетаскивание
// панели и двойной щелчок, вытаскивающий её в окно.
