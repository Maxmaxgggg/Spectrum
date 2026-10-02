#include "cachingstyle.h"

QIcon CachingStyle::standardIcon(StandardPixmap icon,
                                 const QStyleOption* option,
                                 const QWidget* widget) const
{
    const auto found = m_cache.constFind(int(icon));
    if (found != m_cache.constEnd())
        return *found;

    const QIcon made = QProxyStyle::standardIcon(icon, option, widget);
    m_cache.insert(int(icon), made);
    return made;
}
