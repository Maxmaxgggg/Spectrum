#include "statspanel.h"

#include "format.h"

#include <QDateTime>

#include <cmath>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>

namespace {

// Прочерк, а не пустота: пустое поле читается как «ещё не посчитали», а
// прочерк — как «здесь нечему быть».
const QString DASH = QStringLiteral("—");

QString algorithmName(ComputationSettings::Algorithm algorithm)
{
    switch (algorithm) {
    case ComputationSettings::GrayCode:          return QObject::tr("Код Грея");
    case ComputationSettings::DualCode:          return QObject::tr("Дуальный код");
    case ComputationSettings::BrouwerZimmermann: return QObject::tr("Брауэр–Циммерман");
    case ComputationSettings::RandomInfoSets:    return QObject::tr("Стохастический");
    case ComputationSettings::ProductCode:       return QObject::tr("Код-произведение");
    default:                                     return QObject::tr("Простой XOR");
    }
}

} // namespace

StatsPanel::StatsPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* const form = new QFormLayout(this);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto separator = [form]() {
        auto* const line = new QFrame;
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        form->addRow(line);
    };

    m_stateValue     = addRow(form, tr("Состояние:"));
    separator();
    m_deviceValue    = addRow(form, tr("Устройство:"));
    m_algorithmValue = addRow(form, tr("Алгоритм:"));
    // Показатель степени в вероятности пропуска — надстрочным индексом.
    m_algorithmValue->setTextFormat(Qt::RichText);
    separator();
    m_elapsedValue   = addRow(form, tr("Прошло:"));
    m_remainingValue = addRow(form, tr("Осталось:"));
    m_finishValue    = addRow(form, tr("Закончится:"));
    separator();
    m_speedValue     = addRow(form, tr("Скорость:"));
    m_doneValue      = addRow(form, tr("Перебрано:"));

    clearProgress();
    m_deviceValue->setText(DASH);
    m_algorithmValue->setText(DASH);
}

QLabel* StatsPanel::addRow(QFormLayout* form, const QString& caption)
{
    auto* const value = new QLabel(DASH, this);
    // Моноширинный: цифры бегут каждую секунду, и в пропорциональном шрифте
    // они дёргаются по ширине на каждом обновлении.
    value->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(new QLabel(caption, this), value);
    return value;
}

void StatsPanel::showTask(const ComputationSettings& settings)
{
    m_grid.clear();

    if (settings.compDev == ComputationSettings::Gpu) {
        // У случайного поиска своя сетка: блок на попытку, 256 нитей.
        m_deviceText = settings.algorithmType == ComputationSettings::RandomInfoSets
                         ? tr("GPU, блок на попытку")
                         : tr("GPU, сетка %1 x %2")
                               .arg(settings.compDevSet.blocksGpu)
                               .arg(settings.compDevSet.threadsGpu);
    } else {
        m_deviceText = tr("CPU, потоков: %1").arg(settings.compDevSet.threadsCpu);
    }
    m_deviceValue->setText(m_deviceText);

    QString enumeration = settings.enumType == ComputationSettings::Full
                              ? tr("полный перебор")
                              : tr("частичный, до %1 строк").arg(settings.maxRows);
    if (settings.algorithmType == ComputationSettings::BrouwerZimmermann)
        enumeration = tr("компоненты весом до %1").arg(settings.bzWeight);
    if (settings.algorithmType == ComputationSettings::RandomInfoSets)
        enumeration = tr("компоненты весом до %1, в-ть пропуска %2")
                          .arg(settings.leonWeight).arg(Format::powerOfTen(-settings.leonMissExponent, true));
    if (settings.algorithmType == ComputationSettings::ProductCode) {
        enumeration = settings.productWeight > 0
                          ? tr("компоненты весом до %1").arg(settings.productWeight)
                          : tr("до границы Толхёйзена");
    }
    m_algorithmValue->setText(algorithmName(settings.algorithmType)
                            + QStringLiteral(", ") + enumeration);
}

void StatsPanel::showGrid(int blocks, int threads)
{
    m_grid = tr("GPU, сетка %1 x %2 (подобрана)").arg(blocks).arg(threads);
    m_deviceValue->setText(m_grid);
}

void StatsPanel::showProgress(int elapsedSec, int minutesLeft, double speed,
                              quint64 doneOps, quint64 totalOps)
{
    m_elapsedValue->setText(Format::duration(elapsedSec));
    m_remainingValue->setText(Format::remainingTime(minutesLeft));

    // Время окончания полезнее остатка, когда расчёт на часы: сразу видно,
    // ждать ли его сегодня. За горизонтом оценки показывать нечего — она
    // построена на средней скорости и там уже ничего не значит.
    constexpr int MINUTES_IN_YEAR = 365 * 24 * 60;
    if (minutesLeft > 0 && minutesLeft < MINUTES_IN_YEAR) {
        const QDateTime finish = QDateTime::currentDateTime().addSecs(qint64(minutesLeft) * 60);
        const bool today = finish.date() == QDate::currentDate();
        m_finishValue->setText(today ? finish.toString(QStringLiteral("HH:mm"))
                                   : finish.toString(QStringLiteral("d MMMM, HH:mm")));
    } else {
        m_finishValue->setText(DASH);
    }

    m_speedValue->setText(Format::speed(speed));
    m_doneValue->setText(tr("%1 из %2").arg(Format::count(doneOps), Format::count(totalOps)));
}

void StatsPanel::showState(const QString& text)
{
    m_stateValue->setText(text.isEmpty() ? DASH : text);
}

void StatsPanel::clearProgress()
{
    for (QLabel* value : { m_elapsedValue, m_remainingValue, m_finishValue, m_speedValue, m_doneValue })
        value->setText(DASH);
}
