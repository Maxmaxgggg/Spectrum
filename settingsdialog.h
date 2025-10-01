#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QColor>
#include "defines.h"
namespace Ui { class SettingsDialog; }

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

    int      spectrumColorIndex()   const;
    int      refreshSpectrumIndex() const;
    int      stringValue()          const;
    bool     isUseGpuCHBChecked()   const;

    void     setUseGpuCHBChecked( bool);
    void     setUseGpuCHBEnabled( bool);
    void     setStringsSPBEnabled(bool);
    void     setStringsSPBMaxValue(int);
    
public: signals:
    void     settingsApplied();
private slots:

    void on_buttonBox_accepted();

    void on_buttonBox_rejected();

private:
    Ui::SettingsDialog *ui;
    void setData();
};

#endif // SETTINGSDIALOG_H
