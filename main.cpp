#include "cachingstyle.h"
#include "widget.h"

int main(int argc, char *argv[])
{
  QApplication a(argc, argv);
  QTranslator qTranslator;
  if (qTranslator.load(":/translation/qt_ru.qm")) {
      a.installTranslator(&qTranslator);
  }
  // Стиль с памятью на значки: без него каждая переразметка дока заново
  // строит значки его кнопок, а это в статической сборке тянет разбор
  // метаданных всех вкомпилированных плагинов. Подробности в cachingstyle.h.
  a.setStyle(new CachingStyle);

  a.setApplicationName("Spectrum");
  a.setOrganizationName("Alpas");
  MainWindow w;
  w.show();
  return a.exec();
}
