#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include "updateintervals.h"
#include "leon.h"
#include "gpuinfo.h"

#include <algorithm>

#include <QSettings>
#include <QStandardItemModel>
#include <QColorDialog>
#include <qtimer.h>
#include <omp.h>

using Algorithm       = ComputationSettings::Algorithm;
using EnumerationType = ComputationSettings::EnumerationType;
using ComputeDevice   = ComputationSettings::ComputeDevice;

namespace {

// Идентификаторы группы «Тип кода».
enum CodeKind { SingleCode = 0, ProductCode = 1 };

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);

    codeKindBGP = new QButtonGroup(this);
    codeKindBGP->addButton( ui->singleCodeRB,  SingleCode  );
    codeKindBGP->addButton( ui->productCodeRB, ProductCode );

    enumeratorBGP = new QButtonGroup(this);
    enumeratorBGP->addButton( ui->fullEnumRB,    EnumerationType::Full    );
    enumeratorBGP->addButton( ui->partialEnumRB, EnumerationType::Partial );

    // Одна группа на обе пары: видна всегда только подходящая пара, но
    // выбранной может быть лишь одна кнопка из четырёх.
    algorithmBGP = new QButtonGroup(this);
    algorithmBGP->addButton( ui->grayCodeRB,          Algorithm::GrayCode          );
    algorithmBGP->addButton( ui->dualCodeRB,          Algorithm::DualCode          );
    algorithmBGP->addButton( ui->brouwerZimmermannRB, Algorithm::BrouwerZimmermann );
    algorithmBGP->addButton( ui->randomInfoSetsRB,    Algorithm::RandomInfoSets    );

    // Степень десятки в данных пункта: так же, как интервалы.
    ui->leonMissCBX->setItemData(0, 3);
    ui->leonMissCBX->setItemData(1, 6);
    ui->leonMissCBX->setItemData(2, 9);
    ui->leonMissCBX->setItemData(3, 12);

    computeDeviceBGP = new QButtonGroup(this);
    computeDeviceBGP->addButton( ui->cpuRB, ComputeDevice::Cpu );
    computeDeviceBGP->addButton( ui->gpuRB, ComputeDevice::Gpu );

    // Устанавливаем данные для save и update
    ui->saveSpectrumIntervalCBX->setItemData(0, TenSeconds);
    ui->saveSpectrumIntervalCBX->setItemData(1, ThirtySeconds);
    ui->saveSpectrumIntervalCBX->setItemData(2, OneMinute);
    ui->saveSpectrumIntervalCBX->setItemData(3, FiveMinutes);
    ui->saveSpectrumIntervalCBX->setItemData(4, TenMinutes);

    ui->updateSpectrumIntervalCBX->setItemData(0, EveryTenthSecond);
    ui->updateSpectrumIntervalCBX->setItemData(1, EveryQuarterSecond);
    ui->updateSpectrumIntervalCBX->setItemData(2, EveryHalfSecond);
    ui->updateSpectrumIntervalCBX->setItemData(3, EverySecond);
    ui->updateSpectrumIntervalCBX->setItemData(4, EveryFiveSeconds);
    ui->updateSpectrumIntervalCBX->setItemData(5, EveryTenSeconds);
    ui->updateSpectrumIntervalCBX->setItemData(6, EveryThirtySeconds);
    ui->updateSpectrumIntervalCBX->setItemData(7, EveryMinute);

    loadSettings();
    checkGpuAvailable();
    collectSettings();

    int maxThreads = omp_get_max_threads();
    ui->threadsCpuSPB->setMaximum(maxThreads);
    settings.compDevSet.threadsCpu = std::min(maxThreads, ui->threadsCpuSPB->value());

    ui->maxPlotBarsSPB->setToolTip(
        tr("Сколько столбцов рисовать на графике.\n"
           "Если весов больше, соседние сливаются в один столбец,\n"
           "а их значения складываются — форма распределения сохраняется."));

    connect(codeKindBGP, &QButtonGroup::idClicked,
        this, [this](int) { updateComputationControls(); });
    connect(enumeratorBGP, &QButtonGroup::idClicked,
        this, [this](int id) {
            // Запоминается только осознанный выбор: idClicked на программное
            // setChecked не приходит, и принудительный «частичный» у
            // произведения или длинного кода выбор не затирает.
            singleEnumType = static_cast<EnumerationType>(id);
            updateComputationControls();
        });
    connect(algorithmBGP, &QButtonGroup::idClicked,
        this, [this](int id) {
            const Algorithm algorithm = static_cast<Algorithm>(id);
            if (algorithm == Algorithm::GrayCode || algorithm == Algorithm::DualCode)
                fullAlgorithm = algorithm;
            else
                partialAlgorithm = algorithm;
            updateComputationControls();
        });
    // Вес хранится по алгоритму: у Брауэра–Циммермана он маленький, у Леона
    // большой, у произведения ноль означает «до границы ранга». Поле одно,
    // и при смене алгоритма оно показывает свой вес.
    connect(ui->weightSPB, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this](int value) {
            if (!updatingControls)
                weightFor(currentAlgorithm()) = value;
            applyMemoryCap();
        });
    connect(ui->leonMemorySPB, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this](int) { applyMemoryCap(); });
    // Ранг выше первого — компоненты считает стохастический поиск, и его
    // настройки надо показать.
    connect(ui->productRankSPB, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this](int) { if (!updatingControls) updateComputationControls(); });
    connect(computeDeviceBGP, &QButtonGroup::idClicked,
        this, [=](int id) {
            Q_UNUSED(id);
            updateDeviceControls();
        });
    connect(ui->autoTuneGridCHB, &QCheckBox::toggled,
        this, [this](bool) { updateDeviceControls(); });
    // Потолок обновления зависит от кода, вычислителя, алгоритма и сетки.
    // Меняется любое из них — прежний замер больше не про эту конфигурацию,
    // и список снова закрывается.
    connect(codeKindBGP,      &QButtonGroup::idClicked, this, [this](int) { applyUpdateRateLimit(); });
    connect(enumeratorBGP,    &QButtonGroup::idClicked, this, [this](int) { applyUpdateRateLimit(); });
    connect(algorithmBGP,     &QButtonGroup::idClicked, this, [this](int) { applyUpdateRateLimit(); });
    connect(computeDeviceBGP, &QButtonGroup::idClicked, this, [this](int) { applyUpdateRateLimit(); });
    connect(ui->autoTuneGridCHB, &QCheckBox::toggled,   this, [this](bool) { applyUpdateRateLimit(); });
    for (QSpinBox* box : { ui->blocksGpuSPB, ui->threadsGpuSPB, ui->threadsCpuSPB, ui->weightSPB, ui->productRankSPB })
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) { applyUpdateRateLimit(); });

    connect(ui->measureRatePBN, &QPushButton::clicked, this, [this]() {
        collectSettings();
        ui->measureRatePBN->setEnabled(false);
        ui->measureRatePBN->setText(tr("Замеряю…"));
        ui->updateRateHintLBL->setText(tr("Идёт пробный расчёт…"));
        emit measureUpdateRateRequested(settings);
    });

    connect(ui->buttonBox, &QDialogButtonBox::accepted,
        this, [this]() {
            saveSettings();
            emit sendSettingsToWidget(settings);
            accept();
        });
    // Через QDialog::rejected, а не через кнопку: иначе закрытие крестиком или
    // клавишей Esc оставляло бы в полях изменения, которых нет в настройках, —
    // и при следующем открытии диалог показывал бы не то, с чем идёт расчёт.
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::rejected, this, [this]() {
        loadSettings();
        emit sendSettingsToWidget(settings);
    });
    applyUpdateRateLimit();

    // Отключаем кнопку помощи
    setWindowFlags( windowFlags()  & ~Qt::WindowContextHelpButtonHint );
}

SettingsDialog::~SettingsDialog()
{
    saveSettings();
}

// ------------------------------------------------------------ вкладка «Расчёт»

Algorithm SettingsDialog::currentAlgorithm() const
{
    if (codeKindBGP->checkedId() == ProductCode)
        return Algorithm::ProductCode;
    return enumeratorBGP->checkedId() == EnumerationType::Full ? fullAlgorithm : partialAlgorithm;
}

int& SettingsDialog::weightFor(Algorithm algorithm)
{
    switch (algorithm) {
        case Algorithm::RandomInfoSets: return settings.leonWeight;
        case Algorithm::ProductCode:    return settings.productWeight;
        default:                        return settings.bzWeight;
    }
}

void SettingsDialog::updateComputationControls()
{
    updatingControls = true;

    const bool product = codeKindBGP->checkedId() == ProductCode;

    // Тип перебора. У произведения он всегда частичный — по рангам; у
    // произвольного кода полный перебор возможен, пока хотя бы одна из
    // матриц — порождающая или проверочная — не длиннее 63 строк: код Грея
    // перебирает первую, дуальный расчёт — вторую.
    const bool fullPossible = !product && (codeLength == Length::Short || dualCodeLength == Length::Short);
    ui->fullEnumRB->setEnabled(fullPossible);
    ui->enumTypeGBX->setEnabled(!product);
    EnumerationType shown = product ? EnumerationType::Partial : singleEnumType;
    if (!fullPossible)
        shown = EnumerationType::Partial;
    if (QAbstractButton* button = enumeratorBGP->button(shown))
        button->setChecked(true);
    const bool full = shown == EnumerationType::Full;

    // Алгоритм: видна пара, подходящая типу перебора.
    ui->grayCodeRB->setVisible(full);
    ui->dualCodeRB->setVisible(full);
    ui->brouwerZimmermannRB->setVisible(!full);
    ui->randomInfoSetsRB->setVisible(!full);
    // Код Грея не длиннее 63 строк, дуальный расчёт — не длиннее 63 строк
    // проверочной матрицы.
    ui->grayCodeRB->setEnabled(codeLength == Length::Short);
    ui->dualCodeRB->setEnabled(dualCodeLength == Length::Short);
    if (full) {
        if (fullAlgorithm == Algorithm::GrayCode && !ui->grayCodeRB->isEnabled())
            fullAlgorithm = Algorithm::DualCode;
        if (fullAlgorithm == Algorithm::DualCode && !ui->dualCodeRB->isEnabled())
            fullAlgorithm = Algorithm::GrayCode;
    }
    if (QAbstractButton* button = algorithmBGP->button(full ? fullAlgorithm : partialAlgorithm))
        button->setChecked(true);

    const Algorithm algorithm = currentAlgorithm();
    const bool leon = partialAlgorithm == Algorithm::RandomInfoSets && !full;
    // У произведения алгоритм для компонент программа выбирает сама (по
    // размеру); переключатели не показываются, а настройки стохастического
    // поиска нужны только при рангах выше первого.
    ui->grayCodeRB->setVisible(full && !product);
    ui->dualCodeRB->setVisible(full && !product);
    ui->brouwerZimmermannRB->setVisible(!full && !product);
    ui->randomInfoSetsRB->setVisible(!full && !product);

    // Поле веса: у полного перебора его нет, у произведения ноль — «до
    // границы ранга».
    ui->weightLBL->setVisible(!full);
    ui->weightSPB->setVisible(!full);
    // Пока матрицы нет, предел не известен — оставляем широкий, иначе вес
    // из настроек подрезался бы до единицы при первом же открытии.
    ui->weightSPB->setMinimum(product ? 0 : 1);
    ui->weightSPB->setMaximum(product ? 10000000 : matrixCols > 0 ? matrixCols : 2048);
    ui->weightSPB->setValue(qBound(ui->weightSPB->minimum(), weightFor(algorithm), ui->weightSPB->maximum()));
    ui->weightLBL->setText(product ? tr("До веса (0 — до границы Толхёйзена): ") : tr("До веса: "));
    // Пояснения — только во всплывающей подсказке, не на вкладке.
    ui->weightSPB->setToolTip(
        product ? tr("Слова произведения не тяжелее этого веса; ноль — до границы Толхёйзена "
                     "d₁d₂ + max(d₁⌈d₂/2⌉, d₂⌈d₁/2⌉) − 1, ниже которой спектр произведения "
                     "точно выражается через спектры компонент")
        : leon  ? tr("Все слова не тяжелее этого веса будут найдены, если не случится "
                     "события заданной вероятности; тяжелее — не собираются")
                : tr("Все слова не тяжелее этого веса будут найдены и посчитаны точно; "
                     "число строк перебора программа выведет сама"));

    // Настройки Леона и произведения.
    const bool leonSettings = leon;
    ui->leonMissLBL->setVisible(leonSettings);
    ui->leonMissCBX->setVisible(leonSettings);
    ui->leonMemoryLBL->setVisible(leonSettings);
    ui->leonMemorySPB->setVisible(leonSettings);
    // Ранги выше первого пока не считаются: только граница Толхёйзена.
    ui->productRankLBL->setVisible(false);
    ui->productRankSPB->setVisible(false);
    applyMemoryCap();

    updatingControls = false;
}

void SettingsDialog::applyMemoryCap()
{
    const bool product = codeKindBGP->checkedId() == ProductCode;
    const bool full    = enumeratorBGP->checkedId() == EnumerationType::Full;
    const bool leon    = !product && !full && partialAlgorithm == Algorithm::RandomInfoSets;
    if (!leon || matrixCols <= 0 || matrixRows <= 0) {
        ui->weightMemoryLBL->setVisible(false);
        return;
    }
    const int     n     = matrixCols, k = matrixRows;
    const quint64 limit = ui->leonMemorySPB->value() > 0
        ? quint64(ui->leonMemorySPB->value()) << 20
        : std::max<quint64>(256ULL << 20, Leon::physicalMemoryBytes() / 2);
    const int cap = Leon::maxWeightForMemory(n, k, limit);

    // Потолок поля — по памяти; текущее значение подрезается, если вылезло.
    const bool wasUpdating = updatingControls;
    updatingControls = true;
    ui->weightSPB->setMaximum(cap);
    updatingControls = wasUpdating;
    if (ui->weightSPB->value() > cap) {
        ui->weightSPB->setValue(cap);
        weightFor(Algorithm::RandomInfoSets) = cap;
    }

    // Ожидаемый размер таблицы при этом весе.
    const double bytes = Leon::expectedWordsUpTo(n, k, ui->weightSPB->value())
                       * Leon::tableBytesPerWord((n + 63) / 64);
    QString size;
    if (bytes < (1 << 20))
        size = tr("< 1 МБ");
    else if (bytes < (1ULL << 30))
        size = tr("≈ %1 МБ").arg(qRound(bytes / (1 << 20)));
    else
        size = tr("≈ %1 ГБ").arg(bytes / (1ULL << 30), 0, 'f', 1);
    ui->weightMemoryLBL->setText(size);
    ui->weightMemoryLBL->setVisible(true);
    ui->weightMemoryLBL->setToolTip(
        tr("Ожидаемый размер таблицы найденных слов, как у случайного [%1,%2]-кода; "
           "предел веса — по памяти в настройках поиска. У кода со структурой лёгких слов "
           "больше, и таблица может не поместиться раньше")
            .arg(n).arg(k));
}

// ------------------------------------------------------------ замер потолка

QString SettingsDialog::updateRateKey() const
{
    // Матрицы диалог не видит, только её размер. Этого достаточно: потолок
    // определяется шириной строки и числом строк, а не тем, какие в матрице
    // биты.
    const Algorithm algorithm = currentAlgorithm();
    return QStringLiteral("%1|%2|%3x%4|%5|%6x%7|%8|%9|%10")
        .arg(computeDeviceBGP->checkedId())
        .arg(int(algorithm))
        .arg(matrixCols).arg(matrixRows)
        .arg(ui->weightSPB->value())
        .arg(ui->blocksGpuSPB->value()).arg(ui->threadsGpuSPB->value())
        .arg(ui->threadsCpuSPB->value())
        .arg(ui->autoTuneGridCHB->isChecked() ? 1 : 0)
        .arg(algorithm == Algorithm::ProductCode ? int(partialAlgorithm) * 10 + ui->productRankSPB->value() : 0);
}

void SettingsDialog::applyUpdateRateLimit()
{
    QComboBox* const box   = ui->updateSpectrumIntervalCBX;
    const bool       known = measuredRate > 0.0 && measuredFor == updateRateKey();

    auto* const model = qobject_cast<QStandardItemModel*>(box->model());
    for (int i = 0; i < box->count(); ++i) {
        if (QStandardItem* item = model ? model->item(i) : nullptr)
            item->setEnabled(!known || intervalReachable(box->itemData(i).toInt(), measuredRate));
    }

    box->setEnabled(known);

    if (!known) {
        // Замера нет — стоит секунда. Её выдаёт любая замеренная конфигурация:
        // худшая из них, широкий код 2000x50, даёт 6,9 обновления в секунду.
        const int index = box->findData(EverySecond);
        if (index != -1)
            box->setCurrentIndex(index);
        ui->updateRateHintLBL->setText(
            tr("Список откроется после замера: как часто спектр успевает "
               "обновляться, зависит от кода, вычислителя и сетки"));
        return;
    }

    // Выбранное значение могло стать недостижимым — например, после того как
    // подбор сетки выбрал сетку покрупнее.
    if (!intervalReachable(box->currentData().toInt(), measuredRate)) {
        QVector<int> intervals;
        for (int i = 0; i < box->count(); ++i)
            intervals.append(box->itemData(i).toInt());

        const int allowed = fastestAllowed(intervals, measuredRate);
        const int index   = allowed > 0 ? box->findData(allowed) : box->count() - 1;
        if (index != -1)
            box->setCurrentIndex(index);
    }

    ui->updateRateHintLBL->setText(
        tr("Замерено: быстрее %1 обн/с эта конфигурация не даёт")
            .arg(measuredRate, 0, 'f', 1));
}

void SettingsDialog::applyMeasuredRate(double perSecond)
{
    measuredRate = perSecond;
    measuredFor  = perSecond > 0.0 ? updateRateKey() : QString();

    ui->measureRatePBN->setEnabled(true);
    ui->measureRatePBN->setText(tr("Замерить"));
    applyUpdateRateLimit();

    if (perSecond <= 0.0)
        ui->updateRateHintLBL->setText(
            tr("Замер ничего не поймал: за пробу спектр не ушёл ни разу. "
               "Список остаётся на секунде"));
    saveSettings();
}

void SettingsDialog::handleSettingsRequested()
{
    emit sendSettingsToWidget(settings);
}

void SettingsDialog::setInterfaceEnabled( bool enabled )
{
    // Гасятся страницы, а не сам QTabWidget: иначе вместе с ними отключится
    // и полоса вкладок, и во время расчёта нельзя будет даже посмотреть, что
    // выставлено на соседней.
    ui->computationTab->setEnabled(enabled);
    ui->deviceTab->setEnabled(enabled);

    // Вкладка «Спектр» гаснуть не должна: там частота показа, интервал
    // автосохранения и число столбцов графика — они описывают показ, а не
    // задачу, и меняются на ходу. Всё остальное задаёт сам расчёт: под него
    // выделены буферы и посчитан объём работы, менять его на ходу — это
    // просто перезапустить.
    ui->savingTab->setEnabled(true);
    // Пробе нужен свободный вычислитель, а он сейчас занят расчётом.
    ui->measureRatePBN->setEnabled(enabled);

    if (enabled)
        updateComputationControls();
}

void SettingsDialog::applyFromAutosave(int algorithm, int enumType, int rank, int weight,
                                       int componentAlgorithm)
{
    Q_UNUSED(enumType);
    const Algorithm a = static_cast<Algorithm>(algorithm);

    switch (a) {
        case Algorithm::GrayCode:
        case Algorithm::DualCode:
            codeKindBGP->button(SingleCode)->setChecked(true);
            singleEnumType = EnumerationType::Full;
            fullAlgorithm  = a;
            break;
        case Algorithm::BrouwerZimmermann:
        case Algorithm::RandomInfoSets:
            codeKindBGP->button(SingleCode)->setChecked(true);
            singleEnumType   = EnumerationType::Partial;
            partialAlgorithm = a;
            if (weight > 0)
                weightFor(a) = weight;
            break;
        case Algorithm::ProductCode:
            codeKindBGP->button(ProductCode)->setChecked(true);
            partialAlgorithm = componentAlgorithm == int(Algorithm::RandomInfoSets)
                                 ? Algorithm::RandomInfoSets : Algorithm::BrouwerZimmermann;
            settings.productWeight = qMax(0, weight);
            if (rank > 0)
                ui->productRankSPB->setValue(qBound(1, rank, 4));
            break;
        default:
            // Простой XOR из интерфейса убран; запись показывается, а продолжать
            // её нечем — расчёт пойдёт Брауэром–Циммерманом заново.
            codeKindBGP->button(SingleCode)->setChecked(true);
            singleEnumType   = EnumerationType::Partial;
            partialAlgorithm = Algorithm::BrouwerZimmermann;
            break;
    }

    updateComputationControls();
    collectSettings();
    emit sendSettingsToWidget(settings);
}

void SettingsDialog::handleMatrixChanged(int rows, int cols) {
    matrixRows = rows;
    matrixCols = cols;
    // Код Грея перебирает 2^k масок в одном слове — не длиннее 63 строк;
    // дуальный расчёт перебирает проверочную матрицу, у неё n - k строк.
    codeLength     = rows > 63        ? Length::Long : Length::Short;
    dualCodeLength = cols - rows > 63 ? Length::Long : Length::Short;

    // Слов тяжелее длины кода не бывает.
    if (settings.bzWeight > cols)   settings.bzWeight   = qMax(1, cols);
    if (settings.leonWeight > cols) settings.leonWeight = qMax(1, cols);

    // Доступность полного перебора и сам алгоритм могли только что поменяться —
    // приводим вкладку в согласованный вид одним местом, а не в каждой ветке.
    updateComputationControls();
    collectSettings();
    // Сменилась матрица — прежний замер был про другой код.
    applyUpdateRateLimit();

    // Отправляем новые настройки в виджет
    emit sendSettingsToWidget(settings);
}

// ------------------------------------------------------------ вычислитель

bool SettingsDialog::isGpuAvailable() {
    return queryGpu().available;
}
// Подгоняет пределы под конкретную видеокарту и объясняет их пользователем.
//
// Раньше потолок числа блоков был зашит как 200. Замеры показывают, что оптимум
// лежит за ним — при 46 мультипроцессорах это 276-368 блоков, — то есть лучшее
// значение через интерфейс просто нельзя было выбрать.
void SettingsDialog::applyDeviceLimits()
{
    const GpuInfo gpu = queryGpu();
    if (!gpu.available)
        return;

    const int sm = gpu.multiprocessors;

    // С запасом: смысл имеет несколько блоков на мультипроцессор, но верхнюю
    // границу лучше не занижать — оптимум зависит от матрицы.
    ui->blocksGpuSPB->setMaximum(qMax(200, sm * 32));
    ui->threadsGpuSPB->setMaximum(gpu.maxThreadsPerBlock);

    ui->blocksGpuLBL->setToolTip(
        tr("У видеокарты %1 мультипроцессоров.\n"
           "Меньше одного блока на мультипроцессор — половина карты простаивает.\n"
           "По замерам разумно брать от %2 блоков.")
            .arg(sm).arg(sm * 3));
    ui->threadsGpuLBL->setToolTip(
        tr("Предел устройства — %1 нитей в блоке.\n"
           "Для широких кодов доступно меньше: ядру не хватает регистров,\n"
           "и запуск будет отклонён с понятным сообщением.")
            .arg(gpu.maxThreadsPerBlock));
}

// Поля числа блоков и нитей нужны только видеокарте, а при включённом
// автоподборе ещё и не используются — тогда они остаются на виду, но
// недоступны: так видно, что значения не потеряны, просто не в деле.
void SettingsDialog::updateDeviceControls()
{
    const bool gpu  = computeDeviceBGP->checkedId() == ComputeDevice::Gpu;
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

// ------------------------------------------------------------ хранение

void SettingsDialog::saveSettings() {
    QSettings s;

    s.beginGroup("lastSettings");  // ← ВАЖНО

    collectSettings();

    QJsonDocument doc(settings.toJson());
    s.setValue(SettingsKeys::COMPUTATION_SETTINGS, doc.toJson());
    // Отдельно от JSON: в самих настройках лежит один алгоритм, а помнить
    // надо выбор в обеих парах и тип перебора произвольного кода.
    s.setValue(SettingsKeys::FULL_ALGORITHM,    int(fullAlgorithm));
    s.setValue(SettingsKeys::PARTIAL_ALGORITHM, int(partialAlgorithm));
    s.setValue(SettingsKeys::SINGLE_ENUM_TYPE,  int(singleEnumType));
    // Замер живёт рядом с настройками: он свойство конфигурации, а не сеанса,
    // и переживать перезапуск обязан — иначе список закрывался бы каждый раз.
    s.setValue(SettingsKeys::UPDATE_RATE,     measuredRate);
    s.setValue(SettingsKeys::UPDATE_RATE_KEY, measuredFor);

    s.endGroup(); // ← не забыть
}

// Переносит значения из полей в settings, ничего не записывая на диск.
// Пробе нужны настройки, которые пользователь видит сейчас, а не те, что он
// подтвердил кнопкой.
void SettingsDialog::collectSettings()
{
    const Algorithm algorithm = currentAlgorithm();
    settings.algorithmType = algorithm;
    settings.enumType = enumeratorBGP->checkedId() == EnumerationType::Full
                          ? EnumerationType::Full : EnumerationType::Partial;
    // Число строк простого XOR больше не задаётся: всё, сколько есть.
    settings.maxRows = qMax(1, matrixRows);
    // Веса хранятся по алгоритму и обновляются по вводу; здесь — на случай,
    // если поле видно и его значение подрезал новый предел.
    if (algorithm == Algorithm::BrouwerZimmermann || algorithm == Algorithm::RandomInfoSets
        || algorithm == Algorithm::ProductCode)
        weightFor(algorithm) = ui->weightSPB->value();
    settings.productAlgorithm = partialAlgorithm;
    settings.leonMissExponent = ui->leonMissCBX->currentData().toInt();
    settings.leonMemoryMb = ui->leonMemorySPB->value();
    settings.productRank = 1;
    settings.compDev = static_cast<ComputeDevice>(computeDeviceBGP->checkedId());

    settings.compDevSet.threadsCpu = ui->threadsCpuSPB->value();
    settings.compDevSet.blocksGpu = ui->blocksGpuSPB->value();
    settings.compDevSet.threadsGpu = ui->threadsGpuSPB->value();
    settings.autoTuneGrid          = ui->autoTuneGridCHB->isChecked();
    settings.maxPlotBars           = ui->maxPlotBarsSPB->value();

    settings.timeIntSet.saveSpectrumInterval = ui->saveSpectrumIntervalCBX->currentData().toInt();
    settings.timeIntSet.updateSpectrumInterval = ui->updateSpectrumIntervalCBX->currentData().toInt();
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

    measuredRate = s.value(SettingsKeys::UPDATE_RATE, 0.0).toDouble();
    measuredFor  = s.value(SettingsKeys::UPDATE_RATE_KEY).toString();

    // Выбор в парах — из реестра, а сам алгоритм настроек его уточняет: они
    // могли разойтись, если настройки писала другая версия программы.
    fullAlgorithm = static_cast<Algorithm>(
        s.value(SettingsKeys::FULL_ALGORITHM, int(Algorithm::GrayCode)).toInt());
    partialAlgorithm = static_cast<Algorithm>(
        s.value(SettingsKeys::PARTIAL_ALGORITHM, int(Algorithm::BrouwerZimmermann)).toInt());
    singleEnumType = static_cast<EnumerationType>(
        s.value(SettingsKeys::SINGLE_ENUM_TYPE, int(EnumerationType::Full)).toInt());
    if (fullAlgorithm != Algorithm::GrayCode && fullAlgorithm != Algorithm::DualCode)
        fullAlgorithm = Algorithm::GrayCode;
    if (partialAlgorithm != Algorithm::BrouwerZimmermann && partialAlgorithm != Algorithm::RandomInfoSets)
        partialAlgorithm = Algorithm::BrouwerZimmermann;

    switch (settings.algorithmType) {
        case Algorithm::GrayCode:
        case Algorithm::DualCode:
            codeKindBGP->button(SingleCode)->setChecked(true);
            singleEnumType = EnumerationType::Full;
            fullAlgorithm  = settings.algorithmType;
            break;
        case Algorithm::BrouwerZimmermann:
        case Algorithm::RandomInfoSets:
            codeKindBGP->button(SingleCode)->setChecked(true);
            singleEnumType   = EnumerationType::Partial;
            partialAlgorithm = settings.algorithmType;
            break;
        case Algorithm::ProductCode:
            codeKindBGP->button(ProductCode)->setChecked(true);
            if (settings.productAlgorithm == Algorithm::RandomInfoSets
                || settings.productAlgorithm == Algorithm::BrouwerZimmermann)
                partialAlgorithm = static_cast<Algorithm>(settings.productAlgorithm);
            break;
        default:
            // Простой XOR: в новом интерфейсе его нет — ближайшее по смыслу.
            codeKindBGP->button(SingleCode)->setChecked(true);
            singleEnumType   = EnumerationType::Partial;
            partialAlgorithm = Algorithm::BrouwerZimmermann;
            break;
    }

    {
        const int at = ui->leonMissCBX->findData(settings.leonMissExponent);
        ui->leonMissCBX->setCurrentIndex(at >= 0 ? at : 2);
    }
    ui->leonMemorySPB->setValue(settings.leonMemoryMb);
    ui->productRankSPB->setValue(settings.productRank);

    updateComputationControls();

    s.endGroup(); // ← не забыть

    computeDeviceBGP->button(settings.compDev)->setChecked(true);

    int maxThreads = omp_get_max_threads();
    ui->threadsCpuSPB->setMaximum(maxThreads);

    ui->threadsCpuSPB->setValue(std::min(maxThreads, settings.compDevSet.threadsCpu));
    ui->blocksGpuSPB->setValue(settings.compDevSet.blocksGpu);
    ui->threadsGpuSPB->setValue(settings.compDevSet.threadsGpu);
    ui->autoTuneGridCHB->setChecked(settings.autoTuneGrid);
    ui->maxPlotBarsSPB->setValue(settings.maxPlotBars);

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
