#ifndef FILTERPLAINTEXTEDIT_H
#define FILTERPLAINTEXTEDIT_H

#include <QPlainTextEdit>
#include <QRegularExpression>
class FilterPlainTextEdit : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit FilterPlainTextEdit(QWidget *parent = nullptr);
protected:
    // Перехват вставки из буфера (Ctrl+V, drag&drop и т.п.)
    void insertFromMimeData(const QMimeData *source) override;
    // Перехват ввода с клавиатуры
    void keyPressEvent(QKeyEvent *e) override;
private:
    const QRegularExpression reInvalid{QStringLiteral("[^01\n]")};
};

#endif // FILTERPLAINTEXTEDIT_H
