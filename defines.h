#pragma once

#define MAX_K                63
#define CONST_MEM_SIZE       ( 1 << 16 )
#define WORD_SIZE            sizeof(quint64)
#define BINOM_TABLE_SIZE     ( ( MAX_K + 1 ) * ( MAX_K + 1 ) )
#define MAX_CONST_WORDS      ( CONST_MEM_SIZE/WORD_SIZE - BINOM_TABLE_SIZE )
#define MAX_BLOCKWORDS       16




#define PROGRESSBAR_MS              "refreshProgressbarMs"
#define SPECTRUM_MS                 "refreshSpectrumMs"
#define SPECTRUM_COLOR              "spectrumColor"
#define WIDGET_GEOMETRY             "geometry"
#define SETTINGS_GEOMETRY           "settingsGeometry"
#define SPLITTER_STATE              "splitterState"
#define CODE_MATRIX                 "matrix"
#define SPECTRUM_TEXT               "spectrumText"
#define SPECTRUM_VALUES             "spectrumValues"
#define TRANSPARENCY_VALUE          "transparencyValue"
#define STRINGS_VALUE               "stringsValue"
#define STRINGS_MAX_VALUE           "stringsMaxValue"
#define USE_GPU_CHECKED             "useGpuChecked"
#define USE_GRAY_CODE_CHECKED       "useGrayCodeChecked"

#define DEFAULT_PROGRESSBAR_MS      100
#define DEFAULT_TRANSPARENCY_VALUE  50
#define DEFAULT_SPECTRUM_MS         500
#define DEFAULT_STRINGS_VALUE       1
#define DEFAULT_STRINGS_MAX_VALUE   1
#define DEFAULT_SPECTRUM_COLOR      0
#define DEFAULT_USE_GPU             true
#define DEFAULT_USE_GRAY_CODE       false




#define PAUSE_TEXT			   QStringLiteral("Пауза")
#define CONTINUE_TEXT		   QStringLiteral("Продолжить")
#define START_TEXT			   QStringLiteral("Старт")
#define READY_TEXT			   QStringLiteral("Готово")
#define MAIN_TITLE			   QStringLiteral("Расчет спектра кодовых слов")
#define ERROR_TITLE			   QStringLiteral("Ошибка")
#define WARNING_TITLE          QStringLiteral("Предупреждение")
