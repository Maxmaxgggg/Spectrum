#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include "defines.h"
#include "settings.h"
#include "workwithmatrix.h"
#include <QJsonObject>
#include <qjsondocument.h>
#include <qbuttongroup.h>
#include <cuda_runtime.h>

namespace Ui { class SettingsDialog; }
enum class Length { Short, Long };

// Вкладка «Расчёт» устроена как три вопроса подряд: какой код, сколько
// перебирать, каким алгоритмом. Ответы на первые два сужают третий:
//
//   Полный перебор   → код Грея | дуальный код   (весь спектр, до 63 строк)
//   Частичный        → Брауэр–Циммерман | Леон   (низ спектра до веса)
//   Код-произведение → всегда частичный по рангам; алгоритм выбирает, чем
//                      считать большие компоненты
//
// Простой XOR из интерфейса убран: всё, что он умел, Брауэр–Циммерман делает
// быстрее и с гарантией. Сам путь в расчёте остался — старые записи с ним
// открываются.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:

    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

public: signals:
    // Сигнал для отправки настроек виджету
    void sendSettingsToWidget( const QJsonObject& settings );
    // Просьба прогнать пробу: короткий расчёт, по которому видно, как часто
    // спектр успевает обновляться на этих настройках.
    void measureUpdateRateRequested( const QJsonObject& settings );
public slots:
    void handleMatrixChanged(int rows, int cols);
    void handleSettingsRequested();
    void setInterfaceEnabled(bool enabled);
    // Ставит настройки расчёта из поднятого автосохранения: иначе кнопка
    // «Продолжить» искала бы запись другого алгоритма и не нашла бы её.
    // weight — вес записи (у произведения — его вес, rank — ранг);
    // componentAlgorithm — чем считались компоненты произведения.
    void applyFromAutosave(int algorithm, int enumType, int rank, int weight,
                           int componentAlgorithm);
    // Результат пробы, отправок в секунду.
    void applyMeasuredRate(double perSecond);
private slots:

private:
    void checkGpuAvailable();
    // Открывает список интервалов по замеру и гасит недостижимые пункты.
    // Пока замера для текущей конфигурации нет, список заблокирован на
    // секунде: она достижима на любой конфигурации.
    void applyUpdateRateLimit();
    // Описание конфигурации, от которой зависит потолок обновления: код,
    // вычислитель, алгоритм, сетка. Сменилось — замер больше не годится.
    QString updateRateKey() const;
    // Переносит настройки из полей диалога в settings, ничего не сохраняя.
    void collectSettings();
    void applyDeviceLimits();
    // Показывает поля вычислителя, подходящие текущему устройству.
    void updateDeviceControls();
    // Приводит вкладку «Расчёт» в согласованный вид: какие переключатели
    // видны и доступны, что показывает поле веса, что написано в подсказках.
    void updateComputationControls();
    // Алгоритм, который следует из положения переключателей.
    ComputationSettings::Algorithm currentAlgorithm() const;
    // Стохастический поиск по произвольному коду: потолок веса — по памяти
    // под таблицу слов, рядом с полем — ожидаемый размер таблицы.
    void applyMemoryCap();
    // Вес для текущего алгоритма — они хранятся отдельно, поле одно.
    int& weightFor(ComputationSettings::Algorithm algorithm);
    bool isGpuAvailable();
    void loadSettings();
    void saveSettings();


    // До первой матрицы считаем код коротким: полный перебор доступен.
    Length codeLength     = Length::Short;
    Length dualCodeLength = Length::Short;
    Ui::SettingsDialog *ui;
    QButtonGroup* codeKindBGP;
    QButtonGroup* enumeratorBGP;
    QButtonGroup* algorithmBGP;
    QButtonGroup* computeDeviceBGP;

    // Последний выбор в каждой из двух пар алгоритмов: при переключении
    // «полный/частичный» показывается та пара, что подходит, и в ней —
    // то, что пользователь выбирал раньше.
    ComputationSettings::Algorithm fullAlgorithm    = ComputationSettings::GrayCode;
    ComputationSettings::Algorithm partialAlgorithm = ComputationSettings::BrouwerZimmermann;

    // Тип перебора, выбранный для произвольного кода: у произведения группа
    // блокируется на «частичном», и выбор надо помнить отдельно.
    ComputationSettings::EnumerationType singleEnumType =
        ComputationSettings::EnumerationType::Full;

    // Замеренный потолок обновления и конфигурация, на которой он получен.
    // Ноль — замера нет.
    double  measuredRate = 0.0;
    QString measuredFor;

    // Размер кода: от него зависит потолок, а самой матрицы диалог не видит.
    int matrixRows = 0;
    int matrixCols = 0;

    // Поле веса перезаписывается программно при смене алгоритма; в это время
    // его сигнал не должен трогать запомненные веса.
    bool updatingControls = false;

    ComputationSettings settings;
};

#endif // SETTINGSDIALOG_H
