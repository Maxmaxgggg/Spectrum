#pragma once

namespace Constants
{
    constexpr  int CONST_MEM_SIZE = 1 << 16;							// Размер константной памяти видеокарты
    constexpr  int WORD_SIZE = 8;								// Размер слова (под словом понимается тип данных quint64)
    constexpr  int MAX_CONST_WORDS = CONST_MEM_SIZE / WORD_SIZE;		// Максимальный размер массива в константной памяти видеокарты
    constexpr  int MAX_POSITIONS = 33;
    constexpr  int MAX_BLOCKWORDS = 32; 								// Максимальное число 64-битных слов, используемых для хранения одной строки
    constexpr  int MAX_MASK_WORDS = 32;								// Максимальная длина битовой маски
    constexpr  int MAX_ROWS = 2048;
    constexpr  int MAX_COLS = 2048;
    constexpr  int MAX_SHORT_CODE_LENGTH = 63;
    constexpr  int BINOM_TABLE_SIZE_FOR_SHORT_CODES = ( MAX_SHORT_CODE_LENGTH + 1 ) * ( MAX_SHORT_CODE_LENGTH + 1 );
}

namespace SettingsKeys
{
    constexpr const char PROGRESSBAR_MS[]           = "refreshProgressbarMs";
    constexpr const char SPECTRUM_MS[]              = "refreshSpectrumMs";
    constexpr const char SPECTRUM_COLOR[]           = "spectrumColor";
    constexpr const char WIDGET_GEOMETRY[]          = "geometry";
    constexpr const char SETTINGS_GEOMETRY[]        = "settingsGeometry";
    constexpr const char SPLITTER_STATE[]           = "splitterState";
    constexpr const char COMPUTATION_SETTINGS[]     = "computationSettings";
    constexpr const char CODE_MATRIX[]              = "matrix";
    constexpr const char SPECTRUM_TEXT[]            = "spectrumText";
    constexpr const char SPECTRUM_VALUES[]          = "spectrumValues";
    constexpr const char TRANSPARENCY_VALUE[]       = "transparencyValue";
    constexpr const char MATRICES_JSON[]            = "matricesJson";
}

namespace DefaultValues
{
    constexpr int  PROGRESSBAR_MS       = 100;
    constexpr int  TRANSPARENCY_VALUE   = 50;
    constexpr int  SPECTRUM_MS          = 500;
    constexpr int  STRINGS_VALUE        = 1;
    constexpr int  STRINGS_MAX_VALUE    = 1;
    constexpr int  SPECTRUM_COLOR       = 0;

    constexpr bool USE_GPU              = true;
    constexpr bool USE_GRAY_CODE        = false;
    constexpr bool USE_DUAL_CODE        = false;
}


namespace UIStrings
{
    constexpr const char PAUSE_TEXT[]           = "Пауза";
    constexpr const char CONTINUE_TEXT[]        = "Продолжить";
    constexpr const char START_TEXT[]           = "Старт";
    constexpr const char MAIN_TITLE[]           = "Расчет спектра кодовых слов";
    constexpr const char CANCEL_TEXT[]          = "Отменено пользователем";
    constexpr const char ERROR_TITLE[]          = "Ошибка";
    constexpr const char WARNING_TITLE[]        = "Предупреждение";
    constexpr const char GPU_NOT_FOUND_TEXT[]   = "GPU не найдено. Расчеты будут производиться на CPU";

    constexpr const char PAUSE_TOOLTIP[]        = "Приостановка расчета спектра";
    constexpr const char START_TOOLTIP[]        = "Запуск расчета спектра. Блокирует некоторые настройки";
    constexpr const char CONTINUE_TOOLTIP[]     = "Продолжение расчета спектра";
    constexpr const char CANCEL_TOOLTIP[]       = "Остановка расчета спектра";
    constexpr const char EXIT_TOOLTIP[]         = "Выход из приложения";
    constexpr const char SETTINGS_TOOLTIP[]     = "Настройки приложения";
    constexpr const char SPECTRUM_TOOLTIP[]     = "Текстовое и графическое представление спектра кода";
    constexpr const char MATRIX_TOOLTIP[]       = "Порождающая матрица кода";
}


