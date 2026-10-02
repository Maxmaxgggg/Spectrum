# Имена в коде

Одни правила на весь проект, кроме сторонней библиотеки `qcustomplot.*`.
Имена проверяет clang-tidy по `.clang-tidy` в корне: в Visual Studio —
«Свойства проекта → Анализ кода → Clang-Tidy», из командной строки —
`clang-tidy -p <каталог с compile_commands.json> файл.cpp`. Сейчас код
проходит проверку без единого замечания.

## Файлы

- Нижний регистр без разделителей; имя — главный класс или пространство
  имён файла: `mainwindow.h` — `MainWindow`, `autosavestore.h` —
  `AutosaveStore`, `combinations.h` — `Combinations`, `hamming.h` — `Hamming`.
- Реализация одного класса, разнесённая по нескольким файлам, —
  `<класс>_<часть>.cpp`: `worker_cpu.cpp`, `worker_gpu.cpp`. Закрытый
  заголовок класса — `<класс>_p.h`, как в самом Qt: `worker_p.h`.
- Ядра CUDA — `.cu` и `.cuh`, имя — что считает ядро: `spectrumkernel.cu`,
  `leonkernel.cu`.
- Файлы проекта называются по проекту: `Spectrum.vcxproj`, `Spectrum.rc`,
  `Spectrum.qrc`.
- Заголовки защищены от повторного включения через `#pragma once`.

## Типы

- Классы, структуры, перечисления, псевдонимы типов и пространства имён —
  PascalCase: `ComputationSettings`, `LeonLaunch`, `BitOps`.
- Значения перечислений — тоже PascalCase: `GrayCode`, `Full`, `Cpu`.
- Аббревиатура пишется как слово: `GpuInfo`, `UiStrings`, `JsonU64`, `Cpu` —
  не `GPUInfo`, `UIStrings`, `CPU`.
- Параметры шаблонов: типы — PascalCase (`T`, `Visit`), значения —
  UPPER_SNAKE_CASE (`WORDS`, `GLOBAL`).

## Функции

- camelCase, с глагола: `computeSpectrum`, `saveCheckpoint`,
  `unrankPositions`.
- `set…` — только сеттеры. Одноразовая сборка части окна — `setup…`:
  `setupDocks`, `setupWorker`.
- Пути расчёта называются по алгоритму, устройству и, где вариантов два,
  длине кода: `computeXorGpuShort`, `computeGrayCpu`, `computeLeon`. Чанк —
  `xorChunkCpuLong`, ядро — `xorKernelShort`, запуск ядра — `launchXorShort`.

## Сигналы и слоты

- Сигнал называет событие: `finished`, `spectrumUpdated`, `progressChanged`,
  `planReady`, `settingsRequested`. Не приказ (`setInterfaceEnabled`) и не
  адресат (`updateInfoPBR`).
- Слот, отвечающий на сигнал, — `handle` и имя сигнала:
  `handleSpectrumUpdated`, `handleExecuteClicked`.
- Автосвязывания по имени `on_<объект>_<сигнал>` нет — только явный
  `connect`: автосвязь молча отваливается, стоит переименовать объект в
  форме.

## Переменные

- camelCase: `wordsPerRow`, `chunkOffset`.
- Закрытые и защищённые поля классов — `m_` и camelCase: `m_settings`,
  `m_binomTable`. Открытые поля структур — без приставки:
  `CodeGeometry::rows`, `LeonLaunch::window`.
- Глобальные переменные (они есть только в тестах) — `g_` и camelCase:
  `g_passed`, `g_out`.
- Память CUDA помечается приставкой: `d_` — на устройстве, `h_` — её копия
  на хосте, `s_` — разделяемая, `c_` — константная: `d_spectrum`,
  `h_matrix`, `s_matrix`, `c_masks`.
- Однобуквенные имена — только строчные математические обозначения: `n` —
  длина кода (столбцы матрицы), `k` — размерность (строки), `r` — сколько
  строк складывается (слой перебора), `w` — вес, `d` — расстояние, `p`, `l` —
  параметры Штерна.
- Размеры матрицы там, где с ней работают как с массивом, — `rows` и
  `cols`; в формулах и в ядрах — `n` и `k`.

## Константы и макросы

- Константы времени компиляции — `constexpr`, константы на уровне
  пространства имён, `static const`-таблицы — UPPER_SNAKE_CASE: `MAX_ROWS`,
  `LEON_THREADS`, `PRIMITIVE_POLYNOMIALS`.
- Макросы — UPPER_SNAKE_CASE: `CUDA_CALL`, `SPECTRUM_HD`.
- Локальные `const`-переменные — как обычные переменные:
  `const int words = …`.

## Формы Qt Designer

Виджет формы называется по смыслу, в конце — трёхбуквенный код типа:

| Код | Тип                |
|-----|--------------------|
| ACN | `QAction`          |
| BGP | `QButtonGroup`     |
| CBX | `QComboBox`        |
| CHB | `QCheckBox`        |
| CPT | `QCustomPlot`      |
| GBX | `QGroupBox`        |
| LBL | `QLabel`           |
| MNU | `QMenu`            |
| PBN | `QPushButton`      |
| PBR | `QProgressBar`     |
| PTE | `QPlainTextEdit`   |
| RBN | `QRadioButton`     |
| SPB | `QSpinBox`         |

- Так же называются программные двойники виджетов формы в `MainWindow` и
  `SettingsDialog`: `m_matrix2PTE`, `m_codeKindBGP`.
- Виджеты, которые остальные классы создают сами, — смысл и слово:
  `m_okButton`, `m_nameEdit`, `m_tabBar`.
- Компоновки, распорки и страницы вкладок кода не получают:
  `computationTabLayout`, `plotSpacer`, `deviceTab`. Контейнеры, которые
  Qt Designer создаёт сам, сохраняют его имена: `centralwidget`, `menubar`,
  `statusbar`, `buttonBox`.

## Что при переименованиях не трогать

Эти строки лежат на диске у пользователя или вызываются по имени —
переименуешь, и старые данные не прочитаются или вызов молча не дойдёт:

- ключи QSettings (`SettingsKeys`), ключи JSON настроек и автосохранений,
  имена файлов автосохранения;
- `objectName` доков: по ним Qt восстанавливает раскладку окна;
- слоты, которые вызываются строкой через `QMetaObject::invokeMethod`:
  `setSettings`, `computeSpectrum`, `measureUpdateRate`,
  `initializeRunState`.
