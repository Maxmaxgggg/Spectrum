#include "gridtuner.h"

#include "spectrumkernel.cuh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Блоков берём кратно числу мультипроцессоров: сетка меньше одного блока на
// мультипроцессор оставляет часть карты простаивать, а слишком дробная
// добавляет только накладные расходы на планирование.
const int BLOCKS_PER_SM[] = { 1, 2, 3, 4, 6, 8 };

// 1024 нити не рассматриваются намеренно: варианты ядра на 32 слова занимают
// под сотню регистров на нить, и такой блок просто не запускается. Замер это
// переживёт (неудачный вариант отбрасывается), но время потратит впустую.
const int THREADS[] = { 64, 128, 256, 512 };

// Столько времени должен занимать один замер. Меньше — тонет в шуме, больше —
// подбор становится заметен пользователю.
constexpr double TARGET_MEASURE_SEC = 0.010;

// Сколько раз подряд запускается ядро в одном замере. Верхний предел нужен,
// чтобы на очень быстром чанке подбор не растянулся на секунды.
constexpr int MAX_REPEATS = 64;

// Сколько раз мерить один вариант. Берётся лучшее время: помехи от других
// задач на карте всегда добавляют время, но никогда не убавляют.
constexpr int ATTEMPTS = 2;

// Запуск ядра repeats раз подряд. Возвращает время в секундах на один запуск
// или -1, если сетка не запустилась (обычно не хватает регистров под блок).
double timeChunk(const GridTuneTask& task, const LaunchGrid& grid,
                 int repeats, cudaStream_t stream)
{
    cudaEvent_t begin = nullptr;
    cudaEvent_t end   = nullptr;
    if (cudaEventCreate(&begin) != cudaSuccess)
        return -1.0;
    if (cudaEventCreate(&end) != cudaSuccess) {
        cudaEventDestroy(begin);
        return -1.0;
    }

    // У длинного пути объём работы в запуске задаётся числом нитей: каждая
    // берёт свой кусок. У коротких — наоборот, чанк фиксирован, а нити его
    // делят. Поэтому сравнивать длинный путь приходится по пропускной
    // способности, а не по времени запуска.
    const bool isLong = task.kernel == GridTuneTask::Kernel::XorLong;

    const uint64_t threadsTotal = uint64_t(grid.blocks) * uint64_t(grid.threads);

    // Ядро длинного пути читает стартовую маску по номеру нити, поэтому нитей
    // не должно быть больше, чем хост успел заготовить слотов. availableMasks
    // тут не помощник: слоты заполнены не со всего слоя, а с его середины.
    const uint64_t startMasks = std::min(threadsTotal, task.filledStartMasks);

    const uint64_t chunk = isLong
        ? startMasks * task.measureMasksPerThread
        : std::min(task.chunkSize, task.availableMasks);
    if (chunk == 0)
        return -1.0;

    // Замеры размазываются по всему диапазону масок, а не повторяются на одном
    // месте. Стоимость чанка от места зависит: у кода Грея номер маски — это
    // она сама, и стартовый XOR нити стоит столько строк, сколько единиц в
    // маске. В начале диапазона их почти нет, а ровно на степени двойки —
    // всего пара, и замер там показывает, что лишние нити ничего не стоят.
    // Настоящий расчёт проходит все места подряд, поэтому и замер обязан.
    const uint64_t span   = task.availableMasks > chunk ? task.availableMasks - chunk : 0;
    const uint64_t stride = repeats > 1 ? span / uint64_t(repeats - 1) : 0;

    double seconds = -1.0;
    try {
        cudaEventRecord(begin, stream);

        for (int i = 0; i < repeats; ++i) {
            // Длинный путь читает стартовые маски из готового буфера, своего
            // смещения у него нет.
            const uint64_t offset = isLong ? 0 : stride * uint64_t(i);

            if (isLong) {
                int launchGrid = int((startMasks + uint64_t(grid.threads) - 1)
                                     / uint64_t(grid.threads));
                if (launchGrid <= 0) launchGrid = 1;

                launchXorLong(
                    launchGrid, grid.threads, stream,
                    task.scratchSpectrum, task.matrixGlobal,
                    task.cols, task.rows, task.wordsPerRow,
                    chunk,
                    const_cast<int16_t*>(task.startPositions),
                    task.measureMasksPerThread, startMasks, task.r,
                    nullptr, task.slot);
            }
            else if (task.kernel == GridTuneTask::Kernel::Gray) {
                launchGray(
                    grid.blocks, grid.threads, stream,
                    task.scratchSpectrum,
                    task.cols, task.rows, task.wordsPerRow,
                    offset, chunk);
            }
            else {
                launchXorShort(
                    task.scratchSpectrum, task.binomTable,
                    grid.blocks, grid.threads, stream,
                    task.cols, task.rows, task.wordsPerRow,
                    offset, chunk, task.r, task.slot);
            }
        }

        cudaEventRecord(end, stream);
        if (cudaEventSynchronize(end) == cudaSuccess) {
            float ms = 0.0f;
            if (cudaEventElapsedTime(&ms, begin, end) == cudaSuccess) {
                seconds = double(ms) / 1000.0 / double(repeats);
                // Для длинного пути кандидаты обрабатывают разное число масок,
                // поэтому сравнивается время на маску, а не на запуск.
                if (isLong)
                    seconds /= double(chunk);
            }
        }
    }
    catch (...) {
        // Обёртка запуска бросает, если сетка не подошла. Это не ошибка
        // расчёта — просто вариант выбывает из перебора.
        cudaGetLastError();
        seconds = -1.0;
    }

    cudaEventDestroy(begin);
    cudaEventDestroy(end);
    return seconds;
}

// Лучшее из нескольких измерений; -1, если вариант не запускается.
double timeGrid(const GridTuneTask& task, const LaunchGrid& grid,
                int repeats, cudaStream_t stream)
{
    double best = -1.0;
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
        const double seconds = timeChunk(task, grid, repeats, stream);
        if (seconds < 0.0)
            return -1.0;
        if (best < 0.0 || seconds < best)
            best = seconds;
    }
    return best;
}

// Печать одной строки таблицы замеров. У коротких путей сравнимая величина —
// время запуска, у длинного — пропускная способность: там кандидаты
// обрабатывают разное число масок.
void report(const GridTuneTask& task, const LaunchGrid& grid, double seconds,
            const char* note)
{
    if (seconds < 0.0) {
        std::printf("  %4d x %4d : не запустилось%s\n", grid.blocks, grid.threads, note);
        return;
    }

    if (task.kernel == GridTuneTask::Kernel::XorLong) {
        const double masksPerSec = seconds > 0.0 ? 1.0 / seconds : 0.0;
        std::printf("  %4d x %4d : %7.2f млрд масок/с%s\n",
                    grid.blocks, grid.threads, masksPerSec / 1e9, note);
    }
    else {
        std::printf("  %4d x %4d : %7.3f мс%s\n",
                    grid.blocks, grid.threads, seconds * 1000.0, note);
    }
}

} // namespace

LaunchGrid tuneLaunchGrid(const GridTuneTask& task, cudaStream_t stream)
{
    LaunchGrid none;

    if (!task.scratchSpectrum || task.availableMasks == 0)
        return none;

    if (task.kernel == GridTuneTask::Kernel::XorLong) {
        if (!task.startPositions || task.filledStartMasks == 0
                                 || task.measureMasksPerThread == 0)
            return none;
    }
    else if (task.chunkSize == 0) {
        return none;
    }

    cudaDeviceProp props{};
    int device = 0;
    if (cudaGetDevice(&device) != cudaSuccess)
        return none;
    if (cudaGetDeviceProperties(&props, device) != cudaSuccess)
        return none;

    std::vector<int> blockCandidates;
    for (int perSm : BLOCKS_PER_SM)
        blockCandidates.push_back(perSm * props.multiProcessorCount);

    // У длинного пути кандидат не может потребовать больше нитей, чем хост
    // заполнил стартовых масок.
    const uint64_t threadLimit = task.kernel == GridTuneTask::Kernel::XorLong
                               ? task.filledStartMasks : ~0ULL;

    std::vector<int> threadCandidates;
    for (int threads : THREADS) {
        if (threads <= props.maxThreadsPerBlock)
            threadCandidates.push_back(threads);
    }
    if (blockCandidates.empty() || threadCandidates.empty())
        return none;

    // Отправная точка — середина обоих списков: с неё оценивается объём
    // задачи и от неё пляшет первый проход. Сетка обязана укладываться в
    // предел по заготовленным слотам: пробный замер идёт раньше отбора
    // кандидатов, и без этой проверки он читал бы за границей буфера.
    LaunchGrid probe;
    probe.threads = threadCandidates[threadCandidates.size() / 2];
    probe.blocks  = 0;
    for (int blocks : blockCandidates) {
        if (uint64_t(blocks) * uint64_t(probe.threads) > threadLimit)
            break;
        probe.blocks = blocks;
        if (blocks == blockCandidates[blockCandidates.size() / 2])
            break;
    }
    if (probe.blocks == 0)
        return none;

    const auto startedAt = std::chrono::steady_clock::now();

    // Первый запуск после старта всегда дороже остальных: подгружается
    // модуль, прогревается карта. Меряем вхолостую.
    timeChunk(task, probe, 1, stream);

    const double chunkSec = timeChunk(task, probe, 1, stream);
    if (chunkSec <= 0.0)
        return none;

    // Оценка всего расчёта по одному чанку. Она грубая — стоимость маски
    // зависит от числа складываемых строк, — но чтобы отличить задачу на
    // секунду от задачи на час, точности хватает с запасом.
    const double estimatedSec = task.kernel == GridTuneTask::Kernel::XorLong
        ? chunkSec * double(task.totalMasks)
        : chunkSec * double(task.totalMasks)
                   / double(std::min(task.chunkSize, task.availableMasks));
    if (estimatedSec < task.minWorthSeconds)
        return none;

    // Один замер должен длиться около TARGET_MEASURE_SEC, иначе в него лезет
    // шум запуска: на длинном пути замеры по четыре миллисекунды разъезжались
    // на десять процентов от прохода к проходу.
    double probeLaunchSec = chunkSec;
    if (task.kernel == GridTuneTask::Kernel::XorLong) {
        // Для длинного пути chunkSec — время на маску, а не на запуск.
        probeLaunchSec = chunkSec * double(uint64_t(probe.blocks) * uint64_t(probe.threads)
                                         * task.measureMasksPerThread);
    }

    int repeats = probeLaunchSec > 0.0 ? int(TARGET_MEASURE_SEC / probeLaunchSec) : 1;
    repeats = std::max(1, std::min(repeats, MAX_REPEATS));

    LaunchGrid best;
    double bestTime = -1.0;

    if (task.verbose) {
        if (task.kernel == GridTuneTask::Kernel::XorLong)
            std::printf("подбор: %llu масок на нить, до %llu стартовых масок\n",
                        (unsigned long long)task.measureMasksPerThread,
                        (unsigned long long)task.filledStartMasks);
        else
            std::printf("подбор: чанк %llu масок, %.3f мс на чанк, %d повторов\n",
                        (unsigned long long)std::min(task.chunkSize, task.availableMasks),
                        chunkSec * 1000.0, repeats);
    }

    // Настройки пользователя — первый кандидат и заодно нижняя планка.
    if (task.userGrid.isValid()) {
        const double seconds = timeGrid(task, task.userGrid, repeats, stream);
        if (task.verbose)
            report(task, task.userGrid, seconds, "  (из настроек)");
        if (seconds > 0.0) {
            bestTime = seconds;
            best     = task.userGrid;
        }
    }

    // Проходы по очереди: сначала блоки при фиксированных нитях, потом нити
    // при найденных блоках, потом блоки ещё раз. Полную решётку не перебираем
    // — это дороже, а по замерам --sweep такой обход подходит к настоящему
    // оптимуму на 10-25 %, чего с лихвой хватает против 24 % отставания
    // умолчания.
    auto fits = [&](const LaunchGrid& g) {
        return uint64_t(g.blocks) * uint64_t(g.threads) <= threadLimit;
    };

    auto sweepBlocks = [&](int threads) {
        for (int blocks : blockCandidates) {
            const LaunchGrid grid{ blocks, threads };
            if (!fits(grid))
                continue;
            const double seconds = timeGrid(task, grid, repeats, stream);
            if (task.verbose)
                report(task, grid, seconds, "");
            if (seconds < 0.0)
                continue;
            if (bestTime < 0.0 || seconds < bestTime) {
                bestTime = seconds;
                best     = grid;
            }
        }
    };
    auto sweepThreads = [&](int blocks) {
        for (int threads : threadCandidates) {
            const LaunchGrid grid{ blocks, threads };
            if (!fits(grid))
                continue;
            const double seconds = timeGrid(task, grid, repeats, stream);
            if (task.verbose)
                report(task, grid, seconds, "");
            if (seconds < 0.0)
                continue;
            if (bestTime < 0.0 || seconds < bestTime) {
                bestTime = seconds;
                best     = grid;
            }
        }
    };

    sweepBlocks(probe.threads);
    if (!best.isValid())
        return none;

    sweepThreads(best.blocks);
    sweepBlocks(best.threads);

    if (task.verbose) {
        const double spent = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - startedAt).count();
        std::printf("подбор занял %.3f с, выбрано %d x %d\n", spent, best.blocks, best.threads);
    }

    return best;
}
