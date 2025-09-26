#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include <QSettings>
#include <QColorDialog>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);
    this->setData();

    QSettings s;
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    this->restoreGeometry( s.value("settingsGeometry").toByteArray() );
    if ( s.contains("spectrumColor") ){
        int idx = ui->histoColorCMB->findData(s.value("spectrumColor"));
        ui->histoColorCMB->setCurrentIndex( idx );
    }

    if ( s.contains("refreshProgressbarMs") ) {
        int idx = ui->refreshProgressbarCMB->findData(s.value("refreshProgressbarMs"));
        ui->refreshProgressbarCMB->setCurrentIndex( idx );
    }
    if ( s.contains("refreshPlotMs") ) {
        int idx = ui->refreshHistoCMB->findData(s.value("refreshPlotMs"));
        ui->refreshHistoCMB->setCurrentIndex( idx );
    }
    if ( s.contains("refreshPteMs") ) {
        int idx = ui->refreshTextCMB->findData(s.value("refreshPteMs"));
        ui->refreshTextCMB->setCurrentIndex( idx );
    }
    if ( s.contains("stringValue") ) {
        int idx = ui->stringsCMB->findData(s.value("stringValue"));
        ui->stringsCMB->setCurrentIndex( idx );
    }
    if (s.contains("distinctThreadChecked")) {
        ui->distinctThreadCHB->setChecked(s.value("distinctThreadChecked").toBool());
    }

}

SettingsDialog::~SettingsDialog()
{

}

int SettingsDialog::spectrumColorIndex() const
{
    return ui->histoColorCMB->currentIndex();
}

int SettingsDialog::refreshRequestIndex() const
{
    return ui->refreshProgressbarCMB->currentIndex();
}

int SettingsDialog::refreshPlotIndex() const
{
    return ui->refreshHistoCMB->currentIndex();
}

int SettingsDialog::refreshPteIndex() const
{
    return ui->refreshTextCMB->currentIndex();
}

int SettingsDialog::stringData() const
{
    return ui->stringsCMB->currentData().toInt();
}

void SettingsDialog::setDThreadCHBEnabled(bool b)
{
    ui->distinctThreadCHB->setEnabled(b);
}

void SettingsDialog::setStringsCMBEnabled(bool b)
{
    ui->stringsCMB->setEnabled( b );
}

bool SettingsDialog::isDThreadCHBChecked() const
{
    return ui->distinctThreadCHB->isChecked();
}

void SettingsDialog::on_buttonBox_accepted()
{
    QSettings s;

    s.setValue( "settingsGeometry", this->saveGeometry() );

    QVariant colVal      =  ui->histoColorCMB->currentData();
    QVariant barVal      =  ui->refreshProgressbarCMB->currentData();
    QVariant plotVal     =  ui->refreshHistoCMB->currentData();
    QVariant pteVal      =  ui->refreshTextCMB->currentData();
    QVariant strVal      =  ui->stringsCMB->currentData();
    QVariant dThrChecked = ui->distinctThreadCHB->isChecked();
    if (colVal.isValid() )     s.setValue( "spectrumColor",         colVal.toInt()       );
    if (barVal.isValid() )     s.setValue( "refreshProgressbarMs",  barVal.toInt()       );
    if (plotVal.isValid())     s.setValue( "refreshPlotMs",         plotVal.toInt()      );
    if (pteVal.isValid() )     s.setValue( "refreshPteMs",          pteVal.toInt()       );
    if (strVal.isValid() )     s.setValue( "stringValue",           strVal.toInt()       );
    if (dThrChecked.isValid()) s.setValue( "distinctThreadChecked", dThrChecked.toBool() );
    // Записываем в реестр
    s.sync();
    emit settingsApplied();
}

void SettingsDialog::on_buttonBox_rejected()
{
    QSettings s;

    s.setValue( "settingsGeometry", this->saveGeometry() );


    // Читаем сохранённые значения. Второй аргумент — значение по умолчанию
    int  spectrumColor          = s.value( "spectrumColor",         0    ).toInt();
    int  refreshProgressbarMs   = s.value( "refreshProgressbarMs",  1000 ).toInt();
    int  refreshPlotMs          = s.value( "refreshPlotMs",         1000 ).toInt();
    int  refreshPteMs           = s.value( "refreshPteMs",          1000 ).toInt();
    int  strVal                 = s.value( "stringValue",           -1   ).toInt();
    bool dThrChecked            = s.value( "distinctThreadChecked", 1    ).toBool();
    // Устанавливаем их в соответствующие элементы UI
    ui->histoColorCMB->setCurrentIndex(
        ui->histoColorCMB->findData(spectrumColor)
        );
    ui->refreshProgressbarCMB->setCurrentIndex(
        ui->refreshProgressbarCMB->findData(refreshProgressbarMs)
        );
    ui->refreshHistoCMB->setCurrentIndex(
        ui->refreshHistoCMB->findData(refreshPlotMs)
        );
    ui->refreshTextCMB->setCurrentIndex(
        ui->refreshTextCMB->findData(refreshPteMs)
        );
    ui->stringsCMB->setCurrentIndex(
        ui->stringsCMB->findData(strVal)
        );
    ui->distinctThreadCHB->setChecked(
        dThrChecked
    );
}

void SettingsDialog::setData()
{
    ui->refreshTextCMB->setItemData(0, 10);
    ui->refreshTextCMB->setItemData(1, 100);
    ui->refreshTextCMB->setItemData(2, 1000);
    ui->refreshTextCMB->setItemData(3, 10000);
    ui->refreshTextCMB->setItemData(4, 60000);
    ui->refreshTextCMB->setItemData(5, 600000);
    ui->refreshTextCMB->setItemData(6, 3600000);

    ui->refreshProgressbarCMB->setItemData(0, 1000);
    ui->refreshProgressbarCMB->setItemData(1, 10000);
    ui->refreshProgressbarCMB->setItemData(2, 60000);

    ui->refreshHistoCMB->setItemData(0, 10);
    ui->refreshHistoCMB->setItemData(1, 100);
    ui->refreshHistoCMB->setItemData(2, 1000);
    ui->refreshHistoCMB->setItemData(3, 10000);
    ui->refreshHistoCMB->setItemData(4, 60000);
    ui->refreshHistoCMB->setItemData(5, 600000);
    ui->refreshHistoCMB->setItemData(6, 3600000);

    ui->histoColorCMB->setItemData(0, 0);
    ui->histoColorCMB->setItemData(1, 1);
    ui->histoColorCMB->setItemData(2, 2);

    ui->stringsCMB->setItemData(0,2);
    ui->stringsCMB->setItemData(1,3);
    ui->stringsCMB->setItemData(2,4);
    ui->stringsCMB->setItemData(3,5);
    ui->stringsCMB->setItemData(4,-1);
}

