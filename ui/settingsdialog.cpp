#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include <QSettings>
#include <QColorDialog>
#include <qtimer.h>
#include <omp.h>

using Algorithm       = ComputationSettings::Algorithm;
using EnumerationType = ComputationSettings::EnumerationType;
using ComputeDevice   = ComputationSettings::ComputeDevice;

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);
    // Создаём группу радиокнопок, отвечающую за выбор алгоритма
    algorithmBGP = new QButtonGroup(this);
    algorithmBGP->addButton( ui->simpleXorRB, Algorithm::SimpleXor );
    algorithmBGP->addButton( ui->grayCodeRB,  Algorithm::GrayCode  );
    algorithmBGP->addButton( ui->dualCodeRB,  Algorithm::DualCode  );

    enumeratorBGP = new QButtonGroup(this);
    enumeratorBGP->addButton( ui->fullEnumRB,    EnumerationType::Full    );
    enumeratorBGP->addButton( ui->partialEnumRB, EnumerationType::Partial );

    computeDeviceBGP = new QButtonGroup(this);
    computeDeviceBGP->addButton( ui->cpuRB, ComputeDevice::CPU );
    computeDeviceBGP->addButton( ui->gpuRB, ComputeDevice::GPU );


    loadSettings();
    checkGpuAvailable();

    settings.algorithmType = static_cast<Algorithm>(algorithmBGP->checkedId());
    settings.enumType = static_cast<EnumerationType>(enumeratorBGP->checkedId());
    settings.maxRows = ui->maxRowsSPB->value();
    settings.compDev = static_cast<ComputeDevice>(computeDeviceBGP->checkedId());
    
    int maxThreads = omp_get_max_threads();
    ui->threadsCpuSPB->setMaximum(maxThreads);
    settings.compDevSet.threadsCpu = std::min(maxThreads, ui->threadsCpuSPB->value());
    settings.compDevSet.blocksGpu = ui->blocksGpuSPB->value();
    settings.compDevSet.threadsGpu = ui->threadsGpuSPB->value();



    connect(algorithmBGP, &QButtonGroup::idClicked,
        this, [=](int id)
        {
            ui->enumTypeGBX->setVisible( id == Algorithm::SimpleXor );
        });
    //Если выбран полный перебор, то отключаем выбор числа строк
    connect(enumeratorBGP, &QButtonGroup::idClicked,
        this, [=](int id) {
            ui->maxRowsSPB->setVisible( id == EnumerationType::Partial );
            ui->maxRowsSPB->setValue(   ui->maxRowsSPB->maximum()     );
            ui->maxRowsLBL->setVisible( id == EnumerationType::Partial );
        });
    connect(computeDeviceBGP, &QButtonGroup::idClicked,
        this, [=](int id) {
            //ui->threadsCpuLBL->setVisible( id == ComputeDevice::CPU );
            //ui->threadsCpuSPB->setVisible( id == ComputeDevice::CPU );
            ui->blocksGpuLBL->setVisible(  id == ComputeDevice::GPU );
            ui->blocksGpuSPB->setVisible(  id == ComputeDevice::GPU );
            ui->threadsGpuLBL->setVisible( id == ComputeDevice::GPU );
            ui->threadsGpuSPB->setVisible( id == ComputeDevice::GPU );
        });
    connect(ui->buttonBox, &QDialogButtonBox::accepted,
        this, [this]() {
            saveSettings();
            accept();
        });
    connect(ui->buttonBox, &QDialogButtonBox::rejected,
        this, [this]() {
            loadSettings();
            reject();
        });
    // Отключаем кнопку помощи
    setWindowFlags( windowFlags()  & ~Qt::WindowContextHelpButtonHint );
}

SettingsDialog::~SettingsDialog()
{
    saveSettings();
}

void SettingsDialog::handleSettingsRequested()
{
    emit sendSettingsToWorker(settings.toJson());
}

void SettingsDialog::setInterfaceEnabled( bool enabled )
{
    if (!enabled) {
        // Выключаем весь интерфейс
        ui->algorithmGBX->setEnabled(             false );
        ui->enumTypeGBX->setEnabled(              false );
        ui->computeDeviceGBX->setEnabled(         false );
        ui->computeDeviceSettingsGBX->setEnabled( false );
    }
    else {
        // Включаем весь интерфейс
        ui->algorithmGBX->setEnabled(             true  );
        ui->enumTypeGBX->setEnabled(              true  );
        ui->computeDeviceGBX->setEnabled(         true  );
        ui->computeDeviceSettingsGBX->setEnabled( true  );
        // Частично выключаем его
        if ( codeLength == Length::Short ) {
            ui->grayCodeRB->setEnabled(true);
            // Включаем возможность полного перебора
            ui->fullEnumRB->setEnabled(true);
        }
        else {
            ui->grayCodeRB->setEnabled(false);
            // Отключаем возможность полного перебора
            ui->fullEnumRB->setEnabled(false);
            // Включаем частичный перебор
            ui->partialEnumRB->setChecked(true);
        }
        if ( dualCodeLength == Length::Short ) {
            ui->dualCodeRB->setEnabled(true);
        }
        else {
            ui->dualCodeRB->setEnabled(false);
        }
    }
}

void SettingsDialog::handleMatrixChanged(int rows, int cols) {
    // Если число строк порождающей матрицы больше 63
    if (rows > 63) {
        codeLength = Length::Long;
        // Отключаем возможность использования кода грея
        ui->grayCodeRB->setEnabled(false);
        // Если перед отключением было включено использование кода Грея, то насильно выключаем его
        if ( ui->grayCodeRB->isChecked() ){
            ui->simpleXorRB->setChecked(true);
            ui->enumTypeGBX->setVisible(true);
        }
            
        // Отключаем возможность полного перебора
        ui->fullEnumRB->setEnabled(false);
        // Включаем частичный перебор
        ui->partialEnumRB->setChecked(true);
        
        // Находим максимальное количество строк, которые можем сложить, не выходя за uint64
        ui->maxRowsSPB->setMaximum(maxCombIndex(rows));
        ui->maxRowsSPB->setVisible(enumeratorBGP->checkedId() == EnumerationType::Partial);
        ui->maxRowsLBL->setVisible(enumeratorBGP->checkedId() == EnumerationType::Partial);
    }
    // -||- меньше 63
    else {
        codeLength = Length::Short;
        // Включаем возможность использования кода грея
        ui->grayCodeRB->setEnabled(true);
        // Включаем возможность полного перебора
        ui->fullEnumRB->setEnabled(true);
        // Устанавливаем максимальное количество строк равным числу строк порождающей матрицы
        ui->maxRowsSPB->setMaximum(rows);
    }
    // Если размерность дуального кода больше 63
    if (cols - rows > 63) {
        dualCodeLength = Length::Long;
        // Отключаем возможность использование дуального кода для расчета
        ui->dualCodeRB->setEnabled(false);
        // Если во время вписывания новой матрицы был выбран расчет с использованием дуального кода
        if (ui->dualCodeRB->isChecked())
            // Если число строк больше 63, то выбираем расчет с использованием простого XOR-а
            if (rows > 63)
                ui->simpleXorRB->setChecked(true);
            // Иначе - с использованием кода грея
            else
                ui->grayCodeRB->setChecked(true);
    }
    // -||- меньше 63
    else {
        dualCodeLength = Length::Short;
        ui->dualCodeRB->setEnabled(true);
    }
}
bool SettingsDialog::isGpuAvailable() {
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess)
        return false;
    else
        return deviceCount > 0;
}
void SettingsDialog::checkGpuAvailable() {
    if (isGpuAvailable()) {
        ui->gpuRB->setEnabled(true);
    }
    else {
        ui->gpuRB->setEnabled(false);
        ui->cpuRB->setChecked(true);
        //ui->threadsCpuLBL->setVisible(computeDeviceBGP->checkedId() == ComputeDevice::CPU);
        //ui->threadsCpuSPB->setVisible(computeDeviceBGP->checkedId() == ComputeDevice::CPU);
        ui->blocksGpuLBL->setVisible(computeDeviceBGP->checkedId()  == ComputeDevice::GPU);
        ui->blocksGpuSPB->setVisible(computeDeviceBGP->checkedId()  == ComputeDevice::GPU);
        ui->threadsGpuLBL->setVisible(computeDeviceBGP->checkedId() == ComputeDevice::GPU);
        ui->threadsGpuSPB->setVisible(computeDeviceBGP->checkedId() == ComputeDevice::GPU);
    }
}

// Сохраняем настройки в реестр
void SettingsDialog::saveSettings() {
    QSettings s;
    settings.algorithmType = static_cast<Algorithm>(algorithmBGP->checkedId());
    settings.enumType      = static_cast<EnumerationType>(enumeratorBGP->checkedId());
    settings.maxRows       = ui->maxRowsSPB->value();
    settings.compDev       = static_cast<ComputeDevice>(computeDeviceBGP->checkedId());

    settings.compDevSet.threadsCpu = ui->threadsCpuSPB->value();
    settings.compDevSet.blocksGpu  = ui->blocksGpuSPB->value();
    settings.compDevSet.threadsGpu = ui->threadsGpuSPB->value();

    QJsonDocument doc(settings.toJson());
    s.setValue(SettingsKeys::COMPUTATION_SETTINGS, doc.toJson());
}
void SettingsDialog::loadSettings() {
    // Загружаем настройки из реестра
    QSettings s;
    QByteArray data = s.value(SettingsKeys::COMPUTATION_SETTINGS).toByteArray();
    if (!data.isEmpty())
    {
        QJsonDocument doc = QJsonDocument::fromJson(data);
        settings = ComputationSettings::fromJson(doc.object());
    }
    // Загружаем настройки в UI
    algorithmBGP->button(settings.algorithmType)->setChecked(true);
    ui->enumTypeGBX->setVisible(algorithmBGP->checkedId() == Algorithm::SimpleXor);

    enumeratorBGP->button(settings.enumType)->setChecked(true);
    ui->maxRowsSPB->setVisible(enumeratorBGP->checkedId() == EnumerationType::Partial);
    ui->maxRowsLBL->setVisible(enumeratorBGP->checkedId() == EnumerationType::Partial);
    ui->maxRowsSPB->setValue(settings.maxRows);

    computeDeviceBGP->button(settings.compDev)->setChecked(true);

    int maxThreads = omp_get_max_threads();
    ui->threadsCpuSPB->setMaximum(maxThreads);

    //ui->threadsCpuLBL->setVisible( computeDeviceBGP->checkedId() == ComputeDevice::CPU );
    //ui->threadsCpuSPB->setVisible( computeDeviceBGP->checkedId() == ComputeDevice::CPU );
    ui->blocksGpuLBL->setVisible(  computeDeviceBGP->checkedId() == ComputeDevice::GPU );
    ui->blocksGpuSPB->setVisible(  computeDeviceBGP->checkedId() == ComputeDevice::GPU );
    ui->threadsGpuLBL->setVisible( computeDeviceBGP->checkedId() == ComputeDevice::GPU );
    ui->threadsGpuSPB->setVisible( computeDeviceBGP->checkedId() == ComputeDevice::GPU );


    ui->threadsCpuSPB->setValue( std::min(maxThreads,settings.compDevSet.threadsCpu) );
    ui->blocksGpuSPB->setValue(  settings.compDevSet.blocksGpu  );
    ui->threadsGpuSPB->setValue( settings.compDevSet.threadsGpu );
}