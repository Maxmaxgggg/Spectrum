#pragma once

#include "constants.h"
#include "types.h"

#include <QColor>
#include <QSharedPointer>
#include <QVector>

class QCustomPlot;
class QCPBars;
class QCPItemText;
class QCPAxisTicker;

// Столбчатый график спектра.
//
// Сам виджет QCustomPlot лежит в .ui и принадлежит окну; этот класс его
// настраивает и рисует. Кэши координат и тикера — не преждевременная
// оптимизация: спектр перерисовывается на каждом изменении размера окна,
// а длина его равна длине кода, то есть доходит до тысяч столбцов.
class SpectrumPlot
{
public:
    explicit SpectrumPlot(QCustomPlot* plot);

    // Показать новый спектр. Пустой игнорируется.
    void setSpectrum(const SpectrumFloat& spectrum);

    // Перерисовать текущий спектр: шаг подписей по оси X зависит от ширины
    // виджета, поэтому после изменения размера график надо собрать заново.
    // Она же догоняет данные, накопившиеся, пока панель была закрыта.
    void refresh();

    void setBarColor(const QColor& color);

    // Потолок числа столбцов. Спектр длинного кода — это тысяча весов, и
    // столбец выходит тоньше пикселя: соседние веса сливаются в один столбец,
    // а их значения складываются. Форма распределения при этом сохраняется,
    // сумма по картинке — тоже. Ноль снимает ограничение.
    void setMaxBars(int maxBars);

    // Показанные значения: окно сохраняет их между запусками.
    const QVector<double>& values() const { return yValues; }

private:
    // Перестраивает подписи оси X, если сменился шаг между ними.
    void updateTicker();
    // Заливает точки в столбцы. Дорого: QCustomPlot копирует и сортирует весь
    // контейнер, поэтому зовётся только когда спектр действительно поменялся.
    void applyData();
    // Подписи, диапазоны осей и перерисовка. Данные не трогает.
    void redrawGeometry();

    QCustomPlot* plot            = nullptr;
    QCPBars*     bars            = nullptr;
    QCPItemText* overflowMessage = nullptr;

    // Спектр как он есть: вес i, значение yValues[i].
    QVector<double> xValues;
    QVector<double> yValues;

    // Что реально нарисовано. При группировке короче исходного: одна точка на
    // корзину весов, значение — сумма по корзине.
    QVector<double> barX;
    QVector<double> barY;
    double          barWidth = 1.0;
    double          barMax   = 0.0;

    int maxBars = 0;

    // Спектр пришёл, пока панель графика была закрыта: точки в столбцы не
    // залиты, ждём показа. Заливать впустую незачем — на длинном расчёте
    // спектр обновляется раз в секунду часами.
    bool pendingData = false;

    QSharedPointer<QCPAxisTicker> ticker;
    int tickStep = -1;

    // Разбор последнего спектра. Хранится, чтобы refresh() не пересчитывал
    // его заново по тем же самым числам.
    qint64 firstNonZero = -1;
    qint64 lastNonZero  = -1;
    double maxValue     = 0.0;   // максимум по исходному спектру
    // Компоненты не поместились в float: рисовать нечего, показываем текст.
    bool   hasNonFinite = false;

    QColor barColor;
};
