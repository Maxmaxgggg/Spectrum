#pragma once

#include <QObject>
#include <QPointer>

class QDockWidget;
class QMainWindow;
class QTabBar;
class QToolButton;

// Кнопка «поменять местами» между двумя доками, лежащими вкладками рядом.
//
// Полоса вкладок у сложенных доков принадлежит QMainWindow, и своих кнопок
// на ней не бывает. Поэтому кнопка — накладка поверх этой полосы: она
// встаёт на стык двух вкладок и показывается, только когда курсор рядом со
// стыком. Полоса пересоздаётся при каждой перестановке доков, так что
// накладка после каждой перестановки ищет её заново.
//
// Нужна коду произведения: компоненты можно загрузить не в те панели, а
// переставлять их руками — копировать два больших текста туда-сюда.
class DockSwapButton : public QObject
{
    Q_OBJECT

public:
    DockSwapButton(QMainWindow* window, QDockWidget* first, QDockWidget* second,
                   const QString& tip);

    // Найти полосу вкладок с обоими доками и повесить на неё кнопку.
    // Безопасно звать сколько угодно раз.
    void attach();

signals:
    void clicked();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QTabBar* findTabBar() const;
    void     place();
    void     showIfNear(const QPoint& pos);

    QMainWindow*  window;
    QDockWidget*  first;
    QDockWidget*  second;
    QString       tip;

    QPointer<QTabBar>     bar;
    QPointer<QToolButton> button;
};
