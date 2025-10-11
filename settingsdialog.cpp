#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include <QSettings>
#include <QColorDialog>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);

    setData();
    loadSettings();
    updateUiFromSettings();
    
    // Отключаем кнопку помощи
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    
}

SettingsDialog::~SettingsDialog()
{
    saveSettings();
}

void SettingsDialog::handleRequestInitialSettings() {
    emit sendInitialSettingsToWorker(settings[USE_GPU_CHECKED], settings[USE_GRAY_CODE_CHECKED], settings[SPECTRUM_MS], settings[PROGRESSBAR_MS] );
}
void SettingsDialog::on_buttonBox_accepted()
{
    updateSettingsFromUi();
    saveSettings();
}

void SettingsDialog::on_buttonBox_rejected()
{
    loadSettings();
    updateUiFromSettings();
}

void SettingsDialog::on_useGrayCodeCHB_toggled(bool checked)
{
    QSettings s;
    if (checked) {
        setStringsValue(ui->stringsSPB->value());
        ui->stringsSPB->setValue(ui->stringsSPB->maximum());
        ui->stringsSPB->setEnabled(false);
        return;
    }
    ui->stringsSPB->setValue(getStringsValue());
    ui->stringsSPB->setEnabled(true);
}

void SettingsDialog::disableGpu()
{
    setUseGpu(false);
    saveSettings();
    ui->useGpuCHB->setChecked(false);
}
void SettingsDialog::toggleStringsSPB(bool b) {
    ui->stringsSPB->setEnabled(b);
}
void SettingsDialog::toggleUseGpuCHB(bool b) {
    ui->useGpuCHB->setEnabled(b);
}
void SettingsDialog::toggleUseGrayCodeCHB(bool b) {
    ui->useGrayCodeCHB->setEnabled(b);
}
void SettingsDialog::handleMatrixChanged(int v)
{
    settings[STRINGS_MAX_VALUE] = v;
    ui->stringsSPB->setMaximum(v);
    ui->stringsSPB->setValue(v);
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

void SettingsDialog::saveSettings() {

    QSettings s;

    s.setValue( SPECTRUM_COLOR,        settings[ SPECTRUM_COLOR        ].get<int>()  );
    s.setValue( PROGRESSBAR_MS,        settings[ PROGRESSBAR_MS        ].get<int>()  );
    s.setValue( SPECTRUM_MS,           settings[ SPECTRUM_MS           ].get<int>()  );
    s.setValue( USE_GPU_CHECKED,       settings[ USE_GPU_CHECKED       ].get<bool>() );
    s.setValue( USE_GRAY_CODE_CHECKED, settings[ USE_GRAY_CODE_CHECKED ].get<bool>() );
    s.setValue( STRINGS_VALUE,         settings[ STRINGS_VALUE         ].get<int>()  );
    s.setValue( STRINGS_MAX_VALUE,     settings[ STRINGS_MAX_VALUE     ].get<int>()  );
    s.setValue( TRANSPARENCY_VALUE,    settings[ TRANSPARENCY_VALUE    ].get<int>()  );
    s.setValue( SETTINGS_GEOMETRY,     this->saveGeometry()    );
    s.sync();

}
void SettingsDialog::loadSettings() {

    QSettings s;

    if ( s.contains( SETTINGS_GEOMETRY ) ) this->restoreGeometry( s.value( SETTINGS_GEOMETRY ).toByteArray() );

    settings[ SPECTRUM_COLOR        ] = s.value( SPECTRUM_COLOR,        DEFAULT_SPECTRUM_COLOR     ).toInt();
    settings[ PROGRESSBAR_MS        ] = s.value( PROGRESSBAR_MS,        DEFAULT_PROGRESSBAR_MS     ).toInt();
    settings[ SPECTRUM_MS           ] = s.value( SPECTRUM_MS,           DEFAULT_SPECTRUM_MS        ).toInt();
    settings[ USE_GPU_CHECKED       ] = s.value( USE_GPU_CHECKED,       DEFAULT_USE_GPU            ).toBool();
    settings[ USE_GRAY_CODE_CHECKED ] = s.value( USE_GRAY_CODE_CHECKED, DEFAULT_USE_GRAY_CODE      ).toBool();
    settings[ STRINGS_VALUE         ] = s.value( STRINGS_VALUE,         DEFAULT_STRINGS_VALUE      ).toInt();
    settings[ STRINGS_MAX_VALUE     ] = s.value( STRINGS_MAX_VALUE,     DEFAULT_STRINGS_MAX_VALUE  ).toInt();
    settings[ TRANSPARENCY_VALUE    ] = s.value( TRANSPARENCY_VALUE,    DEFAULT_TRANSPARENCY_VALUE ).toInt();

}

void SettingsDialog::updateUiFromSettings()
{
    int  spectrumColor        = getHistoColorValue();
    int  refreshProgressbarMs = getRefreshProgressbarValue();
    int  refreshSpectrumMs    = getRefreshSpectrumValue();
    int  strVal               = getStringsValue();
    int  strMaxVal            = getStringsMaxValue();
    bool useGPUChecked        = isGpuUsed();
    bool useGrayCodeChecked   = isGrayCodeUsed();
    int  trpVal               = getTransparencyValue();

    ui->histoColorCMB->setCurrentIndex(
        ui->histoColorCMB->findData(spectrumColor)
    );
    ui->refreshProgressbarCMB->setCurrentIndex(
        ui->refreshProgressbarCMB->findData(refreshProgressbarMs)
    );
    ui->stringsSPB->setValue(
        strVal
    );
    ui->stringsSPB->setMaximum(
        strMaxVal
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
    if (useGrayCodeChecked) {
        ui->stringsSPB->setEnabled(false);
        ui->stringsSPB->setValue(ui->stringsSPB->maximum());
    }
}

void SettingsDialog::updateSettingsFromUi()
{
    QSettings s;
    s.setValue(SETTINGS_GEOMETRY, this->saveGeometry());

    int colVal = ui->histoColorCMB->currentData().toInt();
    int barVal = ui->refreshProgressbarCMB->currentData().toInt();
    int sptrVal = ui->refreshSpectrumCMB->currentData().toInt();
    int strVal = ui->stringsSPB->value();
    bool useGPUChecked = ui->useGpuCHB->isChecked();
    bool useGrayCodeChecked = ui->useGrayCodeCHB->isChecked();
    int trpVal = ui->transparencySPB->value();

    if (isGpuUsed() != useGPUChecked)
        emit useGpuToggled(useGPUChecked);

    if (isGrayCodeUsed() != useGrayCodeChecked)
        emit useGrayCodeToggled(useGrayCodeChecked);

    if (getRefreshProgressbarValue() != barVal)
        emit refreshProgressbarValueChanged(barVal);
    
    if (getRefreshSpectrumValue() != sptrVal)
        emit refreshSpectrumValueChanged(sptrVal);

    setHistoColorValue(colVal);
    setRefreshProgressbarValue(barVal);
    
    setRefreshSpectrumValue(sptrVal);
    setUseGpu(useGPUChecked);
    setUseGrayCode(useGrayCodeChecked);
    if(!useGrayCodeChecked)
        setStringsValue(strVal);
    setTransparencyValue(trpVal);

}

int SettingsDialog::getHistoColorValue() const
{
    return settings[SPECTRUM_COLOR];
}

int SettingsDialog::getRefreshProgressbarValue() const
{
    return settings[PROGRESSBAR_MS];
}

int SettingsDialog::getRefreshSpectrumValue() const
{
    return settings[SPECTRUM_MS];
}

int SettingsDialog::getStringsValue() const
{
    return settings[STRINGS_VALUE];
}

int SettingsDialog::getStringsMaxValue() const
{
    return settings[STRINGS_MAX_VALUE];
}

bool SettingsDialog::isGpuUsed() const
{
    return settings[USE_GPU_CHECKED];
}

bool SettingsDialog::isGrayCodeUsed() const
{
    return settings[USE_GRAY_CODE_CHECKED];
}

int SettingsDialog::getTransparencyValue() const
{
    return settings[TRANSPARENCY_VALUE];
}

void SettingsDialog::setHistoColorValue(int v)
{
    settings[SPECTRUM_COLOR] = v;
}

void SettingsDialog::setRefreshProgressbarValue(int v)
{
    settings[PROGRESSBAR_MS] = v;
}

void SettingsDialog::setRefreshSpectrumValue(int v)
{
    settings[SPECTRUM_MS] = v;
}

void SettingsDialog::setStringsValue(int v)
{
    settings[STRINGS_VALUE] = v;
}

void SettingsDialog::setStringsMaxValue(int v)
{
    settings[STRINGS_MAX_VALUE] = v;
}

void SettingsDialog::setUseGpu(bool b)
{
    settings[USE_GPU_CHECKED] = b;
}

void SettingsDialog::setUseGrayCode(bool b)
{
    settings[USE_GRAY_CODE_CHECKED] = b;
}

void SettingsDialog::setTransparencyValue(int v)
{
    settings[TRANSPARENCY_VALUE] = v;
}