#include "widget.h"
#include <QTranslator>
#include <QApplication>
#include <QLibraryInfo>
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QTranslator qTranslator;
    QString localePath = "C:/msys64/mingw64/qt5-static/share/qt5/translations";
    bool ok = qTranslator.load(QLocale(QLocale::Russian), "qt","_", localePath);
    if(ok){
        a.installTranslator(&qTranslator);
    }
    a.setApplicationName("Spectrum");
    a.setOrganizationName("Alpas");
    Widget w;
    w.show();

    return a.exec();
}
