#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include <QSettings>
#include <QColorDialog>
#include <qtimer.h>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);

    setData();
    setToolTips();
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
    emit sendInitialSettingsToWorker(settings);
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
    if (checked) {
        setStringsValue(ui->stringsSPB->value());
        ui->stringsSPB->setValue(ui->stringsSPB->maximum());
        ui->stringsSPB->setEnabled(false);
        return;
    }
    ui->useDualCodeCHB->setChecked(false);
    ui->stringsSPB->setValue(getStringsValue());
    ui->stringsSPB->setEnabled(true);
}

void SettingsDialog::on_useDualCodeCHB_toggled(bool checked)
{
    if (checked)
        ui->useGrayCodeCHB->setChecked(true);
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
void SettingsDialog::toggleUseDualCodeCHB(bool b)
{
    ui->useDualCodeCHB->setEnabled(b);
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

void SettingsDialog::setToolTips()
{
    ui->refreshSpectrumLBL->setToolTip(    QString::fromUtf8("Выбор минимального времени обновления гистограммы и текстового спектра\
                                                            \nПри тяжелых расчетах увеличивается автоматически"));

    ui->refreshProgressbarLBL->setToolTip( QString::fromUtf8("Выбор времени обновления шкалы прогресса"));
                                           
    ui->histoColorLBL->setToolTip(         QString::fromUtf8("Выбор цвета гистограммы"));
                                           
    ui->transparencyLBL->setToolTip(       QString::fromUtf8("Установка прозрачности гистограммы"));
                                           
    ui->useGpuLBL->setToolTip(             QString::fromUtf8("Включение использования видеокарты для расчета спектра кода\
                                                            \nЕсли выключено — расчет выполняется на процессоре"));

    ui->useGrayCodeLBL->setToolTip(        QString::fromUtf8("Включение использования кода Грея при генерации битовых масок\
                                                            \nБлокирует выбор максимального количества объединяемых строк\
                                                            \nПовышает производительность при переборе всех строк"));

    ui->useDualCodeLBL->setToolTip(        QString::fromUtf8("Включение расчета спектра кода через спектр дуального кода\
                                                            \nВключает использование кода Грея\
                                                            \nПовышает производительность при n − k < k"));

    ui->stringsLBL->setToolTip(            QString::fromUtf8("Установка максимального числа складываемых по модулю 2 строк при расчете спектра"));
    
}

void SettingsDialog::saveSettings() {

    QSettings s;
    s.setValue( SPECTRUM_COLOR,        settings.value( SPECTRUM_COLOR        ).toInt()  );
    s.setValue( PROGRESSBAR_MS,        settings.value( PROGRESSBAR_MS        ).toInt()  );
    s.setValue( SPECTRUM_MS,           settings.value( SPECTRUM_MS           ).toInt()  );
    s.setValue( USE_GPU_CHECKED,       settings.value( USE_GPU_CHECKED       ).toBool() );
    s.setValue( USE_GRAY_CODE_CHECKED, settings.value( USE_GRAY_CODE_CHECKED ).toBool() );
    s.setValue( USE_DUAL_CODE_CHECKED, settings.value( USE_DUAL_CODE_CHECKED ).toBool() );
    s.setValue( STRINGS_VALUE,         settings.value( STRINGS_VALUE         ).toInt()  );
    s.setValue( STRINGS_MAX_VALUE,     settings.value( STRINGS_MAX_VALUE     ).toInt()  );
    s.setValue( TRANSPARENCY_VALUE,    settings.value( TRANSPARENCY_VALUE    ).toInt()  );
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
    settings[ USE_DUAL_CODE_CHECKED ] = s.value( USE_DUAL_CODE_CHECKED, DEFAULT_USE_DUAL_CODE      ).toBool();
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
    bool useDualCodeChecked   = isDualCodeUsed();
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
    ui->useDualCodeCHB->setChecked(
        useDualCodeChecked
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
    bool useDualCodeChecked = ui->useDualCodeCHB->isChecked();

    int trpVal = ui->transparencySPB->value();

    if (isGpuUsed() != useGPUChecked)
        emit useGpuToggled(useGPUChecked);

    if (isGrayCodeUsed() != useGrayCodeChecked)
        emit useGrayCodeToggled(useGrayCodeChecked);
    if (isDualCodeUsed() != useDualCodeChecked)
        emit useDualCodeToggled(useDualCodeChecked);
    if (getRefreshProgressbarValue() != barVal)
        emit refreshProgressbarValueChanged(barVal);
    
    if (getRefreshSpectrumValue() != sptrVal)
        emit refreshSpectrumValueChanged(sptrVal);

    setHistoColorValue(colVal);
    setRefreshProgressbarValue(barVal);
    
    setRefreshSpectrumValue(sptrVal);
    setUseGpu(useGPUChecked);
    setUseGrayCode(useGrayCodeChecked);
    setUseDualCode(useDualCodeChecked);
    if(!useGrayCodeChecked)
        setStringsValue(strVal);
    setTransparencyValue(trpVal);

}

int SettingsDialog::getHistoColorValue() const
{
    return settings.value(SPECTRUM_COLOR).toInt();
}

int SettingsDialog::getRefreshProgressbarValue() const
{
    return settings.value(PROGRESSBAR_MS).toInt();
}

int SettingsDialog::getRefreshSpectrumValue() const
{
    return settings.value(SPECTRUM_MS).toInt();
}

int SettingsDialog::getStringsValue() const
{
    return settings.value(STRINGS_VALUE).toInt();
}

int SettingsDialog::getStringsMaxValue() const
{
    return settings.value(STRINGS_MAX_VALUE).toInt();
}

bool SettingsDialog::isGpuUsed() const
{
    return settings.value(USE_GPU_CHECKED).toBool();
}

bool SettingsDialog::isGrayCodeUsed() const
{
    return settings.value(USE_GRAY_CODE_CHECKED).toBool();
}

bool SettingsDialog::isDualCodeUsed() const
{
    return settings.value(USE_DUAL_CODE_CHECKED).toBool();
}

int SettingsDialog::getTransparencyValue() const
{
    return settings.value(TRANSPARENCY_VALUE).toInt();
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

void SettingsDialog::setUseDualCode(bool b)
{
    settings[USE_DUAL_CODE_CHECKED] = b;
}

void SettingsDialog::setTransparencyValue(int v)
{
    settings[TRANSPARENCY_VALUE] = v;
}