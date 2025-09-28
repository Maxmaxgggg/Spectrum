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
    this->restoreGeometry( s.value(SETTINGS_GEOMETRY).toByteArray() );
    if ( s.contains(SPECTRUM_COLOR) ){
        int idx = ui->histoColorCMB->findData(s.value(SPECTRUM_COLOR));
        ui->histoColorCMB->setCurrentIndex( idx );
    }

    if ( s.contains(PROGRESSBAR_MS) ) {
        int idx = ui->refreshProgressbarCMB->findData(s.value(PROGRESSBAR_MS));
        ui->refreshProgressbarCMB->setCurrentIndex( idx );
    }
    if ( s.contains(PLOT_MS) ) {
        int idx = ui->refreshHistoCMB->findData(s.value(PLOT_MS));
        ui->refreshHistoCMB->setCurrentIndex( idx );
    }
    if ( s.contains(PTE_MS) ) {
        int idx = ui->refreshTextCMB->findData(s.value(PTE_MS));
        ui->refreshTextCMB->setCurrentIndex( idx );
    }
    if ( s.contains(STRING_VALUE) ) {
        int idx = ui->stringsCMB->findData(s.value(STRING_VALUE));
        ui->stringsCMB->setCurrentIndex( idx );
    }
    if (s.contains(D_THREAD_CHECKED)) {
        ui->distinctThreadCHB->setChecked(s.value(D_THREAD_CHECKED).toBool());
    }
    if (s.contains(TRANSPARENCY_VALUE)) {
        ui->transparencySPB->setValue(s.value(TRANSPARENCY_VALUE).toInt());
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

    s.setValue( SETTINGS_GEOMETRY, this->saveGeometry() );

    QVariant colVal            =    ui->histoColorCMB->currentData();
    QVariant barVal            =    ui->refreshProgressbarCMB->currentData();
    QVariant plotVal           =    ui->refreshHistoCMB->currentData();
    QVariant pteVal            =    ui->refreshTextCMB->currentData();
    QVariant strVal            =    ui->stringsCMB->currentData();
    QVariant dThrChecked       =    ui->distinctThreadCHB->isChecked();
    QVariant trpVal            =    ui->transparencySPB->value();

    if (colVal.isValid() )          s.setValue( SPECTRUM_COLOR,         colVal.toInt()       );
    if (barVal.isValid() )          s.setValue( PROGRESSBAR_MS,         barVal.toInt()       );
    if (plotVal.isValid())          s.setValue( PLOT_MS,                plotVal.toInt()      );
    if (pteVal.isValid() )          s.setValue( PTE_MS,                 pteVal.toInt()       );
    if (strVal.isValid() )          s.setValue( STRING_VALUE,           strVal.toInt()       );
    if (dThrChecked.isValid())      s.setValue( D_THREAD_CHECKED,       dThrChecked.toBool() );
    if (trpVal.isValid())           s.setValue( TRANSPARENCY_VALUE,     trpVal.toInt()       );
    // Записываем в реестр
    s.sync();
    emit settingsApplied();
}

void SettingsDialog::on_buttonBox_rejected()
{
    QSettings s;

    s.setValue( SETTINGS_GEOMETRY, this->saveGeometry() );


    // Читаем сохранённые значения. Второй аргумент — значение по умолчанию
    int  spectrumColor          = s.value( SPECTRUM_COLOR,     0    ).toInt();
    int  refreshProgressbarMs   = s.value( PROGRESSBAR_MS,     1000 ).toInt();
    int  refreshPlotMs          = s.value( PLOT_MS,            1000 ).toInt();
    int  refreshPteMs           = s.value( PTE_MS,             1000 ).toInt();
    int  strVal                 = s.value( STRING_VALUE,       -1   ).toInt();
    bool dThrChecked            = s.value( D_THREAD_CHECKED,   1    ).toBool();
    int  trpVal                 = s.value( TRANSPARENCY_VALUE, 50   ).toInt();
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
    ui->transparencySPB->setValue(
        trpVal
        );
}

void SettingsDialog::setData()
{
    ui->refreshTextCMB->setItemData( 0, 10      );
    ui->refreshTextCMB->setItemData( 1, 100     );
    ui->refreshTextCMB->setItemData( 2, 1000    );
    ui->refreshTextCMB->setItemData( 3, 10000   );
    ui->refreshTextCMB->setItemData( 4, 60000   );
    ui->refreshTextCMB->setItemData( 5, 600000  );
    ui->refreshTextCMB->setItemData( 6, 3600000 );

    ui->refreshProgressbarCMB->setItemData( 0, 1000  );
    ui->refreshProgressbarCMB->setItemData( 1, 10000 );
    ui->refreshProgressbarCMB->setItemData( 2, 60000 );

    ui->refreshHistoCMB->setItemData( 0, 10      );
    ui->refreshHistoCMB->setItemData( 1, 100     );
    ui->refreshHistoCMB->setItemData( 2, 1000    );
    ui->refreshHistoCMB->setItemData( 3, 10000   );
    ui->refreshHistoCMB->setItemData( 4, 60000   );
    ui->refreshHistoCMB->setItemData( 5, 600000  );
    ui->refreshHistoCMB->setItemData( 6, 3600000 );

    ui->histoColorCMB->setItemData( 0, 0 );
    ui->histoColorCMB->setItemData( 1, 1 );
    ui->histoColorCMB->setItemData( 2, 2 );

    ui->stringsCMB->setItemData( 0,  2 );
    ui->stringsCMB->setItemData( 1,  3 );
    ui->stringsCMB->setItemData( 2,  4 );
    ui->stringsCMB->setItemData( 3,  5 );
    ui->stringsCMB->setItemData( 4, -1 );
}

