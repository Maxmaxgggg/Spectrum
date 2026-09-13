#include "dockswapbutton.h"

#include "fonticons.h"

#include <QDockWidget>
#include <QEvent>
#include <QHoverEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>

namespace {

// В какой близости от стыка вкладок кнопка показывается, пикселей.
constexpr int kReach = 28;

}

DockSwapButton::DockSwapButton(QMainWindow* window, QDockWidget* first, QDockWidget* second,
                               const QString& tip)
    : QObject(window), window(window), first(first), second(second), tip(tip)
{
    // Полоса вкладок собирается не сразу, а при перекладке: после любого
    // движения доков ищем её заново, чуть погодя.
    auto rearm = [this]() { QTimer::singleShot(0, this, [this]() { attach(); }); };
    for (QDockWidget* dock : { first, second }) {
        connect(dock, &QDockWidget::dockLocationChanged, this, [rearm](Qt::DockWidgetArea) { rearm(); });
        connect(dock, &QDockWidget::topLevelChanged,     this, [rearm](bool) { rearm(); });
        connect(dock, &QDockWidget::visibilityChanged,   this, [rearm](bool) { rearm(); });
    }
}

// Qt кладёт в данные вкладки указатель на её док — по нему и ищем.
QTabBar* DockSwapButton::findTabBar() const
{
    const QList<QTabBar*> bars = window->findChildren<QTabBar*>();
    for (QTabBar* candidate : bars) {
        bool hasFirst = false, hasSecond = false;
        for (int i = 0; i < candidate->count(); ++i) {
            const quintptr id = candidate->tabData(i).value<quintptr>();
            if (id == quintptr(first))  hasFirst  = true;
            if (id == quintptr(second)) hasSecond = true;
        }
        if (hasFirst && hasSecond)
            return candidate;
    }
    return nullptr;
}

void DockSwapButton::attach()
{
    QTabBar* found = findTabBar();
    if (found == bar && (button || !found))
        return;

    if (bar) {
        bar->removeEventFilter(this);
        bar->setAttribute(Qt::WA_Hover, false);
    }
    delete button;
    bar = found;
    if (!bar)
        return;

    // Кнопка — ребёнок полосы: уходит вместе с ней, когда Qt её пересобирает.
    button = new QToolButton(bar);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    // «Switch» из Segoe Fluent Icons: две стрелки навстречу.
    button->setIcon(FluentIcons::icon(bar, "E8AB"));
    button->hide();
    connect(button, &QToolButton::clicked, this, &DockSwapButton::clicked);

    bar->setAttribute(Qt::WA_Hover, true);
    bar->installEventFilter(this);
    place();
}

// Ставит кнопку на стык вкладок первого и второго дока — какая из них левее,
// неважно.
void DockSwapButton::place()
{
    if (!bar || !button)
        return;
    int a = -1, b = -1;
    for (int i = 0; i < bar->count(); ++i) {
        const quintptr id = bar->tabData(i).value<quintptr>();
        if (id == quintptr(first))  a = i;
        if (id == quintptr(second)) b = i;
    }
    if (a < 0 || b < 0)
        return;
    const QRect left  = bar->tabRect(qMin(a, b));
    const int   seam  = left.right() + 1;
    const int   size  = qMax(16, bar->height() - 2);
    button->setIconSize(QSize(size * 2 / 3, size * 2 / 3));
    button->resize(size, size);
    button->move(seam - size / 2, (bar->height() - size) / 2);
    button->raise();
}

void DockSwapButton::showIfNear(const QPoint& pos)
{
    if (!button)
        return;
    const QRect zone = button->geometry().adjusted(-kReach, -kReach, kReach, kReach);
    button->setVisible(zone.contains(pos));
}

bool DockSwapButton::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == bar) {
        switch (event->type()) {
            case QEvent::Resize:
            case QEvent::LayoutRequest:
                place();
                break;
            case QEvent::HoverEnter:
            case QEvent::HoverMove:
                place();
                showIfNear(static_cast<QHoverEvent*>(event)->pos());
                break;
            case QEvent::MouseMove:
                showIfNear(static_cast<QMouseEvent*>(event)->pos());
                break;
            case QEvent::HoverLeave:
            case QEvent::Leave:
                if (button && !button->underMouse())
                    button->hide();
                break;
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}
