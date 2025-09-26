#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QColor>

namespace Ui { class SettingsDialog; }

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

    int      spectrumColorIndex()  const;
    int      refreshRequestIndex() const;
    int      refreshPlotIndex()    const;
    int      refreshPteIndex()     const;
    int      stringData()          const;
    bool     isDThreadCHBChecked() const;

    void     setDThreadCHBEnabled(bool);
    void     setStringsCMBEnabled(bool);
    
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
