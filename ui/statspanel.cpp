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
const QString kDash = QStringLiteral("—");

QString algorithmName(ComputationSettings::Algorithm algorithm)
{
    switch (algorithm) {
    case ComputationSettings::GrayCode:          return QObject::tr("Код Грея");
    case ComputationSettings::DualCode:          return QObject::tr("Дуальный код");
    case ComputationSettings::BrouwerZimmermann: return QObject::tr("Брауэр–Циммерман");
    case ComputationSettings::RandomInfoSets:    return QObject::tr("Стохастический");
    case ComputationSettings::ProductCode:       return QObject::tr("Код произведения");
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

    stateValue     = addRow(form, tr("Состояние:"));
    separator();
    deviceValue    = addRow(form, tr("Устройство:"));
    algorithmValue = addRow(form, tr("Алгоритм:"));
    // Показатель степени в вероятности пропуска — надстрочным индексом.
    algorithmValue->setTextFormat(Qt::RichText);
    separator();
    elapsedValue   = addRow(form, tr("Прошло:"));
    remainingValue = addRow(form, tr("Осталось:"));
    finishValue    = addRow(form, tr("Закончится:"));
    separator();
    speedValue     = addRow(form, tr("Скорость:"));
    doneValue      = addRow(form, tr("Перебрано:"));

    clearProgress();
    deviceValue->setText(kDash);
    algorithmValue->setText(kDash);
}

QLabel* StatsPanel::addRow(QFormLayout* form, const QString& caption)
{
    auto* const value = new QLabel(kDash, this);
    // Моноширинный: цифры бегут каждую секунду, и в пропорциональном шрифте
    // они дёргаются по ширине на каждом обновлении.
    value->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(new QLabel(caption, this), value);
    return value;
}

void StatsPanel::showTask(const ComputationSettings& settings)
{
    grid.clear();

    if (settings.compDev == ComputationSettings::GPU) {
        // У случайного поиска своя сетка: блок на попытку, 256 нитей.
        deviceText = settings.algorithmType == ComputationSettings::RandomInfoSets
                         ? tr("GPU, блок на попытку")
                         : tr("GPU, сетка %1 x %2")
                               .arg(settings.compDevSet.blocksGpu)
                               .arg(settings.compDevSet.threadsGpu);
    } else {
        deviceText = tr("CPU, потоков: %1").arg(settings.compDevSet.threadsCpu);
    }
    deviceValue->setText(deviceText);

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
                          ? tr("компоненты весом до %1, ранги до %2").arg(settings.productWeight).arg(settings.productRank)
                          : tr("ранги до %1").arg(settings.productRank);
        if (settings.productRank >= 2)
            enumeration += tr(", в-ть пропуска %1").arg(Format::powerOfTen(-settings.leonMissExponent, true));
    }
    algorithmValue->setText(algorithmName(settings.algorithmType)
                            + QStringLiteral(", ") + enumeration);
}

void StatsPanel::showGrid(int blocks, int threads)
{
    grid = tr("GPU, сетка %1 x %2 (подобрана)").arg(blocks).arg(threads);
    deviceValue->setText(grid);
}

void StatsPanel::showProgress(int elapsedSec, int minutesLeft, double speed,
                              quint64 doneOps, quint64 totalOps)
{
    elapsedValue->setText(Format::duration(elapsedSec));
    remainingValue->setText(Format::remainingTime(minutesLeft));

    // Время окончания полезнее остатка, когда расчёт на часы: сразу видно,
    // ждать ли его сегодня. За горизонтом оценки показывать нечего — она
    // построена на средней скорости и там уже ничего не значит.
    constexpr int minutesInYear = 365 * 24 * 60;
    if (minutesLeft > 0 && minutesLeft < minutesInYear) {
        const QDateTime finish = QDateTime::currentDateTime().addSecs(qint64(minutesLeft) * 60);
        const bool today = finish.date() == QDate::currentDate();
        finishValue->setText(today ? finish.toString(QStringLiteral("HH:mm"))
                                   : finish.toString(QStringLiteral("d MMMM, HH:mm")));
    } else {
        finishValue->setText(kDash);
    }

    speedValue->setText(Format::speed(speed));
    doneValue->setText(tr("%1 из %2").arg(Format::count(doneOps), Format::count(totalOps)));
}

void StatsPanel::showState(const QString& text)
{
    stateValue->setText(text.isEmpty() ? kDash : text);
}

void StatsPanel::clearProgress()
{
    for (QLabel* value : { elapsedValue, remainingValue, finishValue, speedValue, doneValue })
        value->setText(kDash);
}
