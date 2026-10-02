#pragma once

#include <QWidget>

class QDockWidget;
class QStyleOptionDockWidget;
class QTabBar;
class QToolButton;

// Полоса заголовка панели.
//
// Зачем своя. Пока панель пришвартована, заголовок рисует Qt: серая полоса,
// название слева, две маленькие кнопки справа. Стоит вытащить панель в
// отдельное окно — она становится обычным окном, и на Windows Qt отдаёт ей
// оформление системы: светлая полоса и красный крестик. Вид скачет, хотя это
// одна и та же панель.
//
// Отключается это ровно одним способом. Qt берёт системную рамку только когда
// у дока нет своего заголовка — за это отвечает роль TitleBar в его раскладке
// (см. QDockWidgetLayout::Role и nativeWindowDeco в qdockwidget_p.h). Как
// только заголовок задан, рамка не запрашивается, и обе картинки рисуем мы.
//
// Размер плавающего окна при этом менять по-прежнему можно: без системной
// рамки Qt тянет его сам, своим QWidgetResizeHandler.
//
// Рисуется всё штатным стилем, элементом CE_DockWidgetTitle, а не руками:
// вид пришвартованной панели меняться не должен, а он и так уже правильный.
class DockTitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit DockTitleBar(QDockWidget* dock);

    // Полоса вкладок вместо названия: панель с несколькими страницами держит
    // вкладки прямо в заголовке, в одной строке с кнопками. Пока полоса
    // спрятана, рисуется название, как обычно.
    void setTabBar(QTabBar* bar);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void initStyleOption(QStyleOptionDockWidget* option) const;
    // Кнопки следуют возможностям дока: непереносимой панели незачем кнопка
    // «в отдельное окно», незакрываемой — крестик.
    void updateButtons();
    int  buttonExtent() const;
    // Док сложен вкладкой с другими: название уже написано на вкладке, и в
    // заголовке его не дублируем — остаются одни кнопки.
    bool tabified() const;

    // Название не рисуется, когда его заменяет полоса вкладок.
    bool showsTabs() const;

    QDockWidget* m_dock  = nullptr;
    QToolButton* m_floatButton = nullptr;
    QToolButton* m_closeButton = nullptr;
    QTabBar*     m_tabBar      = nullptr;
};
