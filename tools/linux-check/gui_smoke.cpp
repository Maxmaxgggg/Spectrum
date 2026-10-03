// Дымовой тест окна без экрана: расчёт Хэмминга (7,4) через кнопку «Старт»,
// текст спектра на экране, сохранение и подъём спектра из настроек.
#include "mainwindow.h"
#include "cachingstyle.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <cstdio>
#include "settingsdialog.h"
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QJsonDocument>
#include "constants.h"
#include <QRadioButton>
#include <QSpinBox>

// Предупреждения Qt о вызовах по имени и незарегистрированных типах —
// провал: такие ошибки видны только во время работы.
static QStringList g_warnings;
static void handler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    if (msg.contains("invokeMethod") || msg.contains("unregistered") || msg.contains("No such method")
        || msg.contains("QObject::connect"))
        g_warnings << msg;
    if (type == QtFatalMsg) abort();
}

static QString waitText(QPlainTextEdit* edit, const QString& expected, int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (edit->toPlainText() == expected) break;
    }
    return edit->toPlainText();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qInstallMessageHandler(handler);
    QApplication a(argc, argv);
    a.setStyle(new CachingStyle);
    a.setApplicationName("SpectrumSmoke");
    a.setOrganizationName("Alpas");
    QSettings().clear();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    const QString expected = QStringLiteral("0 - 1\n3 - 7\n4 - 7\n7 - 1");
    int failed = 0;
    {
        MainWindow w;
        w.show();
        auto* matrix   = w.findChild<QPlainTextEdit*>("matrixPTE");
        auto* spectrum = w.findChild<QPlainTextEdit*>("spectrumPTE");
        auto* run      = w.findChild<QPushButton*>("executePBN");
        if (!matrix || !spectrum || !run) { std::printf("нет виджетов\n"); return 2; }
        matrix->setPlainText("1101000\n0110100\n0011010\n0001101");
        QApplication::processEvents();
        run->click();
        const QString got = waitText(spectrum, expected, 20000);
        const bool ok = got == expected;
        failed += !ok;
        std::printf("%s расчёт из окна: «%s»\n", ok ? "ok    " : "ПРОВАЛ", qPrintable(QString(got).replace('\n', "; ")));
        // Дождаться, пока окно получит finished и отпустит поток.
        QElapsedTimer t; t.start();
        while (t.elapsed() < 2000) QApplication::processEvents(QEventLoop::AllEvents, 50);

        // Замер потолка обновления: настройки уходят воркеру вызовом по имени.
        auto* dialog  = w.findChild<SettingsDialog*>();
        auto* measure = dialog ? dialog->findChild<QPushButton*>("measureRatePBN") : nullptr;
        if (!measure) { std::printf("нет кнопки замера\n"); return 2; }
        measure->setEnabled(true);
        measure->click();
        t.restart();
        while (t.elapsed() < 4000) QApplication::processEvents(QEventLoop::AllEvents, 50);
        const bool probed = measure->text() != QStringLiteral("Замеряю…");
        failed += !probed;
        std::printf("%s замер потолка обновления: кнопка «%s»\n", probed ? "ok    " : "ПРОВАЛ",
                    qPrintable(measure->text()));
    }   // деструктор окна сохраняет настройки
    {
        MainWindow w;
        auto* spectrum = w.findChild<QPlainTextEdit*>("spectrumPTE");
        const bool ok = spectrum->toPlainText() == expected;
        failed += !ok;
        std::printf("%s спектр из настроек: «%s»\n", ok ? "ok    " : "ПРОВАЛ",
                    qPrintable(spectrum->toPlainText().replace('\n', "; ")));
    }
    {
        // Структура «БЧХ» из диалога: частичный перебор Брауэра–Циммермана по
        // сдвигам, Хэмминг (7,4) циклический — спектр до веса 4.
        MainWindow w;
        w.show();
        auto* dialog   = w.findChild<SettingsDialog*>();
        auto* matrix   = w.findChild<QPlainTextEdit*>("matrixPTE");
        auto* spectrum = w.findChild<QPlainTextEdit*>("spectrumPTE");
        auto* run      = w.findChild<QPushButton*>("executePBN");
        auto* bch      = dialog->findChild<QRadioButton*>("bchCodeRBN");
        auto* partial  = dialog->findChild<QRadioButton*>("partialEnumRBN");
        auto* bz       = dialog->findChild<QRadioButton*>("brouwerZimmermannRBN");
        auto* weight   = dialog->findChild<QSpinBox*>("weightSPB");
        auto* box      = dialog->findChild<QDialogButtonBox*>("buttonBox");
        if (!bch || !partial || !bz || !weight || !box) { std::printf("нет полей структуры\n"); return 2; }
        matrix->setPlainText("1101000\n0110100\n0011010\n0001101");
        QApplication::processEvents();
        bch->click(); partial->click(); bz->click();
        weight->setValue(4);
        box->button(QDialogButtonBox::Save)->click();
        QApplication::processEvents();
        spectrum->clear();
        run->click();
        const QString want = QStringLiteral("0 - 1\n3 - 7\n4 - 7");
        const QString got = waitText(spectrum, want, 20000);
        const bool ok = got == want;
        failed += !ok;
        std::printf("%s «БЧХ» + Брауэр–Циммерман из окна: «%s»\n", ok ? "ok    " : "ПРОВАЛ", qPrintable(QString(got).replace('\n', "; ")));
        QElapsedTimer t; t.start();
        while (t.elapsed() < 2000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    {
        MainWindow w;
        auto* dialog = w.findChild<SettingsDialog*>();
        auto* bch    = dialog->findChild<QRadioButton*>("bchCodeRBN");
        const bool ok = bch && bch->isChecked();
        failed += !ok;
        std::printf("%s структура «БЧХ» поднята из настроек\n", ok ? "ok    " : "ПРОВАЛ");
    }
    {
        // Прежняя версия держала код-произведение двумя матрицами: в поле
        // должна оказаться матрица произведения, в заголовке — компоненты.
        QSettings s;
        s.setValue(SettingsKeys::CODE_MATRIX,  QStringLiteral("1000101\n0100111\n0010110\n0001011"));
        s.setValue(SettingsKeys::CODE_MATRIX2, QStringLiteral("100111\n010110\n001011"));
        s.beginGroup("lastSettings");
        s.setValue(SettingsKeys::COMPUTATION_SETTINGS, QByteArray("{\"algorithmType\": 5}"));
        s.endGroup();
        s.sync();
    }
    {
        MainWindow w;
        w.show();
        auto* matrix   = w.findChild<QPlainTextEdit*>("matrixPTE");
        auto* spectrum = w.findChild<QPlainTextEdit*>("spectrumPTE");
        auto* run      = w.findChild<QPushButton*>("executePBN");
        auto* dock     = w.findChild<QDockWidget*>("matrixDock");
        const QString kron = QStringLiteral(
            "100111000000000000000000100111000000100111\n010110000000000000000000010110000000010110\n"
            "001011000000000000000000001011000000001011\n000000100111000000000000100111100111100111\n"
            "000000010110000000000000010110010110010110\n000000001011000000000000001011001011001011\n"
            "000000000000100111000000100111100111000000\n000000000000010110000000010110010110000000\n"
            "000000000000001011000000001011001011000000\n000000000000000000100111000000100111100111\n"
            "000000000000000000010110000000010110010110\n000000000000000000001011000000001011001011");
        const bool migrated = matrix->toPlainText() == kron && !QSettings().contains(SettingsKeys::CODE_MATRIX2);
        failed += !migrated;
        std::printf("%s две матрицы прежней версии → матрица произведения\n", migrated ? "ok    " : "ПРОВАЛ");
        const bool titled = dock && dock->windowTitle() == QStringLiteral("Код-произведение (42,12) = (7,4) ⊗ (6,3)");
        failed += !titled;
        std::printf("%s заголовок панели: «%s»\n", titled ? "ok    " : "ПРОВАЛ", dock ? qPrintable(dock->windowTitle()) : "нет");

        spectrum->clear();
        run->click();
        // Ранг 1 до границы Толхёйзена 14: 9 — 7·4, 12 — 7·3 + 7·4.
        const QString want = QStringLiteral("0 - 1\n9 - 28\n12 - 49");
        const QString got = waitText(spectrum, want, 20000);
        const bool ok = got == want;
        failed += !ok;
        std::printf("%s код-произведение из окна: «%s»\n", ok ? "ok    " : "ПРОВАЛ", qPrintable(QString(got).replace('\n', "; ")));
        QElapsedTimer t; t.start();
        while (t.elapsed() < 2000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    failed += !g_warnings.isEmpty();
    std::printf("%s предупреждений Qt о вызовах и типах: %d%s\n", g_warnings.isEmpty() ? "ok    " : "ПРОВАЛ",
                int(g_warnings.size()), g_warnings.isEmpty() ? "" : qPrintable(" — " + g_warnings.join(" | ")));
    std::printf("сохранено в настройках: %s\n", qPrintable(QSettings().value("spectrumText").toString().replace('\n', "; ")));
    return failed;
}
