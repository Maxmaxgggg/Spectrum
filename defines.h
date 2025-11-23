#pragma once

#define MAX_K						63
#define MAX_BINOM_COEF				34
#define CONST_MEM_SIZE				( 1 << 16 )
#define WORD_SIZE					sizeof(quint64)
#define BINOM_TABLE_SIZE			( ( MAX_K + 1 ) * ( MAX_K + 1 ) )
#define MAX_CONST_WORDS				( CONST_MEM_SIZE/WORD_SIZE - BINOM_TABLE_SIZE )
#define MAX_BLOCKWORDS				16


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
#define USE_DUAL_CODE_CHECKED       "useDualCodeChecked"
#define MATRICES_JSON               "matricesJson"


#define DEFAULT_PROGRESSBAR_MS      100
#define DEFAULT_TRANSPARENCY_VALUE  50
#define DEFAULT_SPECTRUM_MS         500
#define DEFAULT_STRINGS_VALUE       1
#define DEFAULT_STRINGS_MAX_VALUE   1
#define DEFAULT_SPECTRUM_COLOR      0
#define DEFAULT_USE_GPU             true
#define DEFAULT_USE_GRAY_CODE       false
#define DEFAULT_USE_DUAL_CODE       false


#define PAUSE_TEXT					QStringLiteral("Пауза")
#define CONTINUE_TEXT				QStringLiteral("Продолжить")
#define START_TEXT					QStringLiteral("Старт")
#define MAIN_TITLE					QStringLiteral("Расчет спектра кодовых слов")
#define CANCEL_TEXT					QStringLiteral("Отменено пользователем")
#define ERROR_TITLE					QStringLiteral("Ошибка")
#define WARNING_TITLE				QStringLiteral("Предупреждение")
#define GPU_NOT_FOUND_TEXT			QStringLiteral("GPU не найдено. Расчеты будут производиться на CPU")

#define PAUSE_TOOLTIP				QStringLiteral("Приостановка расчета спектра")
#define START_TOOLTIP				QStringLiteral("Запуск расчета спектра. Блокирует некоторые настройки")
#define CONTINUE_TOOLTIP			QStringLiteral("Продолжение расчета спектра")
#define CANCEL_TOOLTIP				QStringLiteral("Остановка расчета спектра")
#define EXIT_TOOLTIP				QStringLiteral("Выход из приложения")
#define SETTINGS_TOOLTIP			QStringLiteral("Настройки приложения")
#define SPECTRUM_TOOLTIP			QStringLiteral("Текстовое и графическое представление спектра кода")
#define MATRIX_TOOLTIP				QStringLiteral("Порождающая матрица кода. При нажатии ПКМ можно сохранять/загружать матрицы")