#pragma once

#define MAX_K                63
#define CONST_MEM_SIZE       ( 1 << 16 )
#define WORD_SIZE            sizeof(quint64)
#define BINOM_TABLE_SIZE     ( ( MAX_K + 1 ) * ( MAX_K + 1 ) )
#define MAX_CONST_WORDS      ( CONST_MEM_SIZE/WORD_SIZE - BINOM_TABLE_SIZE )
#define MAX_BLOCKWORDS       16




#define PROGRESSBAR_MS         "refreshProgressbarMs"
#define SPECTRUM_MS            "refreshSpectrumMs"
#define SPECTRUM_COLOR         "spectrumColor"
#define WIDGET_GEOMETRY        "geometry"
#define SETTINGS_GEOMETRY      "settingsGeometry"
#define SPLITTER_STATE         "splitterState"
#define CODE_MATRIX            "matrix"
#define SPECTRUM_TEXT          "spectrumText"
#define SPECTRUM_VALUES        "spectrumValues"
#define TRANSPARENCY_VALUE     "transparencyValue"
#define STRING_VALUE           "stringValue"
#define USE_GPU_CHECKED        "useGpuChecked"
#define USE_GRAY_CODE_CHECKED  "useGrayCodeChecked"


#define PAUSE_TEXT			   QStringLiteral("Пауза")
#define CONTINUE_TEXT		   QStringLiteral("Продолжить")
#define START_TEXT			   QStringLiteral("Старт")
#define READY_TEXT			   QStringLiteral("Готово")
#define MAIN_TITLE			   QStringLiteral("Расчет спектра кодовых слов")
#define ERROR_TITLE			   QStringLiteral("Ошибка")
#define WARNING_TITLE          QStringLiteral("Предупреждение")
