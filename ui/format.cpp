#include "format.h"

#include <QStringList>

#include <cmath>

namespace {

struct Scale { double div; const char* suffix; };

// Перебирается сверху вниз до первого подходящего порядка.
const Scale SCALES[] = {
    { 1e12, "трлн" }, { 1e9, "млрд" }, { 1e6, "млн" }, { 1e3, "тыс" },
};

// Три значащих разряда: «4,48», «44,8», «448».
QString withPrecision(double value)
{
    const double magnitude = std::abs(value);
    const int precision = magnitude >= 100 ? 0 : (magnitude >= 10 ? 1 : 2);

    QString str = QString::number(value, 'f', precision);
    // Десятичная запятая: точка в русском тексте читается как разделитель тысяч.
    str.replace(QLatin1Char('.'), QLatin1Char(','));
    return str;
}

} // namespace

QString Format::duration(qint64 seconds)
{
    if (seconds < 0)
        seconds = 0;

    const qint64 days    = seconds / 86400;
    const qint64 hours   = (seconds % 86400) / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs    = seconds % 60;

    QStringList parts;
    if (days > 0)
        parts << QObject::tr("%1 д").arg(days);
    if (hours > 0)
        parts << QObject::tr("%1 ч").arg(hours);
    if (minutes > 0)
        parts << QObject::tr("%1 мин").arg(minutes);
    if (secs > 0 || parts.isEmpty())
        parts << QObject::tr("%1 с").arg(secs);

    return parts.join(QLatin1Char(' '));
}

QString Format::remainingTime(int minutesTotal)
{
    if (minutesTotal <= 0)
        return QObject::tr("Меньше минуты");

    const int days    = minutesTotal / (60 * 24);
    const int hours   = (minutesTotal % (60 * 24)) / 60;
    const int minutes = minutesTotal % 60;

    QStringList parts;
    if (days > 0)
        parts << QObject::tr("%1 дн").arg(days);
    if (hours > 0)
        parts << QObject::tr("%1 ч").arg(hours);
    // Минуты показываются только внутри суток: «3 дн 5 ч 12 мин» — ложная точность.
    if (minutes > 0 && days == 0)
        parts << QObject::tr("%1 мин").arg(minutes);

    return parts.join(QLatin1Char(' '));
}

QString Format::speed(double wordsPerSecond)
{
    for (const Scale& scale : SCALES) {
        if (wordsPerSecond >= scale.div) {
            return withPrecision(wordsPerSecond / scale.div)
                 + QLatin1Char(' ') + QString::fromUtf8(scale.suffix)
                 + QObject::tr(" слов/с");
        }
    }
    return withPrecision(wordsPerSecond) + QObject::tr(" слов/с");
}

QString Format::count(quint64 n)
{
    for (const Scale& scale : SCALES) {
        if (double(n) >= scale.div)
            return withPrecision(double(n) / scale.div)
                 + QLatin1Char(' ') + QString::fromUtf8(scale.suffix);
    }
    // До тысячи сокращать нечего, а «847,00» вместо «847» только мешает.
    return QString::number(n);
}

QString Format::groupDigits(const QString& digits)
{
    QString out;
    out.reserve(digits.size() + digits.size() / 3);

    int seen = 0;
    for (int i = digits.size() - 1; i >= 0; --i) {
        if (!digits.at(i).isDigit())          // знак или мусор — дальше не режем
            return digits;
        if (seen && seen % 3 == 0)
            out.prepend(QChar(0x00A0));       // неразрывный пробел: не переносится
        out.prepend(digits.at(i));
        ++seen;
    }
    return out;
}
