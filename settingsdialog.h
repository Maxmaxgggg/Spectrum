#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include "defines.h"
#include <QJsonObject>


namespace Ui { class SettingsDialog; }

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:

    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

    

    int   getHistoColorValue()			const;
    int   getRefreshProgressbarValue()	const;
    int   getRefreshSpectrumValue()		const;
    int   getStringsValue()				const;
    int   getStringsMaxValue()          const;
    bool  isGpuUsed()					const;
    bool  isGrayCodeUsed()				const;
    bool  isDualCodeUsed()              const;
    int   getTransparencyValue()		const;

    void  setHistoColorValue(int);
    void  setRefreshProgressbarValue(int);
    void  setRefreshSpectrumValue(int);
    void  setStringsValue(int);
    void  setStringsMaxValue(int);
    void  setUseGpu(bool);
    void  setUseGrayCode(bool);
    void  setUseDualCode(bool);
    void  setTransparencyValue(int);

public: signals:
    void useGpuToggled(bool);
    void useGrayCodeToggled(bool);
    void useDualCodeToggled(bool);
    void refreshProgressbarValueChanged(int);
    void refreshSpectrumValueChanged(int);
    void sendInitialSettingsToWorker(const QJsonObject& settings);

public slots:

    void disableGpu();

    void toggleStringsSPB(bool);

    void toggleUseGpuCHB(bool);

    void toggleUseGrayCodeCHB(bool);

    void toggleUseDualCodeCHB(bool);

    void handleMatrixChanged(int);

    void handleRequestInitialSettings();
private slots:

    void on_buttonBox_accepted();

    void on_buttonBox_rejected();

    void on_useGrayCodeCHB_toggled(bool checked);

    void on_useDualCodeCHB_toggled(bool checked);

private:

    void saveSettings();

    void loadSettings();

    void updateUiFromSettings();
    void updateSettingsFromUi();
    Ui::SettingsDialog *ui;
    QJsonObject settings;

    void setData();
    void setToolTips();
};

#endif // SETTINGSDIALOG_H
