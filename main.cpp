#include "widget.h"
#include "workwithmatrix.h"
#include "bitmask.h"

int main(int argc, char *argv[])
{
  QApplication a(argc, argv);
  QTranslator qTranslator;
  if (qTranslator.load(":/translation/qt_ru.qm")) {
      a.installTranslator(&qTranslator);
  }
  a.setApplicationName("Spectrum");
  a.setOrganizationName("Alpas");
  MainWindow w;
  w.show();
  return a.exec();
}
