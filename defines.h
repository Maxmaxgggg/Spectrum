#pragma once


namespace Constants
{
    constexpr  int CONST_MEM_SIZE = 1 << 16;							// Размер константной памяти видеокарты
    constexpr  int WORD_SIZE = 8;								// Размер слова (под словом понимается тип данных quint64)
    // Потолок числа информационных множеств у Брауэра–Циммермана. Больше
    // непересекающихся множеств, чем n/k, не бывает, а маски всех множеств
    // лежат в константной памяти видеокарты: 16 x 32 слова — четыре килобайта.
    constexpr  int MAX_INFO_SETS = 16;
    constexpr  int MASKS_CONST_WORDS = MAX_INFO_SETS * 32;			// Слова константной памяти под маски множеств
    // Максимальный размер матрицы в константной памяти: вся память минус маски
    constexpr  int MAX_CONST_WORDS = CONST_MEM_SIZE / WORD_SIZE - MASKS_CONST_WORDS;
    // Через сколько после последнего изменения размера перерисовывать график.
    // Меньше — рисуется прямо во время перетаскивания панели; заметно больше —
    // виден пустой прямоугольник после того, как отпустили.
    constexpr  int PLOT_REFRESH_DELAY_MS = 60;

    // Номер раскладки доков. Менять при добавлении или удалении панели:
    // сохранённая раскладка старого набора тогда просто не подхватится.
    constexpr  int LAYOUT_VERSION = 4;   // 4: вторая матрица вкладкой рядом с первой

    constexpr  int MAX_POSITIONS = 33;
    // Потолок кандидатов автоподбора: под столько нитей длинному пути надо
    // заранее заготовить стартовые маски.
    constexpr  int MAX_TUNE_BLOCKS  = 8 * 64;   // 8 блоков на мультипроцессор
    constexpr  int MAX_TUNE_THREADS = 512;
    constexpr  int MAX_BLOCKWORDS = 32; 								// Максимальное число 64-битных слов, используемых для хранения одной строки
    constexpr  int MAX_MASK_WORDS = 32;								// Максимальная длина битовой маски
    constexpr  int MAX_ROWS = 2048;
    constexpr  int MAX_COLS = 2048;
    constexpr  int MAX_SHORT_CODE_LENGTH = 63;
    // Сколько разделяемой памяти на блок готовы занять под гистограмму и копию
    // матрицы. Предел устройства 48 КБ, немного оставляем про запас.
    constexpr  int MAX_SHARED_BYTES = 40 * 1024;
    constexpr  int BINOM_TABLE_SIZE_FOR_SHORT_CODES = ( MAX_SHORT_CODE_LENGTH + 1 ) * ( MAX_SHORT_CODE_LENGTH + 1 );
    constexpr  int ERROR_OCCURED = -1;
}

namespace Constants
{
    // Сколько крутится проба потолка обновления. Полутора секунд хватает и
    // самой медленной из замеренных конфигураций: широкий код 2000x50 отдаёт
    // спектр 6,9 раза в секунду, то есть около десяти отправок за пробу.
    constexpr int PROBE_DURATION_MS = 1500;
}

namespace SettingsKeys
{
    constexpr const char WIDGET_GEOMETRY[]          = "geometry";
    // Раскладка доков: что пришвартовано, что вытащено, что закрыто.
    // Пришло на смену splitterState — сплиттер убран вместе с жёсткой
    // компоновкой.
    constexpr const char WINDOW_STATE[]             = "windowState";
    constexpr const char COMPUTATION_SETTINGS[]     = "computationSettings";
    constexpr const char CODE_MATRIX[]              = "matrix";
    constexpr const char CODE_MATRIX2[]             = "matrix2";
    constexpr const char SPECTRUM_TEXT[]            = "spectrumText";
    constexpr const char SPECTRUM_VALUES[]          = "spectrumValues";
    constexpr const char MATRICES_JSON[]            = "matricesJson";
    // Тип перебора, выбранный для простого XOR. Хранится отдельно от
    // computationSettings: там при коде Грея лежит принудительный «Полный».
    constexpr const char XOR_ENUM_TYPE[]            = "xorEnumType";   // больше не пишется
    // Выбор в парах алгоритмов и тип перебора произвольного кода: в самих
    // настройках лежит один алгоритм, а помнить надо все три.
    constexpr const char FULL_ALGORITHM[]           = "fullAlgorithm";
    constexpr const char PARTIAL_ALGORITHM[]        = "partialAlgorithm";
    constexpr const char SINGLE_ENUM_TYPE[]         = "singleEnumType";
    // Вписанное число строк для частичного перебора — по той же причине:
    // при полном там лежит число строк матрицы.
    constexpr const char XOR_MAX_ROWS[]             = "xorMaxRows";
    // Замеренный потолок обновления спектра, отправок в секунду, и описание
    // конфигурации, на которой он получен. Не совпало — замер не годится:
    // потолок зависит от кода, вычислителя и сетки запуска.
    constexpr const char UPDATE_RATE[]              = "updateRatePerSecond";
    constexpr const char UPDATE_RATE_KEY[]          = "updateRateFor";
}

namespace DefaultValues
{
    constexpr int  SPECTRUM_COLOR       = 0;
}


namespace UIStrings
{
    // Вторая компонента кода произведения.
    constexpr const char MATRIX2_TOOLTIP[]          = "Порождающая матрица второй компоненты кода произведения; первая — в панели «Матрица»";

    constexpr const char PAUSE_TEXT[]               = "Пауза";
    constexpr const char CONTINUE_TEXT[]            = "Продолжить";
    constexpr const char START_TEXT[]               = "Старт";
    constexpr const char MAIN_TITLE[]               = "Расчет спектра кодовых слов";
    constexpr const char CANCEL_TEXT[]              = "Отменено пользователем";
    constexpr const char ERROR_TITLE[]              = "Ошибка";
    constexpr const char WARNING_TITLE[]            = "Предупреждение";
    constexpr const char READY_TEXT[]               = "Готово. Расчёт занял ";
    constexpr const char GPU_NOT_FOUND_TEXT[]       = "GPU не найдено. Расчеты будут производиться на CPU";


    constexpr const char SAVE_LBL_BASE_TOOLTIP[]    = "В момент сохранения спектра тут появится значок";
    constexpr const char SAVE_LBL_ICON_TOOLTIP[]    = "Сейчас сохраняется спектр";
    constexpr const char PAUSE_TOOLTIP[]            = "Приостановка расчета спектра";
    constexpr const char START_TOOLTIP[]            = "Запуск расчета спектра. Блокирует некоторые настройки";
    constexpr const char CONTINUE_TOOLTIP[]         = "Продолжение расчета спектра";
    constexpr const char CANCEL_TOOLTIP[]           = "Остановка расчета спектра";
    constexpr const char EXIT_TOOLTIP[]             = "Выход из приложения";
    constexpr const char SETTINGS_TOOLTIP[]         = "Настройки приложения";
    constexpr const char SPECTRUM_TOOLTIP[]         = "Спектр кода: сколько кодовых слов приходится на каждый вес";
    constexpr const char PLOT_TOOLTIP[]             = "Тот же спектр столбчатой диаграммой";
    constexpr const char VIEW_RESET_TEXT[]          = "Раскладка по умолчанию";
    constexpr const char MATRIX_TOOLTIP[]           = "Порождающая матрица кода";
    constexpr const char STATS_TOOLTIP[]            = "Чем считаем, сколько идёт и сколько сделано";

}


