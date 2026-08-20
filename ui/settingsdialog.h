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
private slots:

private:
    void checkGpuAvailable();
    void applyDeviceLimits();
    // Показывает поля вычислителя, подходящие текущему устройству.
    void updateDeviceControls();
    bool isGpuAvailable();
    void loadSettings();
    void saveSettings();


    Length codeLength;
    Length dualCodeLength;
    Ui::SettingsDialog *ui;
    QButtonGroup* algorithmBGP;
    QButtonGroup* enumeratorBGP;
    QButtonGroup* computeDeviceBGP;
    ComputationSettings settings;
};

#endif // SETTINGSDIALOG_H
