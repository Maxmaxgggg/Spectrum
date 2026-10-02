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
    : QDialog(parent), m_ui(new Ui::SettingsDialog)
{
    m_ui->setupUi(this);

    m_codeKindBGP = new QButtonGroup(this);
    m_codeKindBGP->addButton( m_ui->singleCodeRB,  SingleCode  );
    m_codeKindBGP->addButton( m_ui->productCodeRB, ProductCode );

    m_enumeratorBGP = new QButtonGroup(this);
    m_enumeratorBGP->addButton( m_ui->fullEnumRB,    EnumerationType::Full    );
    m_enumeratorBGP->addButton( m_ui->partialEnumRB, EnumerationType::Partial );

    // Одна группа на обе пары: видна всегда только подходящая пара, но
    // выбранной может быть лишь одна кнопка из четырёх.
    m_algorithmBGP = new QButtonGroup(this);
    m_algorithmBGP->addButton( m_ui->grayCodeRB,          Algorithm::GrayCode          );
    m_algorithmBGP->addButton( m_ui->dualCodeRB,          Algorithm::DualCode          );
    m_algorithmBGP->addButton( m_ui->brouwerZimmermannRB, Algorithm::BrouwerZimmermann );
    m_algorithmBGP->addButton( m_ui->randomInfoSetsRB,    Algorithm::RandomInfoSets    );

    // Степень десятки в данных пункта: так же, как интервалы.
    m_ui->leonMissCBX->setItemData(0, 3);
    m_ui->leonMissCBX->setItemData(1, 6);
    m_ui->leonMissCBX->setItemData(2, 9);
    m_ui->leonMissCBX->setItemData(3, 12);

    m_computeDeviceBGP = new QButtonGroup(this);
    m_computeDeviceBGP->addButton( m_ui->cpuRB, ComputeDevice::Cpu );
    m_computeDeviceBGP->addButton( m_ui->gpuRB, ComputeDevice::Gpu );

    // Устанавливаем данные для save и update
    m_ui->saveSpectrumIntervalCBX->setItemData(0, TenSeconds);
    m_ui->saveSpectrumIntervalCBX->setItemData(1, ThirtySeconds);
    m_ui->saveSpectrumIntervalCBX->setItemData(2, OneMinute);
    m_ui->saveSpectrumIntervalCBX->setItemData(3, FiveMinutes);
    m_ui->saveSpectrumIntervalCBX->setItemData(4, TenMinutes);

    m_ui->updateSpectrumIntervalCBX->setItemData(0, EveryTenthSecond);
    m_ui->updateSpectrumIntervalCBX->setItemData(1, EveryQuarterSecond);
    m_ui->updateSpectrumIntervalCBX->setItemData(2, EveryHalfSecond);
    m_ui->updateSpectrumIntervalCBX->setItemData(3, EverySecond);
    m_ui->updateSpectrumIntervalCBX->setItemData(4, EveryFiveSeconds);
    m_ui->updateSpectrumIntervalCBX->setItemData(5, EveryTenSeconds);
    m_ui->updateSpectrumIntervalCBX->setItemData(6, EveryThirtySeconds);
    m_ui->updateSpectrumIntervalCBX->setItemData(7, EveryMinute);

    loadSettings();
    checkGpuAvailable();
    collectSettings();

    int maxThreads = omp_get_max_threads();
    m_ui->threadsCpuSPB->setMaximum(maxThreads);
    m_settings.compDevSet.threadsCpu = std::min(maxThreads, m_ui->threadsCpuSPB->value());

    m_ui->maxPlotBarsSPB->setToolTip(
        tr("Сколько столбцов рисовать на графике.\n"
           "Если весов больше, соседние сливаются в один столбец,\n"
           "а их значения складываются — форма распределения сохраняется."));

    connect(m_codeKindBGP, &QButtonGroup::idClicked,
        this, [this](int) { updateComputationControls(); });
    connect(m_enumeratorBGP, &QButtonGroup::idClicked,
        this, [this](int id) {
            // Запоминается только осознанный выбор: idClicked на программное
            // setChecked не приходит, и принудительный «частичный» у
            // произведения или длинного кода выбор не затирает.
            m_singleEnumType = static_cast<EnumerationType>(id);
            updateComputationControls();
        });
    connect(m_algorithmBGP, &QButtonGroup::idClicked,
        this, [this](int id) {
            const Algorithm algorithm = static_cast<Algorithm>(id);
            if (algorithm == Algorithm::GrayCode || algorithm == Algorithm::DualCode)
                m_fullAlgorithm = algorithm;
            else
                m_partialAlgorithm = algorithm;
            updateComputationControls();
        });
    // Вес хранится по алгоритму: у Брауэра–Циммермана он маленький, у Леона
    // большой, у произведения ноль означает «до границы ранга». Поле одно,
    // и при смене алгоритма оно показывает свой вес.
    connect(m_ui->weightSPB, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this](int value) {
            if (!m_updatingControls)
                weightFor(currentAlgorithm()) = value;
            applyMemoryCap();
        });
    connect(m_ui->leonMemorySPB, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this](int) { applyMemoryCap(); });
    // Ранг выше первого — компоненты считает стохастический поиск, и его
    // настройки надо показать.
    connect(m_ui->productRankSPB, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this](int) { if (!m_updatingControls) updateComputationControls(); });
    connect(m_computeDeviceBGP, &QButtonGroup::idClicked,
        this, [=](int id) {
            Q_UNUSED(id);
            updateDeviceControls();
        });
    connect(m_ui->autoTuneGridCHB, &QCheckBox::toggled,
        this, [this](bool) { updateDeviceControls(); });
    // Потолок обновления зависит от кода, вычислителя, алгоритма и сетки.
    // Меняется любое из них — прежний замер больше не про эту конфигурацию,
    // и список снова закрывается.
    connect(m_codeKindBGP,      &QButtonGroup::idClicked, this, [this](int) { applyUpdateRateLimit(); });
    connect(m_enumeratorBGP,    &QButtonGroup::idClicked, this, [this](int) { applyUpdateRateLimit(); });
    connect(m_algorithmBGP,     &QButtonGroup::idClicked, this, [this](int) { applyUpdateRateLimit(); });
    connect(m_computeDeviceBGP, &QButtonGroup::idClicked, this, [this](int) { applyUpdateRateLimit(); });
    connect(m_ui->autoTuneGridCHB, &QCheckBox::toggled,   this, [this](bool) { applyUpdateRateLimit(); });
    for (QSpinBox* box : { m_ui->blocksGpuSPB, m_ui->threadsGpuSPB, m_ui->threadsCpuSPB, m_ui->weightSPB, m_ui->productRankSPB })
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) { applyUpdateRateLimit(); });

    connect(m_ui->measureRatePBN, &QPushButton::clicked, this, [this]() {
        collectSettings();
        m_ui->measureRatePBN->setEnabled(false);
        m_ui->measureRatePBN->setText(tr("Замеряю…"));
        m_ui->updateRateHintLBL->setText(tr("Идёт пробный расчёт…"));
        emit measureUpdateRateRequested(m_settings);
    });

    connect(m_ui->buttonBox, &QDialogButtonBox::accepted,
        this, [this]() {
            saveSettings();
            emit sendSettingsToWidget(m_settings);
            accept();
        });
    // Через QDialog::rejected, а не через кнопку: иначе закрытие крестиком или
    // клавишей Esc оставляло бы в полях изменения, которых нет в настройках, —
    // и при следующем открытии диалог показывал бы не то, с чем идёт расчёт.
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::rejected, this, [this]() {
        loadSettings();
        emit sendSettingsToWidget(m_settings);
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
    if (m_codeKindBGP->checkedId() == ProductCode)
        return Algorithm::ProductCode;
    return m_enumeratorBGP->checkedId() == EnumerationType::Full ? m_fullAlgorithm : m_partialAlgorithm;
}

int& SettingsDialog::weightFor(Algorithm algorithm)
{
    switch (algorithm) {
        case Algorithm::RandomInfoSets: return m_settings.leonWeight;
        case Algorithm::ProductCode:    return m_settings.productWeight;
        default:                        return m_settings.bzWeight;
    }
}

void SettingsDialog::updateComputationControls()
{
    m_updatingControls = true;

    const bool product = m_codeKindBGP->checkedId() == ProductCode;

    // Тип перебора. У произведения он всегда частичный — по рангам; у
    // произвольного кода полный перебор возможен, пока хотя бы одна из
    // матриц — порождающая или проверочная — не длиннее 63 строк: код Грея
    // перебирает первую, дуальный расчёт — вторую.
    const bool fullPossible = !product && (m_codeLength == Length::Short || m_dualCodeLength == Length::Short);
    m_ui->fullEnumRB->setEnabled(fullPossible);
    m_ui->enumTypeGBX->setEnabled(!product);
    EnumerationType shown = product ? EnumerationType::Partial : m_singleEnumType;
    if (!fullPossible)
        shown = EnumerationType::Partial;
    if (QAbstractButton* button = m_enumeratorBGP->button(shown))
        button->setChecked(true);
    const bool full = shown == EnumerationType::Full;

    // Алгоритм: видна пара, подходящая типу перебора.
    m_ui->grayCodeRB->setVisible(full);
    m_ui->dualCodeRB->setVisible(full);
    m_ui->brouwerZimmermannRB->setVisible(!full);
    m_ui->randomInfoSetsRB->setVisible(!full);
    // Код Грея не длиннее 63 строк, дуальный расчёт — не длиннее 63 строк
    // проверочной матрицы.
    m_ui->grayCodeRB->setEnabled(m_codeLength == Length::Short);
    m_ui->dualCodeRB->setEnabled(m_dualCodeLength == Length::Short);
    if (full) {
        if (m_fullAlgorithm == Algorithm::GrayCode && !m_ui->grayCodeRB->isEnabled())
            m_fullAlgorithm = Algorithm::DualCode;
        if (m_fullAlgorithm == Algorithm::DualCode && !m_ui->dualCodeRB->isEnabled())
            m_fullAlgorithm = Algorithm::GrayCode;
    }
    if (QAbstractButton* button = m_algorithmBGP->button(full ? m_fullAlgorithm : m_partialAlgorithm))
        button->setChecked(true);

    const Algorithm algorithm = currentAlgorithm();
    const bool leon = m_partialAlgorithm == Algorithm::RandomInfoSets && !full;
    // У произведения алгоритм для компонент программа выбирает сама (по
    // размеру); переключатели не показываются, а настройки стохастического
    // поиска нужны только при рангах выше первого.
    m_ui->grayCodeRB->setVisible(full && !product);
    m_ui->dualCodeRB->setVisible(full && !product);
    m_ui->brouwerZimmermannRB->setVisible(!full && !product);
    m_ui->randomInfoSetsRB->setVisible(!full && !product);

    // Поле веса: у полного перебора его нет, у произведения ноль — «до
    // границы ранга».
    m_ui->weightLBL->setVisible(!full);
    m_ui->weightSPB->setVisible(!full);
    // Пока матрицы нет, предел не известен — оставляем широкий, иначе вес
    // из настроек подрезался бы до единицы при первом же открытии.
    m_ui->weightSPB->setMinimum(product ? 0 : 1);
    m_ui->weightSPB->setMaximum(product ? 10000000 : m_matrixCols > 0 ? m_matrixCols : 2048);
    m_ui->weightSPB->setValue(qBound(m_ui->weightSPB->minimum(), weightFor(algorithm), m_ui->weightSPB->maximum()));
    m_ui->weightLBL->setText(product ? tr("До веса (0 — до границы Толхёйзена): ") : tr("До веса: "));
    // Пояснения — только во всплывающей подсказке, не на вкладке.
    m_ui->weightSPB->setToolTip(
        product ? tr("Слова произведения не тяжелее этого веса; ноль — до границы Толхёйзена "
                     "d₁d₂ + max(d₁⌈d₂/2⌉, d₂⌈d₁/2⌉) − 1, ниже которой спектр произведения "
                     "точно выражается через спектры компонент")
        : leon  ? tr("Все слова не тяжелее этого веса будут найдены, если не случится "
                     "события заданной вероятности; тяжелее — не собираются")
                : tr("Все слова не тяжелее этого веса будут найдены и посчитаны точно; "
                     "число строк перебора программа выведет сама"));

    // Настройки Леона и произведения.
    const bool leonSettings = leon;
    m_ui->leonMissLBL->setVisible(leonSettings);
    m_ui->leonMissCBX->setVisible(leonSettings);
    m_ui->leonMemoryLBL->setVisible(leonSettings);
    m_ui->leonMemorySPB->setVisible(leonSettings);
    // Ранги выше первого пока не считаются: только граница Толхёйзена.
    m_ui->productRankLBL->setVisible(false);
    m_ui->productRankSPB->setVisible(false);
    applyMemoryCap();

    m_updatingControls = false;
}

void SettingsDialog::applyMemoryCap()
{
    const bool product = m_codeKindBGP->checkedId() == ProductCode;
    const bool full    = m_enumeratorBGP->checkedId() == EnumerationType::Full;
    const bool leon    = !product && !full && m_partialAlgorithm == Algorithm::RandomInfoSets;
    if (!leon || m_matrixCols <= 0 || m_matrixRows <= 0) {
        m_ui->weightMemoryLBL->setVisible(false);
        return;
    }
    const int     n     = m_matrixCols, k = m_matrixRows;
    const quint64 limit = m_ui->leonMemorySPB->value() > 0
        ? quint64(m_ui->leonMemorySPB->value()) << 20
        : std::max<quint64>(256ULL << 20, Leon::physicalMemoryBytes() / 2);
    const int cap = Leon::maxWeightForMemory(n, k, limit);

    // Потолок поля — по памяти; текущее значение подрезается, если вылезло.
    const bool wasUpdating = m_updatingControls;
    m_updatingControls = true;
    m_ui->weightSPB->setMaximum(cap);
    m_updatingControls = wasUpdating;
    if (m_ui->weightSPB->value() > cap) {
        m_ui->weightSPB->setValue(cap);
        weightFor(Algorithm::RandomInfoSets) = cap;
    }

    // Ожидаемый размер таблицы при этом весе.
    const double bytes = Leon::expectedWordsUpTo(n, k, m_ui->weightSPB->value())
                       * Leon::tableBytesPerWord((n + 63) / 64);
    QString size;
    if (bytes < (1 << 20))
        size = tr("< 1 МБ");
    else if (bytes < (1ULL << 30))
        size = tr("≈ %1 МБ").arg(qRound(bytes / (1 << 20)));
    else
        size = tr("≈ %1 ГБ").arg(bytes / (1ULL << 30), 0, 'f', 1);
    m_ui->weightMemoryLBL->setText(size);
    m_ui->weightMemoryLBL->setVisible(true);
    m_ui->weightMemoryLBL->setToolTip(
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
        .arg(m_computeDeviceBGP->checkedId())
        .arg(int(algorithm))
        .arg(m_matrixCols).arg(m_matrixRows)
        .arg(m_ui->weightSPB->value())
        .arg(m_ui->blocksGpuSPB->value()).arg(m_ui->threadsGpuSPB->value())
        .arg(m_ui->threadsCpuSPB->value())
        .arg(m_ui->autoTuneGridCHB->isChecked() ? 1 : 0)
        .arg(algorithm == Algorithm::ProductCode ? int(m_partialAlgorithm) * 10 + m_ui->productRankSPB->value() : 0);
}

void SettingsDialog::applyUpdateRateLimit()
{
    QComboBox* const box   = m_ui->updateSpectrumIntervalCBX;
    const bool       known = m_measuredRate > 0.0 && m_measuredFor == updateRateKey();

    auto* const model = qobject_cast<QStandardItemModel*>(box->model());
    for (int i = 0; i < box->count(); ++i) {
        if (QStandardItem* item = model ? model->item(i) : nullptr)
            item->setEnabled(!known || intervalReachable(box->itemData(i).toInt(), m_measuredRate));
    }

    box->setEnabled(known);

    if (!known) {
        // Замера нет — стоит секунда. Её выдаёт любая замеренная конфигурация:
        // худшая из них, широкий код 2000x50, даёт 6,9 обновления в секунду.
        const int index = box->findData(EverySecond);
        if (index != -1)
            box->setCurrentIndex(index);
        m_ui->updateRateHintLBL->setText(
            tr("Список откроется после замера: как часто спектр успевает "
               "обновляться, зависит от кода, вычислителя и сетки"));
        return;
    }

    // Выбранное значение могло стать недостижимым — например, после того как
    // подбор сетки выбрал сетку покрупнее.
    if (!intervalReachable(box->currentData().toInt(), m_measuredRate)) {
        QVector<int> intervals;
        for (int i = 0; i < box->count(); ++i)
            intervals.append(box->itemData(i).toInt());

        const int allowed = fastestAllowed(intervals, m_measuredRate);
        const int index   = allowed > 0 ? box->findData(allowed) : box->count() - 1;
        if (index != -1)
            box->setCurrentIndex(index);
    }

    m_ui->updateRateHintLBL->setText(
        tr("Замерено: быстрее %1 обн/с эта конфигурация не даёт")
            .arg(m_measuredRate, 0, 'f', 1));
}

void SettingsDialog::applyMeasuredRate(double perSecond)
{
    m_measuredRate = perSecond;
    m_measuredFor  = perSecond > 0.0 ? updateRateKey() : QString();

    m_ui->measureRatePBN->setEnabled(true);
    m_ui->measureRatePBN->setText(tr("Замерить"));
    applyUpdateRateLimit();

    if (perSecond <= 0.0)
        m_ui->updateRateHintLBL->setText(
            tr("Замер ничего не поймал: за пробу спектр не ушёл ни разу. "
               "Список остаётся на секунде"));
    saveSettings();
}

void SettingsDialog::handleSettingsRequested()
{
    emit sendSettingsToWidget(m_settings);
}

void SettingsDialog::setInterfaceEnabled( bool enabled )
{
    // Гасятся страницы, а не сам QTabWidget: иначе вместе с ними отключится
    // и полоса вкладок, и во время расчёта нельзя будет даже посмотреть, что
    // выставлено на соседней.
    m_ui->computationTab->setEnabled(enabled);
    m_ui->deviceTab->setEnabled(enabled);

    // Вкладка «Спектр» гаснуть не должна: там частота показа, интервал
    // автосохранения и число столбцов графика — они описывают показ, а не
    // задачу, и меняются на ходу. Всё остальное задаёт сам расчёт: под него
    // выделены буферы и посчитан объём работы, менять его на ходу — это
    // просто перезапустить.
    m_ui->savingTab->setEnabled(true);
    // Пробе нужен свободный вычислитель, а он сейчас занят расчётом.
    m_ui->measureRatePBN->setEnabled(enabled);

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
            m_codeKindBGP->button(SingleCode)->setChecked(true);
            m_singleEnumType = EnumerationType::Full;
            m_fullAlgorithm  = a;
            break;
        case Algorithm::BrouwerZimmermann:
        case Algorithm::RandomInfoSets:
            m_codeKindBGP->button(SingleCode)->setChecked(true);
            m_singleEnumType   = EnumerationType::Partial;
            m_partialAlgorithm = a;
            if (weight > 0)
                weightFor(a) = weight;
            break;
        case Algorithm::ProductCode:
            m_codeKindBGP->button(ProductCode)->setChecked(true);
            m_partialAlgorithm = componentAlgorithm == int(Algorithm::RandomInfoSets)
                                 ? Algorithm::RandomInfoSets : Algorithm::BrouwerZimmermann;
            m_settings.productWeight = qMax(0, weight);
            if (rank > 0)
                m_ui->productRankSPB->setValue(qBound(1, rank, 4));
            break;
        default:
            // Простой XOR из интерфейса убран; запись показывается, а продолжать
            // её нечем — расчёт пойдёт Брауэром–Циммерманом заново.
            m_codeKindBGP->button(SingleCode)->setChecked(true);
            m_singleEnumType   = EnumerationType::Partial;
            m_partialAlgorithm = Algorithm::BrouwerZimmermann;
            break;
    }

    updateComputationControls();
    collectSettings();
    emit sendSettingsToWidget(m_settings);
}

void SettingsDialog::handleMatrixChanged(int rows, int cols) {
    m_matrixRows = rows;
    m_matrixCols = cols;
    // Код Грея перебирает 2^k масок в одном слове — не длиннее 63 строк;
    // дуальный расчёт перебирает проверочную матрицу, у неё n - k строк.
    m_codeLength     = rows > 63        ? Length::Long : Length::Short;
    m_dualCodeLength = cols - rows > 63 ? Length::Long : Length::Short;

    // Слов тяжелее длины кода не бывает.
    if (m_settings.bzWeight > cols)   m_settings.bzWeight   = qMax(1, cols);
    if (m_settings.leonWeight > cols) m_settings.leonWeight = qMax(1, cols);

    // Доступность полного перебора и сам алгоритм могли только что поменяться —
    // приводим вкладку в согласованный вид одним местом, а не в каждой ветке.
    updateComputationControls();
    collectSettings();
    // Сменилась матрица — прежний замер был про другой код.
    applyUpdateRateLimit();

    // Отправляем новые настройки в виджет
    emit sendSettingsToWidget(m_settings);
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
    m_ui->blocksGpuSPB->setMaximum(qMax(200, sm * 32));
    m_ui->threadsGpuSPB->setMaximum(gpu.maxThreadsPerBlock);

    m_ui->blocksGpuLBL->setToolTip(
        tr("У видеокарты %1 мультипроцессоров.\n"
           "Меньше одного блока на мультипроцессор — половина карты простаивает.\n"
           "По замерам разумно брать от %2 блоков.")
            .arg(sm).arg(sm * 3));
    m_ui->threadsGpuLBL->setToolTip(
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
    const bool gpu  = m_computeDeviceBGP->checkedId() == ComputeDevice::Gpu;
    const bool autoTune = gpu && m_ui->autoTuneGridCHB->isChecked();

    m_ui->blocksGpuLBL->setVisible(gpu);
    m_ui->blocksGpuSPB->setVisible(gpu);
    m_ui->threadsGpuLBL->setVisible(gpu);
    m_ui->threadsGpuSPB->setVisible(gpu);
    m_ui->autoTuneGridCHB->setVisible(gpu);

    m_ui->blocksGpuLBL->setEnabled(!autoTune);
    m_ui->blocksGpuSPB->setEnabled(!autoTune);
    m_ui->threadsGpuLBL->setEnabled(!autoTune);
    m_ui->threadsGpuSPB->setEnabled(!autoTune);
}

void SettingsDialog::checkGpuAvailable() {
    if (isGpuAvailable()) {
        m_ui->gpuRB->setEnabled(true);
        applyDeviceLimits();
    }
    else {
        m_ui->gpuRB->setEnabled(false);
        m_ui->cpuRB->setChecked(true);
        updateDeviceControls();
    }
}

// ------------------------------------------------------------ хранение

void SettingsDialog::saveSettings() {
    QSettings s;

    s.beginGroup("lastSettings");  // ← ВАЖНО

    collectSettings();

    QJsonDocument doc(m_settings.toJson());
    s.setValue(SettingsKeys::COMPUTATION_SETTINGS, doc.toJson());
    // Отдельно от JSON: в самих настройках лежит один алгоритм, а помнить
    // надо выбор в обеих парах и тип перебора произвольного кода.
    s.setValue(SettingsKeys::FULL_ALGORITHM,    int(m_fullAlgorithm));
    s.setValue(SettingsKeys::PARTIAL_ALGORITHM, int(m_partialAlgorithm));
    s.setValue(SettingsKeys::SINGLE_ENUM_TYPE,  int(m_singleEnumType));
    // Замер живёт рядом с настройками: он свойство конфигурации, а не сеанса,
    // и переживать перезапуск обязан — иначе список закрывался бы каждый раз.
    s.setValue(SettingsKeys::UPDATE_RATE,     m_measuredRate);
    s.setValue(SettingsKeys::UPDATE_RATE_KEY, m_measuredFor);

    s.endGroup(); // ← не забыть
}

// Переносит значения из полей в m_settings, ничего не записывая на диск.
// Пробе нужны настройки, которые пользователь видит сейчас, а не те, что он
// подтвердил кнопкой.
void SettingsDialog::collectSettings()
{
    const Algorithm algorithm = currentAlgorithm();
    m_settings.algorithmType = algorithm;
    m_settings.enumType = m_enumeratorBGP->checkedId() == EnumerationType::Full
                          ? EnumerationType::Full : EnumerationType::Partial;
    // Число строк простого XOR больше не задаётся: всё, сколько есть.
    m_settings.maxRows = qMax(1, m_matrixRows);
    // Веса хранятся по алгоритму и обновляются по вводу; здесь — на случай,
    // если поле видно и его значение подрезал новый предел.
    if (algorithm == Algorithm::BrouwerZimmermann || algorithm == Algorithm::RandomInfoSets
        || algorithm == Algorithm::ProductCode)
        weightFor(algorithm) = m_ui->weightSPB->value();
    m_settings.productAlgorithm = m_partialAlgorithm;
    m_settings.leonMissExponent = m_ui->leonMissCBX->currentData().toInt();
    m_settings.leonMemoryMb = m_ui->leonMemorySPB->value();
    m_settings.productRank = 1;
    m_settings.compDev = static_cast<ComputeDevice>(m_computeDeviceBGP->checkedId());

    m_settings.compDevSet.threadsCpu = m_ui->threadsCpuSPB->value();
    m_settings.compDevSet.blocksGpu = m_ui->blocksGpuSPB->value();
    m_settings.compDevSet.threadsGpu = m_ui->threadsGpuSPB->value();
    m_settings.autoTuneGrid          = m_ui->autoTuneGridCHB->isChecked();
    m_settings.maxPlotBars           = m_ui->maxPlotBarsSPB->value();

    m_settings.timeIntSet.saveSpectrumInterval = m_ui->saveSpectrumIntervalCBX->currentData().toInt();
    m_settings.timeIntSet.updateSpectrumInterval = m_ui->updateSpectrumIntervalCBX->currentData().toInt();
}

void SettingsDialog::loadSettings() {
    QSettings s;

    s.beginGroup("lastSettings");  // ← ВАЖНО

    QByteArray data = s.value(SettingsKeys::COMPUTATION_SETTINGS).toByteArray();
    if (!data.isEmpty())
    {
        QJsonDocument doc = QJsonDocument::fromJson(data);
        m_settings = ComputationSettings::fromJson(doc.object());
    }

    m_measuredRate = s.value(SettingsKeys::UPDATE_RATE, 0.0).toDouble();
    m_measuredFor  = s.value(SettingsKeys::UPDATE_RATE_KEY).toString();

    // Выбор в парах — из реестра, а сам алгоритм настроек его уточняет: они
    // могли разойтись, если настройки писала другая версия программы.
    m_fullAlgorithm = static_cast<Algorithm>(
        s.value(SettingsKeys::FULL_ALGORITHM, int(Algorithm::GrayCode)).toInt());
    m_partialAlgorithm = static_cast<Algorithm>(
        s.value(SettingsKeys::PARTIAL_ALGORITHM, int(Algorithm::BrouwerZimmermann)).toInt());
    m_singleEnumType = static_cast<EnumerationType>(
        s.value(SettingsKeys::SINGLE_ENUM_TYPE, int(EnumerationType::Full)).toInt());
    if (m_fullAlgorithm != Algorithm::GrayCode && m_fullAlgorithm != Algorithm::DualCode)
        m_fullAlgorithm = Algorithm::GrayCode;
    if (m_partialAlgorithm != Algorithm::BrouwerZimmermann && m_partialAlgorithm != Algorithm::RandomInfoSets)
        m_partialAlgorithm = Algorithm::BrouwerZimmermann;

    switch (m_settings.algorithmType) {
        case Algorithm::GrayCode:
        case Algorithm::DualCode:
            m_codeKindBGP->button(SingleCode)->setChecked(true);
            m_singleEnumType = EnumerationType::Full;
            m_fullAlgorithm  = m_settings.algorithmType;
            break;
        case Algorithm::BrouwerZimmermann:
        case Algorithm::RandomInfoSets:
            m_codeKindBGP->button(SingleCode)->setChecked(true);
            m_singleEnumType   = EnumerationType::Partial;
            m_partialAlgorithm = m_settings.algorithmType;
            break;
        case Algorithm::ProductCode:
            m_codeKindBGP->button(ProductCode)->setChecked(true);
            if (m_settings.productAlgorithm == Algorithm::RandomInfoSets
                || m_settings.productAlgorithm == Algorithm::BrouwerZimmermann)
                m_partialAlgorithm = static_cast<Algorithm>(m_settings.productAlgorithm);
            break;
        default:
            // Простой XOR: в новом интерфейсе его нет — ближайшее по смыслу.
            m_codeKindBGP->button(SingleCode)->setChecked(true);
            m_singleEnumType   = EnumerationType::Partial;
            m_partialAlgorithm = Algorithm::BrouwerZimmermann;
            break;
    }

    {
        const int at = m_ui->leonMissCBX->findData(m_settings.leonMissExponent);
        m_ui->leonMissCBX->setCurrentIndex(at >= 0 ? at : 2);
    }
    m_ui->leonMemorySPB->setValue(m_settings.leonMemoryMb);
    m_ui->productRankSPB->setValue(m_settings.productRank);

    updateComputationControls();

    s.endGroup(); // ← не забыть

    m_computeDeviceBGP->button(m_settings.compDev)->setChecked(true);

    int maxThreads = omp_get_max_threads();
    m_ui->threadsCpuSPB->setMaximum(maxThreads);

    m_ui->threadsCpuSPB->setValue(std::min(maxThreads, m_settings.compDevSet.threadsCpu));
    m_ui->blocksGpuSPB->setValue(m_settings.compDevSet.blocksGpu);
    m_ui->threadsGpuSPB->setValue(m_settings.compDevSet.threadsGpu);
    m_ui->autoTuneGridCHB->setChecked(m_settings.autoTuneGrid);
    m_ui->maxPlotBarsSPB->setValue(m_settings.maxPlotBars);

    updateDeviceControls();

    int index = m_ui->saveSpectrumIntervalCBX->findData(m_settings.timeIntSet.saveSpectrumInterval);
    if (index != -1) {
        m_ui->saveSpectrumIntervalCBX->setCurrentIndex(index);
    }
    index = m_ui->updateSpectrumIntervalCBX->findData(m_settings.timeIntSet.updateSpectrumInterval);
    if (index != -1) {
        m_ui->updateSpectrumIntervalCBX->setCurrentIndex(index);
    }
}
