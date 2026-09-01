#pragma once

#include <QIcon>
#include <QString>

class QWidget;

// Значки из шрифта Segoe Fluent Icons.
//
// Приём взят из «Контекста»: глиф рисуется цветом текста текущей палитры, то
// есть значок остаётся видимым и в светлой теме, и в тёмной, и Qt сам делает
// из него приглушённый вариант для выключенной кнопки. Растровые значки так
// не умеют — их пришлось бы держать в двух наборах.
namespace FluentIcons {

// Коды глифов. Те же, что в «Контексте», чтобы кнопки в двух программах
// выглядели одинаково.
constexpr const char PLAY[]  = "E768";   // треугольник: старт и продолжение
constexpr const char PAUSE[] = "E769";   // две полосы: пауза
constexpr const char STOP[]  = "E71A";   // квадрат: отмена

// widget нужен только ради палитры — цвет берётся из неё.
QIcon icon(const QWidget* widget, const char* code, int size = 18);

} // namespace FluentIcons
