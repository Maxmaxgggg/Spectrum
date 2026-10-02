#pragma once

#include <QHash>
#include <QIcon>
#include <QProxyStyle>

// Стиль, который помнит выданные им значки.
//
// Зачем. Заголовок каждого дока при переразметке спрашивает у стиля
// прямоугольники своих кнопок, а QCommonStyle::subElementRect ради этого
// каждый раз строит значок заново — через QIcon::addFile. В статически
// собранном Qt любое построение значка из файла заставляет QImageReader
// искать подходящий обработчик, а тот заново разбирает JSON-метаданные всех
// вкомпилированных плагинов.
//
// Замерено сэмплирующим профайлером: при перетаскивании разделителя GUI-поток
// проводил в этом разборе больше трети времени, и стоимость росла с числом
// панелей — 11 событий изменения размера в секунду при трёх доках против 30
// при одном.
//
// Значки стиля не зависят ни от состояния виджета, ни от палитры, поэтому
// хранить их по одному номеру достаточно.
class CachingStyle : public QProxyStyle
{
    Q_OBJECT

public:
    using QProxyStyle::QProxyStyle;

    QIcon standardIcon(StandardPixmap icon,
                       const QStyleOption* option = nullptr,
                       const QWidget* widget = nullptr) const override;

private:
    mutable QHash<int, QIcon> cache;
};
