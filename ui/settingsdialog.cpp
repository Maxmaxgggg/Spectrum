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

    // Устанавливаем данные для save и update
    ui->saveSpectrumIntervalCBX->setItemData(0, TenSeconds);
    ui->saveSpectrumIntervalCBX->setItemData(1, ThirtySeconds);
    ui->saveSpectrumIntervalCBX->setItemData(2, OneMinute);
    ui->saveSpectrumIntervalCBX->setItemData(3, FiveMinutes);
    ui->saveSpectrumIntervalCBX->setItemData(4, TenMinutes);

    ui->updateSpectrumIntervalCBX->setItemData(0, OneSecond);
    ui->updateSpectrumIntervalCBX->setItemData(1, FiveSeconds);
    ui->updateSpectrumIntervalCBX->setItemData(2, TenSeconds);
    ui->updateSpectrumIntervalCBX->setItemData(3, ThirtySeconds);
    ui->updateSpectrumIntervalCBX->setItemData(4, OneMinute);

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
            emit sendSettingsToWidget(settings.toJson());
            accept();
        });
    connect(ui->buttonBox, &QDialogButtonBox::rejected,
        this, [this]() {
            loadSettings();
            // На всякий случай
            emit sendSettingsToWidget(settings.toJson());
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
    emit sendSettingsToWidget(settings.toJson());
}

void SettingsDialog::setInterfaceEnabled( bool enabled )
{
    if (!enabled) {
        // Выключаем весь интерфейс
        ui->algorithmGBX->setEnabled(             false );
        ui->enumTypeGBX->setEnabled(              false );
        ui->computeDeviceGBX->setEnabled(         false );
        ui->computeDeviceSettingsGBX->setEnabled( false );
        ui->saveAndUpdateSpectrumGBX->setEnabled( false );
    }
    else {
        // Включаем весь интерфейс
        ui->algorithmGBX->setEnabled(             true  );
        ui->enumTypeGBX->setEnabled(              true  );
        ui->computeDeviceGBX->setEnabled(         true  );
        ui->computeDeviceSettingsGBX->setEnabled( true  );
        ui->saveAndUpdateSpectrumGBX->setEnabled( true  );
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
            settings.algorithmType = Algorithm::SimpleXor;
        }
            
        // Отключаем возможность полного перебора
        ui->fullEnumRB->setEnabled(false);
        // Включаем частичный перебор
        ui->partialEnumRB->setChecked(true);
        settings.enumType = EnumerationType::Partial;
        // Находим максимальное количество строк, которые можем сложить, не выходя за uint64
        ui->maxRowsSPB->setMaximum(maxCombIndex(rows));
        ui->maxRowsSPB->setVisible(enumeratorBGP->checkedId() == EnumerationType::Partial);
        ui->maxRowsLBL->setVisible(enumeratorBGP->checkedId() == EnumerationType::Partial);
        if (settings.maxRows > ui->maxRowsSPB->maximum())
            settings.maxRows = ui->maxRowsSPB->value();
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
        if ( settings.maxRows > ui->maxRowsSPB->maximum() )
            settings.maxRows = ui->maxRowsSPB->value();
    }
    // Если размерность дуального кода больше 63
    if (cols - rows > 63) {
        dualCodeLength = Length::Long;
        // Отключаем возможность использование дуального кода для расчета
        ui->dualCodeRB->setEnabled(false);
        // Если во время вписывания новой матрицы был выбран расчет с использованием дуального кода
        if (ui->dualCodeRB->isChecked())
            // Если число строк больше 63, то выбираем расчет с использованием простого XOR-а
            if (rows > 63) {
                ui->simpleXorRB->setChecked(true);
                settings.algorithmType = Algorithm::SimpleXor;
                ui->enumTypeGBX->setVisible(true);
                ui->partialEnumRB->setChecked(true);
                ui->fullEnumRB->setEnabled(false);
            }
            // Иначе - с использованием кода грея
            else {
                ui->grayCodeRB->setChecked(true);
                settings.algorithmType = Algorithm::GrayCode;
            }
                
    }
    // -||- меньше 63
    else {
        dualCodeLength = Length::Short;
        ui->dualCodeRB->setEnabled(true);
    }
    // Отправляем новые настройки в виджет
    emit sendSettingsToWidget(settings.toJson());
}
bool SettingsDialog::isGpuAvailable() {
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess)
        return false;
    else
        return deviceCount > 0;
}
// Подгоняет пределы под конкретную видеокарту и объясняет их пользователем.
//
// Раньше потолок числа блоков был зашит как 200. Замеры показывают, что оптимум
// лежит за ним — при 46 мультипроцессорах это 276-368 блоков, — то есть лучшее
// значение через интерфейс просто нельзя было выбрать.
void SettingsDialog::applyDeviceLimits()
{
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess)
        return;

    const int sm = prop.multiProcessorCount;

    // С запасом: смысл имеет несколько блоков на мультипроцессор, но верхнюю
    // границу лучше не занижать — оптимум зависит от матрицы.
    ui->blocksGpuSPB->setMaximum(qMax(200, sm * 32));
    ui->threadsGpuSPB->setMaximum(prop.maxThreadsPerBlock);

    ui->blocksGpuLBL->setToolTip(
        tr("У видеокарты %1 мультипроцессоров.\n"
           "Меньше одного блока на мультипроцессор — половина карты простаивает.\n"
           "По замерам разумно брать от %2 блоков.")
            .arg(sm).arg(sm * 3));
    ui->threadsGpuLBL->setToolTip(
        tr("Предел устройства — %1 нитей в блоке.\n"
           "Для широких кодов доступно меньше: ядру не хватает регистров,\n"
           "и запуск будет отклонён с понятным сообщением.")
            .arg(prop.maxThreadsPerBlock));
}

void SettingsDialog::checkGpuAvailable() {
    if (isGpuAvailable()) {
        ui->gpuRB->setEnabled(true);
        applyDeviceLimits();
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

void SettingsDialog::saveSettings() {
    QSettings s;

    s.beginGroup("lastSettings");  // ← ВАЖНО

    settings.algorithmType = static_cast<Algorithm>(algorithmBGP->checkedId());
    settings.enumType = static_cast<EnumerationType>(enumeratorBGP->checkedId());
    settings.maxRows = ui->maxRowsSPB->value();
    settings.compDev = static_cast<ComputeDevice>(computeDeviceBGP->checkedId());

    settings.compDevSet.threadsCpu = ui->threadsCpuSPB->value();
    settings.compDevSet.blocksGpu = ui->blocksGpuSPB->value();
    settings.compDevSet.threadsGpu = ui->threadsGpuSPB->value();

    settings.timeIntSet.saveSpectrumInterval = ui->saveSpectrumIntervalCBX->currentData().toInt();
    settings.timeIntSet.updateSpectrumInterval = ui->updateSpectrumIntervalCBX->currentData().toInt();

    QJsonDocument doc(settings.toJson());
    s.setValue(SettingsKeys::COMPUTATION_SETTINGS, doc.toJson());

    s.endGroup(); // ← не забыть
}
void SettingsDialog::loadSettings() {
    QSettings s;

    s.beginGroup("lastSettings");  // ← ВАЖНО

    QByteArray data = s.value(SettingsKeys::COMPUTATION_SETTINGS).toByteArray();
    if (!data.isEmpty())
    {
        QJsonDocument doc = QJsonDocument::fromJson(data);
        settings = ComputationSettings::fromJson(doc.object());
    }

    s.endGroup(); // ← не забыть

    // дальше UI без изменений
    algorithmBGP->button(settings.algorithmType)->setChecked(true);
    ui->enumTypeGBX->setVisible(algorithmBGP->checkedId() == Algorithm::SimpleXor);

    enumeratorBGP->button(settings.enumType)->setChecked(true);
    ui->maxRowsSPB->setVisible(enumeratorBGP->checkedId() == EnumerationType::Partial);
    ui->maxRowsLBL->setVisible(enumeratorBGP->checkedId() == EnumerationType::Partial);
    ui->maxRowsSPB->setValue(settings.maxRows);

    computeDeviceBGP->button(settings.compDev)->setChecked(true);

    int maxThreads = omp_get_max_threads();
    ui->threadsCpuSPB->setMaximum(maxThreads);

    ui->blocksGpuLBL->setVisible(computeDeviceBGP->checkedId() == ComputeDevice::GPU);
    ui->blocksGpuSPB->setVisible(computeDeviceBGP->checkedId() == ComputeDevice::GPU);
    ui->threadsGpuLBL->setVisible(computeDeviceBGP->checkedId() == ComputeDevice::GPU);
    ui->threadsGpuSPB->setVisible(computeDeviceBGP->checkedId() == ComputeDevice::GPU);

    ui->threadsCpuSPB->setValue(std::min(maxThreads, settings.compDevSet.threadsCpu));
    ui->blocksGpuSPB->setValue(settings.compDevSet.blocksGpu);
    ui->threadsGpuSPB->setValue(settings.compDevSet.threadsGpu);

    int index = ui->saveSpectrumIntervalCBX->findData(settings.timeIntSet.saveSpectrumInterval);
    if (index != -1) {
        ui->saveSpectrumIntervalCBX->setCurrentIndex(index);
    }
    index = ui->updateSpectrumIntervalCBX->findData(settings.timeIntSet.updateSpectrumInterval);
    if (index != -1) {
        ui->updateSpectrumIntervalCBX->setCurrentIndex(index);
    }
}