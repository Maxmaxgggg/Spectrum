#include "matrixmenu.h"

#include <QAction>
#include <QCursor>
#include <QIcon>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>
#include <QWidget>

namespace {

// Задержка перед раскрытием подменю «Удалить» и допуск на дрожание курсора.
constexpr int  HOVER_DELAY_MS  = 120;
constexpr int  HOVER_MARGIN_PX = 6;

const char ICON_MATRIX[] = ":/ui/icons/newspaper.png";
const char ICON_ADD[]    = ":/ui/icons/newspaper--plus.png";
const char ICON_DELETE[] = ":/ui/icons/newspaper--minus.png";

} // namespace

MatrixMenu::MatrixMenu(QMenu* menu, QMenu* deleteMenu, QAction* addAction, QWidget* dialogParent)
    : QObject(dialogParent)
    , menu(menu)
    , deleteMenu(deleteMenu)
    , addAction(addAction)
    , dialogParent(dialogParent)
{
    Q_ASSERT(menu && deleteMenu && addAction);

    addAction->setIcon(QIcon(ICON_ADD));
    deleteMenu->setIcon(QIcon(ICON_DELETE));
    connect(addAction, &QAction::triggered, this, &MatrixMenu::addCurrentMatrix);

    menu->setMouseTracking(true);

    hoverTimer = new QTimer(this);
    hoverTimer->setSingleShot(true);
    hoverTimer->setInterval(HOVER_DELAY_MS);

    connect(menu, &QMenu::hovered, this, [this](QAction* action) {
        pendingHover = action;
        if (pendingHover && pendingHover->menu() && actionsEnabled)
            hoverTimer->start();
        else
            hoverTimer->stop();
    });

    connect(hoverTimer, &QTimer::timeout, this, [this]() {
        if (!pendingHover || !pendingHover->menu())
            return;

        const QPoint local = this->menu->mapFromGlobal(QCursor::pos());
        if (this->menu->actionAt(local) == pendingHover) {
            this->menu->setActiveAction(pendingHover);
            return;
        }

        // Курсор мог соскользнуть на пару пикселей за границу пункта, пока
        // тикал таймер — это ещё не повод не раскрывать подменю.
        const QRect rect = this->menu->actionGeometry(pendingHover);
        if (rect.isNull())
            return;

        const QRect expanded = rect.adjusted(-HOVER_MARGIN_PX, -HOVER_MARGIN_PX,
                                              HOVER_MARGIN_PX,  HOVER_MARGIN_PX);
        if (expanded.contains(local))
            this->menu->setActiveAction(pendingHover);
    });

    // Список матриц могли поменять из другого экземпляра программы, поэтому
    // пересобираем его перед каждым показом, а не один раз при запуске.
    connect(menu, &QMenu::aboutToShow, this, &MatrixMenu::rebuild);

    library.load();
    updateDeleteMenuState();
}

void MatrixMenu::setMatrixSource(std::function<QString()> source)
{
    matrixSource = std::move(source);
}

QAction* MatrixMenu::deleteMenuAction() const
{
    return deleteMenu->menuAction();
}

void MatrixMenu::updateDeleteMenuState()
{
    deleteMenu->setEnabled(actionsEnabled && !deleteMenu->actions().isEmpty());
}

void MatrixMenu::rebuild()
{
    library.load();
    const QStringList names = library.names();

    // Всё, кроме двух статических пунктов, — динамика прошлого показа.
    const QList<QAction*> existing = menu->actions();
    for (QAction* action : existing) {
        if (action == addAction || action == deleteMenuAction())
            continue;
        menu->removeAction(action);
    }
    deleteMenu->clear();

    if (names.isEmpty()) {
        deleteMenu->setEnabled(false);
        return;
    }

    menu->addSeparator();

    for (const QString& name : names) {
        QAction* load = new QAction(name, menu);
        load->setData(name);
        load->setIcon(QIcon(ICON_MATRIX));
        load->setEnabled(actionsEnabled);
        connect(load, &QAction::triggered, this, [this, name]() {
            emit matrixChosen(library.matrix(name));
        });
        menu->addAction(load);
    }

    for (const QString& name : names) {
        QAction* remove = new QAction(name, deleteMenu);
        remove->setIcon(QIcon(ICON_MATRIX));
        remove->setEnabled(actionsEnabled);
        connect(remove, &QAction::triggered, this, [this, name, remove]() {
            removeMatrix(name, remove);
        });
        deleteMenu->addAction(remove);
    }

    updateDeleteMenuState();
}

void MatrixMenu::removeMatrix(const QString& name, QAction* deleteAction)
{
    if (!library.remove(name)) {
        QMessageBox::warning(dialogParent, tr("Ошибка"),
                             tr("Не удалось удалить матрицу \"%1\"").arg(name));
        return;
    }

    // Меню открыто прямо сейчас, поэтому пункты убираются по одному, а не
    // полной пересборкой: пересобирать видимое меню под курсором нельзя.
    deleteMenu->removeAction(deleteAction);
    deleteAction->deleteLater();

    for (QAction* action : menu->actions()) {
        if (action == addAction || action == deleteMenuAction())
            continue;
        if (action->data().toString() == name || action->text() == name) {
            menu->removeAction(action);
            action->deleteLater();
            break;
        }
    }

    updateDeleteMenuState();
}

QString MatrixMenu::suggestedName() const
{
    if (!matrixSource)
        return QString();

    const QStringList rows = matrixSource().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (rows.isEmpty())
        return QStringLiteral("(0,0)");

    // Столбцов столько, сколько символов в самой длинной строке: разделителей
    // в формате матрицы нет, каждый символ — отдельный бит.
    int cols = 0;
    for (const QString& row : rows)
        cols = qMax(cols, row.length());

    return tr("Матрица (%1,%2)").arg(cols).arg(rows.size());
}

void MatrixMenu::addCurrentMatrix()
{
    if (!matrixSource)
        return;

    // Диалог собирается вручную, чтобы убрать кнопку «?» в заголовке.
    QInputDialog dialog(dialogParent);
    dialog.setWindowTitle(tr("Сохранить матрицу"));
    dialog.setLabelText(tr("Имя матрицы:"));
    dialog.setTextValue(suggestedName());
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString name = dialog.textValue().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(dialogParent, tr("Ошибка"), tr("Имя не может быть пустым"));
        return;
    }

    library.load();

    if (library.contains(name)) {
        const auto answer = QMessageBox::question(dialogParent, tr("Перезапись"),
            tr("Матрица с именем \"%1\" уже существует. Перезаписать?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    library.save(name, matrixSource());
    updateDeleteMenuState();
}

void MatrixMenu::setActionsEnabled(bool enabled)
{
    actionsEnabled = enabled;

    for (QAction* action : menu->actions()) {
        if (action == addAction || action == deleteMenuAction())
            continue;
        action->setEnabled(enabled);
    }

    for (QAction* action : deleteMenu->actions())
        action->setEnabled(enabled);
    updateDeleteMenuState();

    if (!menu->isVisible())
        return;

    // Меню открыто в момент запуска расчёта: проще закрыть, чем показывать
    // список, половина которого только что стала неактивной.
    if (!enabled)
        menu->close();
    else
        rebuild();
}
