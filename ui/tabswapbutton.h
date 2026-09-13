#pragma once

#include <QObject>

class QTabBar;
class QToolButton;

// Кнопка «поменять местами» на стыке первых двух вкладок полосы.
//
// Своих кнопок между вкладками у QTabBar не бывает, поэтому кнопка — накладка
// поверх полосы: стоит на стыке вкладок и показывается, только когда курсор
// рядом. Нужна матрицам кода произведения: компоненты легко загрузить не в
// те вкладки, а переставлять их руками — копировать два больших текста.
class TabSwapButton : public QObject
{
    Q_OBJECT

public:
    TabSwapButton(QTabBar* bar, const QString& tip);

signals:
    void clicked();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void place();
    void showIfNear(const QPoint& pos);

    QTabBar*     bar;
    QToolButton* button;
};
