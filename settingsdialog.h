#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QColor>
#include "defines.h"
#include "json.hpp"


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
    int   getTransparencyValue()		const;

    void  setHistoColorValue(int);
    void  setRefreshProgressbarValue(int);
    void  setRefreshSpectrumValue(int);
    void  setStringsValue(int);
    void  setStringsMaxValue(int);
    void  setUseGpu(bool);
    void  setUseGrayCode(bool);
    void  setTransparencyValue(int);

public: signals:
    void useGpuToggled(bool);
    void useGrayCodeToggled(bool);
    void refreshProgressbarValueChanged(int);
    void refreshSpectrumValueChanged(int);
    void sendInitialSettingsToWorker(bool, bool, int, int);
    

public slots:

    void disableGpu();

    void toggleStringsSPB(bool);

    void toggleUseGpuCHB(bool);

    void toggleUseGrayCodeCHB(bool);

    void handleMatrixChanged(int);

    void handleRequestInitialSettings();
private slots:

    void on_buttonBox_accepted();

    void on_buttonBox_rejected();

    void on_useGrayCodeCHB_toggled(bool checked);

    

private:

    void saveSettings();

    void loadSettings();

    void updateUiFromSettings();
    void updateSettingsFromUi();
    Ui::SettingsDialog *ui;
    nlohmann::json settings;
    void setData();
};

#endif // SETTINGSDIALOG_H
