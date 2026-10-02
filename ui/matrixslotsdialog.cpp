#include "matrixslotsdialog.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int CELL_PX = 64;
}

MatrixSlotsDialog::MatrixSlotsDialog(Mode mode, const QString& matrixText, QWidget* parent)
    : QDialog(parent), m_mode(mode), m_matrixText(matrixText)
{
    setWindowTitle(mode == Mode::Load ? tr("Загрузить матрицу") : tr("Сохранить матрицу"));

    auto* const layout = new QVBoxLayout(this);

    auto* const grid = new QGridLayout;
    grid->setSpacing(4);
    m_cells.resize(MatrixLibrary::SLOTS);
    for (int slot = 0; slot < MatrixLibrary::SLOTS; ++slot) {
        auto* const cell = new QToolButton(this);
        cell->setFixedSize(CELL_PX, CELL_PX);
        // «(1000,997)» в ячейку обычным кеглем не влезает.
        QFont small = cell->font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        cell->setFont(small);
        cell->setCheckable(true);
        cell->setAutoExclusive(true);
        cell->setAutoRaise(false);
        cell->installEventFilter(this);
        cell->setProperty("slot", slot);
        connect(cell, &QToolButton::clicked, this, [this, slot]() { select(slot); });
        connect(cell, &QToolButton::pressed, this, [this, slot]() { select(slot); });
        grid->addWidget(cell, slot / MatrixLibrary::COLS, slot % MatrixLibrary::COLS);
        m_cells[slot] = cell;
    }
    layout->addLayout(grid);

    auto* const nameRow = new QHBoxLayout;
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("Название"));
    // При загрузке поле тоже редактируется: так матрицу можно переименовать,
    // не пересохраняя. Применяется по Enter или уходу фокуса.
    if (mode == Mode::Load)
        connect(m_nameEdit, &QLineEdit::editingFinished, this, &MatrixSlotsDialog::renameSelected);
    m_sizeLabel = new QLabel(this);
    m_sizeLabel->setMinimumWidth(80);
    nameRow->addWidget(m_nameEdit, 1);
    nameRow->addWidget(m_sizeLabel);
    layout->addLayout(nameRow);

    auto* const buttons = new QDialogButtonBox(this);
    m_okButton = buttons->addButton(mode == Mode::Load ? tr("Загрузить") : tr("Сохранить"),
                                  QDialogButtonBox::AcceptRole);
    if (mode == Mode::Load) {
        m_removeButton = buttons->addButton(tr("Удалить"), QDialogButtonBox::DestructiveRole);
        connect(m_removeButton, &QPushButton::clicked, this, &MatrixSlotsDialog::removeSelected);
    }
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &MatrixSlotsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &MatrixSlotsDialog::reject);
    layout->addWidget(buttons);

    m_library.load();
    rebuild();
    // При сохранении подпись по умолчанию — размер матрицы.
    if (mode == Mode::Save)
        m_nameEdit->setText(tr("Матрица %1").arg(MatrixLibrary::dimensions(matrixText)));
}

void MatrixSlotsDialog::rebuild()
{
    for (int slot = 0; slot < MatrixLibrary::SLOTS; ++slot) {
        const MatrixLibrary::Entry entry = m_library.at(slot);
        QToolButton* const cell = m_cells[slot];
        cell->setText(entry.slot < 0 ? QString() : MatrixLibrary::dimensions(entry.matrix));
        cell->setToolTip(entry.name);
        // В режиме загрузки пустые ячейки не выбираются — там нечего брать.
        cell->setEnabled(m_mode == Mode::Save || entry.slot >= 0);
    }
    m_okButton->setEnabled(m_selected >= 0 && (m_mode == Mode::Save || m_library.has(m_selected)));
    if (m_removeButton)
        m_removeButton->setEnabled(m_selected >= 0 && m_library.has(m_selected));
}

bool MatrixSlotsDialog::eventFilter(QObject* watched, QEvent* event)
{
    // Наведение — показать название; ушли — вернуть название выбранной.
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        const QVariant slot = watched->property("slot");
        if (slot.isValid())
            showName(event->type() == QEvent::Enter ? slot.toInt() : m_selected);
    }
    return QDialog::eventFilter(watched, event);
}

void MatrixSlotsDialog::showName(int slot)
{
    // Пока пользователь печатает в поле, наведение его не трогает.
    if (m_nameEdit->hasFocus())
        return;
    if (m_mode == Mode::Save && slot == m_selected) {
        // В поле — то, что пользователь напечатал; не затирать.
        return;
    }
    const MatrixLibrary::Entry entry = slot >= 0 ? m_library.at(slot) : MatrixLibrary::Entry();
    if (m_mode == Mode::Load) {
        m_nameEdit->setText(entry.name);
        m_sizeLabel->setText(entry.slot < 0 ? QString() : MatrixLibrary::dimensions(entry.matrix));
    } else {
        // Подсказка о занятой ячейке — в подписи размера, поле не трогаем.
        m_sizeLabel->setText(entry.slot < 0 ? QString() : tr("занято: %1").arg(entry.name));
    }
}

void MatrixSlotsDialog::select(int slot)
{
    // Щелчок по ячейке — конец правки имени прежней: применить и показать
    // имя новой.
    if (m_nameEdit->hasFocus())
        m_nameEdit->clearFocus();
    m_selected = slot;
    m_cells[slot]->setChecked(true);
    if (m_mode == Mode::Load) {
        showName(slot);
    } else {
        const MatrixLibrary::Entry entry = m_library.at(slot);
        m_sizeLabel->setText(entry.slot < 0 ? QString() : tr("занято: %1").arg(entry.name));
    }
    m_okButton->setEnabled(m_mode == Mode::Save || m_library.has(slot));
    if (m_removeButton)
        m_removeButton->setEnabled(m_library.has(slot));
}

void MatrixSlotsDialog::renameSelected()
{
    if (m_mode != Mode::Load || m_selected < 0)
        return;
    m_library.load();
    const MatrixLibrary::Entry entry = m_library.at(m_selected);
    const QString name = m_nameEdit->text().trimmed();
    if (entry.slot < 0 || name.isEmpty() || name == entry.name)
        return;
    m_library.save(m_selected, name, entry.matrix);
    m_cells[m_selected]->setToolTip(name);
}

void MatrixSlotsDialog::accept()
{
    if (m_selected < 0)
        return;
    if (m_mode == Mode::Load)
        renameSelected();
    m_library.load();
    if (m_mode == Mode::Load) {
        const MatrixLibrary::Entry entry = m_library.at(m_selected);
        if (entry.slot < 0)
            return;
        m_chosen = entry.matrix;
        QDialog::accept();
        return;
    }
    if (m_library.has(m_selected)) {
        const MatrixLibrary::Entry old = m_library.at(m_selected);
        const auto answer = QMessageBox::question(
            this, tr("Сохранить матрицу"),
            tr("В этой ячейке уже лежит «%1» %2. Заменить?").arg(old.name, MatrixLibrary::dimensions(old.matrix)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    const QString name = m_nameEdit->text().trimmed();
    m_library.save(m_selected, name.isEmpty() ? tr("Матрица %1").arg(MatrixLibrary::dimensions(m_matrixText)) : name,
                 m_matrixText);
    QDialog::accept();
}

void MatrixSlotsDialog::removeSelected()
{
    if (m_selected < 0 || !m_library.has(m_selected))
        return;
    const MatrixLibrary::Entry entry = m_library.at(m_selected);
    const auto answer = QMessageBox::question(
        this, tr("Удалить матрицу"),
        tr("Удалить «%1» %2?").arg(entry.name, MatrixLibrary::dimensions(entry.matrix)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    m_library.load();
    m_library.remove(m_selected);
    rebuild();
    showName(m_selected);
}
