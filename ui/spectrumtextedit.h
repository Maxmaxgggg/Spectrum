#pragma once

#include <QPlainTextEdit>

// Поле спектра: числа показываются с разбивкой по три цифры, а копируются
// без неё.
//
// Разделитель — узкий неразрывный пробел (U+202F): в моноширинном шрифте он
// уже обычного, и группы читаются как одно число. При копировании
// выделенного (Ctrl+C, перетаскивание) он вырезается, так что в буфер
// уходят обычные цифры — их можно вставить куда угодно.
class SpectrumTextEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit SpectrumTextEdit(QWidget* parent = nullptr);

    // Разделитель групп цифр.
    static QChar separator();
    // «40654224» → «40 654 224» (с узкими пробелами).
    static QString grouped(const QString& digits);

protected:
    QMimeData* createMimeDataFromSelection() const override;
};
