#include "tabswapbutton.h"

#include "fluenticons.h"

#include <QEvent>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QTabBar>
#include <QToolButton>

namespace {

// В какой близости от стыка вкладок кнопка показывается, пикселей.
constexpr int REACH = 28;

}

TabSwapButton::TabSwapButton(QTabBar* bar, const QString& tip)
    : QObject(bar), bar(bar), button(new QToolButton(bar))
{
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    // «Switch» из Segoe Fluent Icons: две стрелки навстречу.
    button->setIcon(FluentIcons::icon(bar, "E8AB"));
    button->hide();
    connect(button, &QToolButton::clicked, this, &TabSwapButton::clicked);

    bar->setAttribute(Qt::WA_Hover, true);
    bar->installEventFilter(this);
    place();
}

// Ставит кнопку на стык первой и второй вкладки.
void TabSwapButton::place()
{
    if (bar->count() < 2)
        return;
    const QRect left = bar->tabRect(0);
    const int   seam = left.right() + 1;
    const int   size = qMax(16, bar->height() - 2);
    button->setIconSize(QSize(size * 2 / 3, size * 2 / 3));
    button->resize(size, size);
    button->move(seam - size / 2, (bar->height() - size) / 2);
    button->raise();
}

void TabSwapButton::showIfNear(const QPoint& pos)
{
    if (bar->count() < 2) {
        button->hide();
        return;
    }
    const QRect zone = button->geometry().adjusted(-REACH, -REACH, REACH, REACH);
    button->setVisible(zone.contains(pos));
}

bool TabSwapButton::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == bar) {
        switch (event->type()) {
            case QEvent::Resize:
            case QEvent::LayoutRequest:
            case QEvent::Show:
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
                if (!button->underMouse())
                    button->hide();
                break;
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}
