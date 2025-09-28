#pragma once

#define MAX_K                64
#define CONST_MEM_SIZE       65'536
#define WORD_SIZE            sizeof(quint64)
#define BINOM_TABLE_SIZE     ( ( MAX_K + 1 ) * ( MAX_K + 1 ) )
#define MAX_CONST_WORDS      ( CONST_MEM_SIZE/WORD_SIZE - BINOM_TABLE_SIZE )
#define MAX_BLOCKWORDS       16




#define PROGRESSBAR_MS      "refreshProgressbarMs"
#define PLOT_MS             "refreshPlotMs"
#define PTE_MS              "refreshPteMs"
#define SPECTRUM_COLOR      "spectrumColor"
#define WIDGET_GEOMETRY     "geometry"
#define SETTINGS_GEOMETRY   "settingsGeometry"
#define SPLITTER_STATE      "splitterState"
#define CODE_MATRIX         "matrix"
#define SPECTRUM_TEXT       "spectrumText"
#define SPECTRUM_VALUES     "spectrumValues"
#define TRANSPARENCY_VALUE  "transparencyValue"
#define STRING_VALUE        "stringValue"
#define D_THREAD_CHECKED    "distinctThreadChecked"


#define PAUSE_TEXT			QStringLiteral("Пауза")
#define CONTINUE_TEXT		QStringLiteral("Продолжить")
#define START_TEXT			QStringLiteral("Старт")
#define READY_TEXT			QStringLiteral("Готово")
#define MAIN_TITLE			QStringLiteral("Расчет спектра кодовых слов")
