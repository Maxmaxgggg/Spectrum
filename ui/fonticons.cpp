#include "fonticons.h"

#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QWidget>

QIcon FluentIcons::icon(const QWidget* widget, const char* code, int size)
{
    bool ok = false;
    const uint value = QString::fromLatin1(code).toUInt(&ok, 16);
    if (!ok)
        return QIcon();

    const QString glyph = QString::fromUcs4(&value, 1);

    // Рисуем вдвое крупнее запрошенного: при экранном масштабировании Qt
    // уменьшает картинку, а не растягивает, и края остаются чёткими.
    const int side = size * 2;

    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font;
    // Фолбэк для Win10: те же коды есть в Segoe MDL2 Assets.
    font.setFamilies({ QStringLiteral("Segoe Fluent Icons"),
                       QStringLiteral("Segoe MDL2 Assets") });
    // Глиф чуть меньше холста: остаётся поле по краям, и на кнопке со значком
    // и подписью значок не липнет к тексту.
    font.setPixelSize(int(side * 0.78));
    painter.setFont(font);

    const QPalette palette = widget ? widget->palette() : QPalette();
    painter.setPen(palette.color(QPalette::WindowText));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, glyph);
    painter.end();

    return QIcon(pixmap);
}
