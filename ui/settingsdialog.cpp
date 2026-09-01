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
    settings.autoTuneGrid = ui->autoTuneGridCHB->isChecked();

    connect(algorithmBGP, &QButtonGroup::idClicked,
        this, [this](int) { updateEnumTypeControls(); });
    //Если выбран полный перебор, то отключаем выбор числа строк
    connect(enumeratorBGP, &QButtonGroup::idClicked,
        this, [this](int id) {
            // Запоминается только осознанный выбор пользователя: idClicked
            // на программное setChecked не приходит, поэтому принудительный
            // «Полный» на коде Грея сюда не попадает и выбор не затирает.
            xorEnumType = static_cast<EnumerationType>(id);
            updateEnumTypeControls();
        });
    // Вписанное число строк запоминается отдельно. Сигналы поля на время
    // программной установки блокируются, так что сюда доходит только ввод
    // пользователя и подрезка по новому максимуму при смене матрицы.
    connect(ui->maxRowsSPB, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this](int value) { xorMaxRows = value; });
    connect(computeDeviceBGP, &QButtonGroup::idClicked,
        this, [=](int id) {
            Q_UNUSED(id);
            updateDeviceControls();
        });
    connect(ui->autoTuneGridCHB, &QCheckBox::toggled,
        this, [this](bool) { updateDeviceControls(); });
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

// Группа «Тип перебора» имеет смысл только для простого XOR: код Грея и
// дуальный расчёт идут по всем 2^k маскам, частичного перебора у них нет.
//
// Раньше группа на них просто исчезала, и вкладка оставалась полупустой.
// Теперь она блокируется и показывает «Полный» — видно, что вариант есть,
// но к этому алгоритму неприменим.
void SettingsDialog::updateEnumTypeControls()
{
    const bool forXor = algorithmBGP->checkedId() == Algorithm::SimpleXor;

    ui->enumTypeGBX->setEnabled(forXor);

    EnumerationType shown = EnumerationType::Full;
    if (forXor) {
        shown = xorEnumType;
        // У длинных кодов полный перебор запрещён независимо от того, что
        // пользователь выбирал раньше.
        if (!ui->fullEnumRB->isEnabled())
            shown = EnumerationType::Partial;
    }

    if (QAbstractButton* button = enumeratorBGP->button(shown))
        button->setChecked(true);

    const bool partial = shown == EnumerationType::Partial;

    // Поле не прячется, а блокируется: при полном переборе оно показывает,
    // сколько строк складывается на самом деле — все, сколько их в матрице.
    // Пропадавшая надпись оставляла на её месте дыру, да и не было видно,
    // что настройка вообще есть.
    ui->maxRowsLBL->setEnabled(partial);
    ui->maxRowsSPB->setEnabled(partial);

    // Без блокировки сигналов setValue сам же и затёр бы запомненное число.
    const QSignalBlocker block(ui->maxRowsSPB);
    ui->maxRowsSPB->setValue(partial
        ? qBound(ui->maxRowsSPB->minimum(), xorMaxRows, ui->maxRowsSPB->maximum())
        : ui->maxRowsSPB->maximum());
}

void SettingsDialog::setInterfaceEnabled( bool enabled )
{
    // Гасятся страницы, а не сам QTabWidget: иначе вместе с ними отключится
    // и полоса вкладок, и во время расчёта нельзя будет даже посмотреть, что
    // выставлено на соседней.
    ui->computationTab->setEnabled(enabled);
    ui->deviceTab->setEnabled(enabled);
    ui->savingTab->setEnabled(enabled);

    if (enabled) {
        // Часть пунктов недоступна и в покое: код Грея не бывает длиннее
        // 63 строк, дуальный расчёт — тоже.
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

void SettingsDialog::applyFromAutosave(int algorithm, int enumType, int maxRows)
{
    if (QAbstractButton* button = algorithmBGP->button(algorithm))
        button->setChecked(true);

    if (algorithm == Algorithm::SimpleXor)
        xorEnumType = static_cast<EnumerationType>(enumType);
    if (maxRows > 0)
        xorMaxRows = maxRows;

    updateEnumTypeControls();

    settings.algorithmType = static_cast<Algorithm>(algorithm);
    settings.enumType      = static_cast<EnumerationType>(enumeratorBGP->checkedId());
    settings.maxRows       = ui->maxRowsSPB->value();

    emit sendSettingsToWidget(settings.toJson());
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
            settings.algorithmType = Algorithm::SimpleXor;
        }
            
        // Отключаем возможность полного перебора
        ui->fullEnumRB->setEnabled(false);
        // Включаем частичный перебор
        ui->partialEnumRB->setChecked(true);
        settings.enumType = EnumerationType::Partial;
        // Находим максимальное количество строк, которые можем сложить, не выходя за uint64
        ui->maxRowsSPB->setMaximum(maxCombIndex(rows));
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
    // Доступность полного перебора и сам алгоритм могли только что поменяться —
    // приводим группу в согласованный вид одним местом, а не в каждой ветке.
    updateEnumTypeControls();

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

// Поля числа блоков и нитей нужны только видеокарте, а при включённом
// автоподборе ещё и не используются — тогда они остаются на виду, но
// недоступны: так видно, что значения не потеряны, просто не в деле.
void SettingsDialog::updateDeviceControls()
{
    const bool gpu  = computeDeviceBGP->checkedId() == ComputeDevice::GPU;
    const bool auto_ = gpu && ui->autoTuneGridCHB->isChecked();

    ui->blocksGpuLBL->setVisible(gpu);
    ui->blocksGpuSPB->setVisible(gpu);
    ui->threadsGpuLBL->setVisible(gpu);
    ui->threadsGpuSPB->setVisible(gpu);
    ui->autoTuneGridCHB->setVisible(gpu);

    ui->blocksGpuLBL->setEnabled(!auto_);
    ui->blocksGpuSPB->setEnabled(!auto_);
    ui->threadsGpuLBL->setEnabled(!auto_);
    ui->threadsGpuSPB->setEnabled(!auto_);
}

void SettingsDialog::checkGpuAvailable() {
    if (isGpuAvailable()) {
        ui->gpuRB->setEnabled(true);
        applyDeviceLimits();
    }
    else {
        ui->gpuRB->setEnabled(false);
        ui->cpuRB->setChecked(true);
        updateDeviceControls();
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
    settings.autoTuneGrid          = ui->autoTuneGridCHB->isChecked();

    settings.timeIntSet.saveSpectrumInterval = ui->saveSpectrumIntervalCBX->currentData().toInt();
    settings.timeIntSet.updateSpectrumInterval = ui->updateSpectrumIntervalCBX->currentData().toInt();

    QJsonDocument doc(settings.toJson());
    s.setValue(SettingsKeys::COMPUTATION_SETTINGS, doc.toJson());
    // Отдельно от JSON: в settings.enumType при коде Грея лежит «Полный», и
    // выбор пользователя для XOR там не сохранить.
    s.setValue(SettingsKeys::XOR_ENUM_TYPE, int(xorEnumType));
    s.setValue(SettingsKeys::XOR_MAX_ROWS,  xorMaxRows);

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

    // дальше UI без изменений
    algorithmBGP->button(settings.algorithmType)->setChecked(true);

    // Выбор для XOR хранится отдельно от settings: в самих настройках при
    // выбранном коде Грея лежит «Полный», иначе выбор терялся бы при каждом
    // перезапуске.
    xorEnumType = static_cast<EnumerationType>(
        s.value(SettingsKeys::XOR_ENUM_TYPE, int(settings.enumType)).toInt());

    ui->maxRowsSPB->setValue(settings.maxRows);
    xorMaxRows = s.value(SettingsKeys::XOR_MAX_ROWS, settings.maxRows).toInt();

    updateEnumTypeControls();

    s.endGroup(); // ← не забыть

    computeDeviceBGP->button(settings.compDev)->setChecked(true);

    int maxThreads = omp_get_max_threads();
    ui->threadsCpuSPB->setMaximum(maxThreads);

    ui->threadsCpuSPB->setValue(std::min(maxThreads, settings.compDevSet.threadsCpu));
    ui->blocksGpuSPB->setValue(settings.compDevSet.blocksGpu);
    ui->threadsGpuSPB->setValue(settings.compDevSet.threadsGpu);
    ui->autoTuneGridCHB->setChecked(settings.autoTuneGrid);

    updateDeviceControls();

    int index = ui->saveSpectrumIntervalCBX->findData(settings.timeIntSet.saveSpectrumInterval);
    if (index != -1) {
        ui->saveSpectrumIntervalCBX->setCurrentIndex(index);
    }
    index = ui->updateSpectrumIntervalCBX->findData(settings.timeIntSet.updateSpectrumInterval);
    if (index != -1) {
        ui->updateSpectrumIntervalCBX->setCurrentIndex(index);
    }
}