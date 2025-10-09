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
    if (s.contains(SPECTRUM_MS)) {
        int idx = ui->refreshSpectrumCMB->findData(s.value(SPECTRUM_MS));
        ui->refreshSpectrumCMB->setCurrentIndex(idx);
    }
    if (s.contains(USE_GRAY_CODE_CHECKED)) {
        ui->useGrayCodeCHB->blockSignals(true); 
        ui->useGrayCodeCHB->setChecked(s.value(USE_GRAY_CODE_CHECKED).toBool());
        ui->stringsSPB->setEnabled(!s.value(USE_GRAY_CODE_CHECKED).toBool());
        ui->useGrayCodeCHB->blockSignals(false);
    }
    if ( s.contains(STRING_VALUE) ) {
        int val = s.value(STRING_VALUE).toInt();
        ui->stringsSPB->setValue( val );
    }
    if (s.contains(USE_GPU_CHECKED)) {
        ui->useGpuCHB->setChecked(s.value(USE_GPU_CHECKED).toBool());
    }
    if (s.contains(TRANSPARENCY_VALUE)) {
        ui->transparencySPB->setValue(s.value(TRANSPARENCY_VALUE).toInt());
    }
    int val = ui->stringsSPB->value();
    int t = 5;


}

SettingsDialog::~SettingsDialog()
{

}

int SettingsDialog::spectrumColorIndex() const
{
    return ui->histoColorCMB->currentIndex();
}

int SettingsDialog::refreshSpectrumIndex() const
{
    return ui->refreshSpectrumCMB->currentIndex();
}

int SettingsDialog::stringValue() const
{
    return ui->stringsSPB->value();
}

void SettingsDialog::setUseGpuCHBChecked(bool b)
{
    ui->useGpuCHB->setChecked(b);
}

void SettingsDialog::setUseGpuCHBEnabled(bool b)
{
    ui->useGpuCHB->setEnabled(b);
}

void SettingsDialog::setUseGrayCodeCHBEnabled(bool b)
{
    ui->useGrayCodeCHB->setEnabled(b);
}

void SettingsDialog::setUseGrayCodeCHBChecked(bool b)
{
    ui->useGrayCodeCHB->setChecked(b);
}


void SettingsDialog::setStringsSPBEnabled(bool b)
{
    ui->stringsSPB->setEnabled( b );
}

void SettingsDialog::setStringsSPBMaxValue(int v)
{
    int val = v;
    if (val == 0 || val > MAX_K)
        val = MAX_K;
    ui->stringsSPB->setMaximum(val);
    ui->stringsSPB->setValue(val);
}

void SettingsDialog::setStringsSPBValue(int v)
{
    ui->stringsSPB->setValue(v);
}

bool SettingsDialog::isUseGpuCHBChecked() const
{
    return ui->useGpuCHB->isChecked();
}

bool SettingsDialog::isUseGrayCodeCHBChecked() const
{
    return ui->useGrayCodeCHB->isChecked();
}

void SettingsDialog::on_buttonBox_accepted()
{
    QSettings s;

    s.setValue( SETTINGS_GEOMETRY, this->saveGeometry() );

    QVariant colVal              =    ui->histoColorCMB->currentData();
    QVariant barVal              =    ui->refreshProgressbarCMB->currentData();
    QVariant sptrVal             =    ui->refreshSpectrumCMB->currentData();
    QVariant strVal              =    ui->stringsSPB->value();
    QVariant useGPUChecked       =    ui->useGpuCHB->isChecked();
    QVariant useGrayCodeChecked  =    ui->useGrayCodeCHB->isChecked();
    QVariant trpVal              =    ui->transparencySPB->value();

    if (colVal.isValid() )            s.setValue( SPECTRUM_COLOR,         colVal.toInt()              );
    if (barVal.isValid() )            s.setValue( PROGRESSBAR_MS,         barVal.toInt()              );
    if (sptrVal.isValid())            s.setValue( SPECTRUM_MS,            sptrVal.toInt()             );
    if (useGrayCodeChecked.isValid()) s.setValue( USE_GRAY_CODE_CHECKED,  useGrayCodeChecked.toBool());
    if (strVal.isValid() )            s.setValue( STRING_VALUE,           strVal.toInt()              );
    if (useGPUChecked.isValid())      s.setValue( USE_GPU_CHECKED,        useGPUChecked.toBool()      );
    if (trpVal.isValid())             s.setValue( TRANSPARENCY_VALUE,     trpVal.toInt()              );
    // Записываем в реестр
    s.sync();
    emit settingsApplied();
}

void SettingsDialog::on_buttonBox_rejected()
{
    QSettings s;

    s.setValue( SETTINGS_GEOMETRY, this->saveGeometry() );


    // Читаем сохранённые значения. Второй аргумент — значение по умолчанию
    int  spectrumColor          = s.value( SPECTRUM_COLOR,        0    ).toInt();
    int  refreshProgressbarMs   = s.value( PROGRESSBAR_MS,        100  ).toInt();
    int  refreshSpectrumMs      = s.value( SPECTRUM_MS,           100  ).toInt();
    int  strVal                 = s.value( STRING_VALUE,          1    ).toInt();
    bool useGPUChecked          = s.value( USE_GPU_CHECKED,       1    ).toBool();
    bool useGrayCodeChecked     = s.value( USE_GRAY_CODE_CHECKED, 0    ).toBool();
    int  trpVal                 = s.value( TRANSPARENCY_VALUE,    50   ).toInt();
    // Устанавливаем их в соответствующие элементы UI
    ui->histoColorCMB->setCurrentIndex(
        ui->histoColorCMB->findData(spectrumColor)
        );
    ui->refreshProgressbarCMB->setCurrentIndex(
        ui->refreshProgressbarCMB->findData(refreshProgressbarMs)
        );
    ui->stringsSPB->setValue(
        strVal
        );
    ui->useGpuCHB->setChecked(
        useGPUChecked
        );
    ui->useGrayCodeCHB->setChecked(
        useGrayCodeChecked
        );
    ui->transparencySPB->setValue(
        trpVal
        );
    ui->refreshSpectrumCMB->setCurrentIndex(
        ui->refreshSpectrumCMB->findData(refreshSpectrumMs)
        );
}

void SettingsDialog::on_useGrayCodeCHB_toggled(bool checked)
{
    QSettings s;
    if (checked) {
        s.setValue(STRING_VALUE, ui->stringsSPB->value());
        ui->stringsSPB->setValue(ui->stringsSPB->maximum());
        ui->stringsSPB->setEnabled(false);
        return;
    }
    int val = s.value(STRING_VALUE).toInt();
    ui->stringsSPB->setValue(val);
    ui->stringsSPB->setEnabled(true);
    
}

void SettingsDialog::setData()
{

    ui->refreshProgressbarCMB->setItemData( 0, 100   );
    ui->refreshProgressbarCMB->setItemData( 1, 500   );
    ui->refreshProgressbarCMB->setItemData( 2, 1000  );
    ui->refreshProgressbarCMB->setItemData( 3, 5000  );
    ui->refreshProgressbarCMB->setItemData( 4, 10000 );
    ui->refreshProgressbarCMB->setItemData( 5, 60000 );

    ui->refreshSpectrumCMB->setItemData( 0, 100     );
    ui->refreshSpectrumCMB->setItemData( 1, 500     );
    ui->refreshSpectrumCMB->setItemData( 2, 1000    );
    ui->refreshSpectrumCMB->setItemData( 3, 5000    );
    ui->refreshSpectrumCMB->setItemData( 4, 10000   );
    ui->refreshSpectrumCMB->setItemData( 5, 60000   );
    ui->refreshSpectrumCMB->setItemData( 6, 600000  );
    ui->refreshSpectrumCMB->setItemData( 7, 3600000 );

    ui->histoColorCMB->setItemData( 0, 0 );
    ui->histoColorCMB->setItemData( 1, 1 );
    ui->histoColorCMB->setItemData( 2, 2 );
}

