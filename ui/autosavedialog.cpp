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

// Полное число операций расчёта — по нему считается процент готовности.
// Через double: точность здесь не нужна, а биномы для кода длиной под тысячу
// не помещаются никуда, кроме как в приблизительное число.
double totalOperations(const AutosaveEntry& entry)
{
    const int rows = entry.rows;

    if (entry.record.algorithm != ComputationSettings::SimpleXor)
        return std::pow(2.0, double(rows));

    const int maxRows = entry.record.maxRows > 0 ? qMin(entry.record.maxRows, rows) : rows;

    double total = 0.0;
    double term  = 1.0;               // C(rows, 0)
    for (int r = 0; r <= maxRows; ++r) {
        total += term;
        term = term * double(rows - r) / double(r + 1);
    }
    return total;
}

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
    , store(store)
{
    setWindowTitle(tr("Сохранения"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(760, 380);

    tree = new QTreeWidget(this);
    tree->setColumnCount(5);
    tree->setHeaderLabels({ tr("Код"), tr("Алгоритм"), tr("Состояние"),
                            tr("Сохранено"), tr("Размер") });
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setSortingEnabled(true);
    tree->sortByColumn(3, Qt::DescendingOrder);

    summary = new QLabel(this);

    loadBtn      = new QPushButton(tr("Загрузить матрицу"), this);
    removeBtn    = new QPushButton(tr("Удалить"), this);
    removeAllBtn = new QPushButton(tr("Удалить всё"), this);
    QPushButton* const folderBtn = new QPushButton(tr("Открыть папку"), this);
    QPushButton* const closeBtn  = new QPushButton(tr("Закрыть"), this);

    loadBtn->setToolTip(tr("Вернуть матрицу этой записи в редактор,\n"
                           "чтобы продолжить расчёт с неё."));

    QHBoxLayout* const buttons = new QHBoxLayout;
    buttons->addWidget(loadBtn);
    buttons->addWidget(removeBtn);
    buttons->addWidget(removeAllBtn);
    buttons->addStretch();
    buttons->addWidget(folderBtn);
    buttons->addWidget(closeBtn);

    QVBoxLayout* const layout = new QVBoxLayout(this);
    layout->addWidget(tree);
    layout->addWidget(summary);
    layout->addLayout(buttons);

    connect(tree, &QTreeWidget::itemSelectionChanged, this, &AutosaveDialog::updateButtons);
    connect(tree, &QTreeWidget::itemDoubleClicked, this, &AutosaveDialog::loadMatrixOfSelected);
    connect(loadBtn,      &QPushButton::clicked, this, &AutosaveDialog::loadMatrixOfSelected);
    connect(removeBtn,    &QPushButton::clicked, this, &AutosaveDialog::removeSelected);
    connect(removeAllBtn, &QPushButton::clicked, this, &AutosaveDialog::removeAll);
    connect(folderBtn,    &QPushButton::clicked, this, &AutosaveDialog::openFolder);
    connect(closeBtn,     &QPushButton::clicked, this, &QDialog::accept);

    refresh();
}

QString AutosaveDialog::describeAlgorithm(const AutosaveRecord& record)
{
    switch (record.algorithm) {
        case ComputationSettings::GrayCode: return tr("код Грея");
        case ComputationSettings::DualCode: return tr("дуальный код");
        default:                            return tr("простой XOR");
    }
}

QString AutosaveDialog::describeProgress(const AutosaveEntry& entry)
{
    const AutosaveRecord& record = entry.record;

    if (record.finished) {
        // У кода Грея слоёв нет, число строк в описании было бы бессмысленно.
        if (record.algorithm != ComputationSettings::SimpleXor)
            return tr("готово");
        return tr("готово до %1 строк").arg(record.maxRows);
    }

    const double total = totalOperations(entry);
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
    tree->clear();

    const QVector<AutosaveEntry> entries = store->list();
    qint64 bytes = 0;

    for (const AutosaveEntry& entry : entries) {
        bytes += entry.bytes;

        QTreeWidgetItem* const item = new QTreeWidgetItem(tree);
        item->setText(0, tr("(%1,%2)").arg(entry.cols).arg(entry.rows));
        item->setText(1, describeAlgorithm(entry.record));
        item->setText(2, describeProgress(entry));
        item->setText(3, entry.record.savedAt.isValid()
                             ? QLocale().toString(entry.record.savedAt, QLocale::ShortFormat)
                             : QString());
        item->setText(4, formatBytes(entry.bytes));
        item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);

        item->setData(0, FOLDER_ROLE,    entry.folder);
        item->setData(0, ALGORITHM_ROLE, int(entry.record.algorithm));
        // Дата сортируется по значению, а не по тексту: иначе «01.09» встанет
        // раньше «31.08».
        item->setData(3, Qt::UserRole + 2, entry.record.savedAt);
    }

    for (int i = 0; i < tree->columnCount(); ++i)
        tree->resizeColumnToContents(i);

    // Размер папки на диске больше суммы записей: там же лежат матрицы.
    qint64 onDisk = 0;
    QDirIterator it(store->rootPath(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        onDisk += it.fileInfo().size();
    }

    summary->setText(entries.isEmpty()
        ? tr("Сохранений нет.")
        : tr("Записей: %1, на диске %2 (вместе с матрицами).")
              .arg(entries.size()).arg(formatBytes(onDisk)));

    updateButtons();
}

void AutosaveDialog::updateButtons()
{
    const bool any      = tree->topLevelItemCount() > 0;
    const bool selected = !tree->selectedItems().isEmpty();

    removeBtn->setEnabled(selected);
    loadBtn->setEnabled(tree->selectedItems().size() == 1);
    removeAllBtn->setEnabled(any);
}

void AutosaveDialog::removeSelected()
{
    const QList<QTreeWidgetItem*> selected = tree->selectedItems();
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
        store->removeRecord(folder, algorithm);
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

    store->removeAll();
    refresh();
}

void AutosaveDialog::loadMatrixOfSelected()
{
    const QList<QTreeWidgetItem*> selected = tree->selectedItems();
    if (selected.size() != 1)
        return;

    const Matrix matrix = store->matrixOf(selected.first()->data(0, FOLDER_ROLE).toString());
    if (matrix.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось прочитать матрицу записи"));
        return;
    }

    emit matrixRequested(matrix);
    accept();
}

void AutosaveDialog::openFolder()
{
    QDir().mkpath(store->rootPath());
    QDesktopServices::openUrl(QUrl::fromLocalFile(store->rootPath()));
}
