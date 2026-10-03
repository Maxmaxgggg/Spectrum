#include "autosavedialog.h"

#include "format.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace {

QString formatBytes(qint64 bytes)
{
    if (bytes >= 1024 * 1024)
        return QObject::tr("%1 МБ").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
    if (bytes >= 1024)
        return QObject::tr("%1 КБ").arg(bytes / 1024);
    return QObject::tr("%1 Б").arg(bytes);
}

// Роль, в которой в строке лежит имя папки и алгоритм: по ним потом удаляем.
constexpr int FOLDER_ROLE    = Qt::UserRole;
constexpr int ALGORITHM_ROLE = Qt::UserRole + 1;

} // namespace

AutosaveDialog::AutosaveDialog(AutosaveStore* store, QWidget* parent)
    : QDialog(parent)
    , m_store(store)
{
    setWindowTitle(tr("Сохранения"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(760, 380);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({ tr("Код"), tr("Алгоритм"), tr("Состояние"),
                            tr("Сохранено"), tr("Размер") });
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(3, Qt::DescendingOrder);

    m_summary = new QLabel(this);

    m_loadButton      = new QPushButton(tr("Загрузить матрицу"), this);
    m_removeButton    = new QPushButton(tr("Удалить"), this);
    m_removeAllButton = new QPushButton(tr("Удалить всё"), this);
    QPushButton* const folderBtn = new QPushButton(tr("Открыть папку"), this);
    QPushButton* const closeBtn  = new QPushButton(tr("Закрыть"), this);

    m_loadButton->setToolTip(tr("Вернуть матрицу этой записи в редактор,\n"
                                "чтобы продолжить расчёт с неё."));

    QHBoxLayout* const buttons = new QHBoxLayout;
    buttons->addWidget(m_loadButton);
    buttons->addWidget(m_removeButton);
    buttons->addWidget(m_removeAllButton);
    buttons->addStretch();
    buttons->addWidget(folderBtn);
    buttons->addWidget(closeBtn);

    QVBoxLayout* const layout = new QVBoxLayout(this);
    layout->addWidget(m_tree);
    layout->addWidget(m_summary);
    layout->addLayout(buttons);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &AutosaveDialog::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &AutosaveDialog::loadMatrixOfSelected);
    connect(m_loadButton,      &QPushButton::clicked, this, &AutosaveDialog::loadMatrixOfSelected);
    connect(m_removeButton,    &QPushButton::clicked, this, &AutosaveDialog::removeSelected);
    connect(m_removeAllButton, &QPushButton::clicked, this, &AutosaveDialog::removeAll);
    connect(folderBtn,    &QPushButton::clicked, this, &AutosaveDialog::openFolder);
    connect(closeBtn,     &QPushButton::clicked, this, &QDialog::accept);

    refresh();
}

QString AutosaveDialog::describeAlgorithm(const AutosaveRecord& record)
{
    // Структура «БЧХ» — частичный перебор шёл по циклическим сдвигам.
    const QString bch = record.cyclic ? tr(", БЧХ") : QString();
    switch (record.algorithm) {
        case ComputationSettings::GrayCode:          return tr("код Грея");
        case ComputationSettings::DualCode:          return tr("дуальный код");
        case ComputationSettings::BrouwerZimmermann: return tr("Брауэр–Циммерман, до веса %1%2")
                                                                .arg(record.bzWeight).arg(bch);
        case ComputationSettings::RandomInfoSets:    return tr("стохастический, до веса %1, в-ть пропуска %2%3")
                                                                .arg(record.leonWeight)
                                                                .arg(Format::powerOfTen(-record.leonMissExponent))
                                                                .arg(bch);
        case ComputationSettings::ProductCode:       return tr("код-произведение");
        default:                                     return tr("простой XOR");
    }
}

QString AutosaveDialog::describeProgress(const AutosaveEntry& entry)
{
    const AutosaveRecord& record = entry.record;

    if (record.finished) {
        // У кода Грея слоёв нет, число строк в описании было бы бессмысленно.
        if (record.algorithm == ComputationSettings::GrayCode
            || record.algorithm == ComputationSettings::DualCode)
            return tr("готово");
        if (record.algorithm == ComputationSettings::RandomInfoSets)
            return tr("готово, попыток %1").arg(Format::count(record.leonTrials));
        if (record.algorithm == ComputationSettings::ProductCode)
            return record.productMissExponent > 0
                       ? tr("готово, до веса %1 (в-ть пропуска %2)").arg(record.productExactUpTo)
                             .arg(Format::powerOfTen(-record.productMissExponent))
                       : tr("готово, точно до веса %1").arg(record.productExactUpTo);
        return tr("готово до %1 строк").arg(record.maxRows);
    }

    const double total = totalOperations(entry.record, entry.rows);
    if (total <= 0.0)
        return tr("%1 слов").arg(Format::count(record.state.doneOps));

    const double percent = 100.0 * double(record.state.doneOps) / total;

    // На больших кодах готовность — тысячные доли процента, и «0.0 %» не
    // говорит ничего. Там полезнее само число перебранных слов.
    if (percent < 0.05)
        return tr("%1 слов").arg(Format::count(record.state.doneOps));

    return tr("%1 %").arg(percent, 0, 'f', percent < 10.0 ? 1 : 0);
}

void AutosaveDialog::refresh()
{
    m_tree->clear();

    const QVector<AutosaveEntry> entries = m_store->list();
    qint64 bytes = 0;

    for (const AutosaveEntry& entry : entries) {
        bytes += entry.bytes;

        QTreeWidgetItem* const item = new QTreeWidgetItem(m_tree);
        item->setText(0, tr("(%1,%2)").arg(entry.cols).arg(entry.rows));
        item->setText(1, describeAlgorithm(entry.record));
        item->setText(2, describeProgress(entry));
        item->setText(3, entry.record.savedAt.isValid()
                             ? QLocale().toString(entry.record.savedAt, QLocale::ShortFormat)
                             : QString());
        item->setText(4, formatBytes(entry.bytes));

        item->setData(0, FOLDER_ROLE,    entry.folder);
        item->setData(0, ALGORITHM_ROLE, int(entry.record.algorithm));
        // Дата сортируется по значению, а не по тексту: иначе «01.09» встанет
        // раньше «31.08».
        item->setData(3, Qt::UserRole + 2, entry.record.savedAt);
    }

    for (int i = 0; i < m_tree->columnCount(); ++i)
        m_tree->resizeColumnToContents(i);

    // Размер папки на диске больше суммы записей: там же лежат матрицы.
    qint64 onDisk = 0;
    QDirIterator it(m_store->rootPath(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        onDisk += it.fileInfo().size();
    }

    m_summary->setText(entries.isEmpty()
        ? tr("Сохранений нет.")
        : tr("Записей: %1, на диске %2 (вместе с матрицами).")
              .arg(entries.size()).arg(formatBytes(onDisk)));

    updateButtons();
}

void AutosaveDialog::updateButtons()
{
    const bool any      = m_tree->topLevelItemCount() > 0;
    const bool selected = !m_tree->selectedItems().isEmpty();

    m_removeButton->setEnabled(selected);
    m_loadButton->setEnabled(m_tree->selectedItems().size() == 1);
    m_removeAllButton->setEnabled(any);
}

void AutosaveDialog::removeSelected()
{
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    if (selected.isEmpty())
        return;

    if (QMessageBox::question(this, tr("Удаление"),
            tr("Удалить выбранные записи (%1)? Расчёт придётся начинать заново.")
                .arg(selected.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    for (QTreeWidgetItem* item : selected) {
        const QString folder = item->data(0, FOLDER_ROLE).toString();
        const auto algorithm = static_cast<ComputationSettings::Algorithm>(
            item->data(0, ALGORITHM_ROLE).toInt());
        m_store->removeRecord(folder, algorithm);
    }
    refresh();
}

void AutosaveDialog::removeAll()
{
    if (QMessageBox::question(this, tr("Удаление"),
            tr("Удалить все сохранения? Все незаконченные расчёты придётся\n"
               "начинать заново."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    m_store->removeAll();
    refresh();
}

void AutosaveDialog::loadMatrixOfSelected()
{
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    if (selected.size() != 1)
        return;

    const Matrix matrix = m_store->matrixOf(selected.first()->data(0, FOLDER_ROLE).toString());
    if (matrix.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось прочитать матрицу записи"));
        return;
    }

    // Вместе с матрицей уходит и сама запись: окно поднимет по ней спектр,
    // прогресс и настройки расчёта, чтобы можно было сразу продолжить.
    emit entryChosen(matrix, entryOf(selected.first()));
    accept();
}

AutosaveRecord AutosaveDialog::entryOf(QTreeWidgetItem* item) const
{
    const QString folder = item->data(0, FOLDER_ROLE).toString();
    const auto algorithm = static_cast<ComputationSettings::Algorithm>(
        item->data(0, ALGORITHM_ROLE).toInt());

    for (const AutosaveEntry& entry : m_store->list())
        if (entry.folder == folder && entry.record.algorithm == algorithm)
            return entry.record;

    return AutosaveRecord();
}

void AutosaveDialog::openFolder()
{
    QDir().mkpath(m_store->rootPath());
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_store->rootPath()));
}
