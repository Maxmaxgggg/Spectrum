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

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:

    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

public: signals:
    // Сигнал для отправки настроек виджету
    void sendSettingsToWidget( const QJsonObject& settings );
public slots:
    void handleMatrixChanged(int rows, int cols);
    void handleSettingsRequested();
    void setInterfaceEnabled(bool enabled);
    // Ставит настройки расчёта из поднятого автосохранения: иначе кнопка
    // «Продолжить» искала бы запись другого алгоритма и не нашла бы её.
    void applyFromAutosave(int algorithm, int enumType, int maxRows);
private slots:

private:
    void checkGpuAvailable();
    void applyDeviceLimits();
    // Показывает поля вычислителя, подходящие текущему устройству.
    void updateDeviceControls();
    // Приводит группу «Тип перебора» в соответствие выбранному алгоритму.
    void updateEnumTypeControls();
    bool isGpuAvailable();
    void loadSettings();
    void saveSettings();


    Length codeLength;
    Length dualCodeLength;
    Ui::SettingsDialog *ui;
    QButtonGroup* algorithmBGP;
    QButtonGroup* enumeratorBGP;
    QButtonGroup* computeDeviceBGP;

    // Тип перебора, выбранный для простого XOR. Код Грея и дуальный расчёт
    // перебирают все 2^k масок и о частичном переборе не знают, поэтому на
    // них группа блокируется и показывает «Полный». Выбор пользователя при
    // этом не теряется: он лежит здесь и возвращается при переходе обратно.
    ComputationSettings::EnumerationType xorEnumType =
        ComputationSettings::EnumerationType::Full;

    // Число строк, вписанное пользователем для частичного перебора. Хранится
    // отдельно от поля ввода: при полном переборе там показывается число
    // строк матрицы, и вписанное значение иначе затиралось бы при каждом
    // переключении туда-обратно.
    int xorMaxRows = 0;

    ComputationSettings settings;
};

#endif // SETTINGSDIALOG_H
