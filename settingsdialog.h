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

    int      spectrumColorIndex()      const;
    int      refreshSpectrumIndex()    const;
    int      stringValue()             const;
    bool     isUseGpuCHBChecked()      const;
    bool     isUseGrayCodeCHBChecked() const;

    void     setUseGpuCHBChecked( bool);
    void     setUseGpuCHBEnabled( bool);
    void     setUseGrayCodeCHBEnabled(bool);
    void     setUseGrayCodeCHBChecked(bool);
    void     setStringsSPBEnabled(bool);
    void     setStringsSPBMaxValue(int);
    void     setStringsSPBValue(int);
public: signals:
    void     settingsApplied();
private slots:

    void on_buttonBox_accepted();

    void on_buttonBox_rejected();

    void on_useGrayCodeCHB_toggled(bool checked);
private:
    Ui::SettingsDialog *ui;
    void setData();
};

#endif // SETTINGSDIALOG_H
