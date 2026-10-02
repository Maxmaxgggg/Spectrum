#pragma once

#include "settings.h"

#include <QWidget>

class QLabel;

// Панель хода расчёта.
//
// Раньше всё это лежало одной строкой под прогрессбаром. Строка на всю ширину
// окна — это данные, разложенные по единственному измерению, которого в этом
// окне и так с избытком: подписи приходилось резать, разделять точками, а
// глазу не за что зацепиться. В столбике подписи выравниваются, значения
// встают колонкой, и читается это сразу.
//
// Панель — такой же док, как матрица и спектр: её можно вытащить в отдельное
// окно, закрыть и вернуть через меню «Вид». Если её закрыть, ход расчёта всё
// равно виден — процент на полосе внизу и «осталось» в заголовке окна.
class StatsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit StatsPanel(QWidget* parent = nullptr);

    // Чем и что считаем. Вызывается на старте расчёта.
    void showTask(const ComputationSettings& settings);
    // Сетка, выбранная подбором: до расчёта она неизвестна.
    void showGrid(int blocks, int threads);
    void showProgress(int elapsedSec, int minutesLeft, double speed,
                      quint64 doneOps, quint64 totalOps);
    // Расчёта нет: прочерки вместо цифр прошлого запуска.
    void clearProgress();
    // Короткое сообщение о происходящем: «Готово», «Отменено пользователем».
    void showState(const QString& text);

private:
    QLabel* addRow(class QFormLayout* form, const QString& caption);

    QLabel* m_stateValue     = nullptr;
    QLabel* m_deviceValue    = nullptr;
    QLabel* m_algorithmValue = nullptr;

    QLabel* m_elapsedValue   = nullptr;
    QLabel* m_remainingValue = nullptr;
    QLabel* m_finishValue    = nullptr;

    QLabel* m_speedValue     = nullptr;
    QLabel* m_doneValue      = nullptr;

    // Сетка приходит отдельным сигналом, позже описания задачи, и её надо
    // помнить, чтобы дописать к строке устройства.
    QString m_grid;
    QString m_deviceText;
};
