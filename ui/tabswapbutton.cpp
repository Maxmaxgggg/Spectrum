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
    : QObject(bar), m_bar(bar), m_button(new QToolButton(bar))
{
    m_button->setAutoRaise(true);
    m_button->setFocusPolicy(Qt::NoFocus);
    m_button->setToolTip(tip);
    m_button->setCursor(Qt::PointingHandCursor);
    // «Switch» из Segoe Fluent Icons: две стрелки навстречу.
    m_button->setIcon(FluentIcons::icon(bar, "E8AB"));
    m_button->hide();
    connect(m_button, &QToolButton::clicked, this, &TabSwapButton::clicked);

    bar->setAttribute(Qt::WA_Hover, true);
    bar->installEventFilter(this);
    place();
}

// Ставит кнопку на стык первой и второй вкладки.
void TabSwapButton::place()
{
    if (m_bar->count() < 2)
        return;
    const QRect left = m_bar->tabRect(0);
    const int   seam = left.right() + 1;
    const int   size = qMax(16, m_bar->height() - 2);
    m_button->setIconSize(QSize(size * 2 / 3, size * 2 / 3));
    m_button->resize(size, size);
    m_button->move(seam - size / 2, (m_bar->height() - size) / 2);
    m_button->raise();
}

void TabSwapButton::showIfNear(const QPoint& pos)
{
    if (m_bar->count() < 2) {
        m_button->hide();
        return;
    }
    const QRect zone = m_button->geometry().adjusted(-REACH, -REACH, REACH, REACH);
    m_button->setVisible(zone.contains(pos));
}

bool TabSwapButton::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_bar) {
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
                if (!m_button->underMouse())
                    m_button->hide();
                break;
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}
