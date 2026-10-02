#pragma once

class QWidget;

// Индикатор прогресса на кнопке приложения в панели задач Windows.
//
// Вынесено из MainWindow прежде всего ради заголовка: там ради трёх полей
// подключались <windows.h> и <shobjidl.h>, и всё, что включало mainwindow.h,
// получало заодно макросы min/max и полтысячи чужих имён.
//
// Вне Windows все методы — пустышки, чтобы вызывающему коду не приходилось
// оборачивать их в #ifdef.
class TaskbarProgress
{
public:
    // Индикатор привязывается к нативному окну виджета, поэтому создавать
    // объект нужно после того, как окно уже собрано.
    explicit TaskbarProgress(QWidget* window);
    ~TaskbarProgress();

    TaskbarProgress(const TaskbarProgress&)            = delete;
    TaskbarProgress& operator=(const TaskbarProgress&) = delete;

    // Панель задач недоступна, например, при запуске из-под RDP-сессии
    // без proper shell. Тогда объект просто ничего не делает.
    bool isAvailable() const;

    // 0 и 100 гасят индикатор: пустая и полная полоса на кнопке одинаково
    // бессмысленны, а «горит на 100%» ещё и выглядит как зависший расчёт.
    void setPercent(int percent);
    void clear();

private:
    QWidget* m_window = nullptr;

    // Не ITaskbarList3*, чтобы не тащить сюда shobjidl.h.
    void* m_taskbar = nullptr;
};
