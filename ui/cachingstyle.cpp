#include "cachingstyle.h"

QIcon CachingStyle::standardIcon(StandardPixmap icon,
                                 const QStyleOption* option,
                                 const QWidget* widget) const
{
    const auto found = cache.constFind(int(icon));
    if (found != cache.constEnd())
        return *found;

    const QIcon made = QProxyStyle::standardIcon(icon, option, widget);
    cache.insert(int(icon), made);
    return made;
}
