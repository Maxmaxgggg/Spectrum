#include "bitmask.h"

BitMask::BitMask(unsigned len, unsigned ones) {
    if (len == 0) throw std::invalid_argument("BitMask: length must be > 0");
    if (ones > len) throw std::invalid_argument("BitMask: ones > length");
    BITS = len;
    WORDS = (BITS + 63) / 64;
    COUNT = ones;
    DATA = (uint64_t*)calloc(WORDS, sizeof(uint64_t));
    if (!DATA) throw std::bad_alloc();
    initLowest(COUNT);


    // Ѕуфер дл€ последней маски
    lastMask = (uint64_t*)calloc(WORDS, sizeof(uint64_t));
    if (!lastMask) {
        free(DATA);
        DATA = nullptr;
        throw std::bad_alloc();
    }

    // —троим последнюю маску: COUNT единиц в старших позици€х
    // позиции: [BITS-COUNT ... BITS-1]
    for (unsigned i = 0; i < COUNT; ++i) {
        unsigned bitPos = (unsigned)(BITS - 1 - i);
        unsigned wordIdx = bitPos >> 6;        // /64
        unsigned bitIdx = bitPos & 63u;       // %64
        lastMask[wordIdx] |= (1ULL << bitIdx);
    }
}

BitMask::BitMask(const BitMask& other) {
    BITS = other.BITS;
    WORDS = other.WORDS;
    COUNT = other.COUNT;
    DATA = (uint64_t*)calloc(WORDS, sizeof(uint64_t));
    if (!DATA) throw std::bad_alloc();
    std::copy(other.DATA, other.DATA + WORDS, DATA);
}

BitMask::BitMask(BitMask&& other) noexcept {
    BITS = other.BITS;
    WORDS = other.WORDS;
    COUNT = other.COUNT;
    DATA = other.DATA;
    other.DATA = nullptr;
    other.BITS = other.WORDS = other.COUNT = 0;
}


BitMask::~BitMask() {
    if (DATA) {
        free(DATA);
        DATA = nullptr;
    }
    if (lastMask) {
        free(lastMask);
        lastMask = nullptr;
    }
}

void BitMask::print() const {
    for (int i = static_cast<int>(BITS) - 1; i >= 0; --i) {
        unsigned wi = static_cast<unsigned>(i) >> 6;
        unsigned bi = static_cast<unsigned>(i) & 63u;
        std::cout << ((DATA[wi] >> bi) & 1ULL);
    }
    std::cout << "\n";
}

// ‘ункци€ возварщает индекс младшего установленного бита (допустим, если число 5 = 0b0101, то функци€ вернЄт 0)
static inline unsigned ctz64(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long idx;
    // _BitScanForward64 возвращает 0 если x==0, но мы не вызываем с 0
    _BitScanForward64(&idx, x);
    return (unsigned)idx;
#else
    return (unsigned)__builtin_ctzll(x);
#endif
}

bool BitMask::nextMask()
{
    // ѕроверки
    if (COUNT == 0 || COUNT > BITS) return false;
    //if (isLastMask()) return false;

    // 1) Ќаходим младшую установленную единицу
    int wordIndex = -1;
    unsigned bitIndex = 0;
    for (unsigned wi = 0; wi < WORDS; ++wi) {
        uint64_t w = DATA[wi];
        if (w != 0ULL) {
            bitIndex = ctz64(w);
            wordIndex = (int)wi;
            break;
        }
    }
    // Ќекорректное состо€ние: если единиц нет
    if (wordIndex < 0) return false;

    // √лобальный индекс первого ненулевого бита
    unsigned t0 = (unsigned)wordIndex * 64u + bitIndex;

    // 2) скопировать исходные слова, начина€ с wordIndex до конца,
    //    т.к. именно там мы будем вычисл€ть ones = r ^ orig.
    unsigned start = (unsigned)wordIndex;
    unsigned origLen = WORDS - start;
    std::vector<uint64_t> orig;
    orig.resize(origLen);
    for (unsigned i = 0; i < origLen; ++i) orig[i] = DATA[start + i];

    // 3) выполнить r = x + (1<<bitIndex) с переносами (измен€ет DATA in-place)
    uint64_t carry = (1ULL << bitIndex);
    unsigned i = start;
    for (; i < WORDS && carry; ++i) {
        uint64_t old = DATA[i];
        uint64_t sum = old + carry;
        DATA[i] = sum;
        carry = (sum < old) ? 1ULL : 0ULL;
    }

    if (carry) {
        // переполнение (перенос вышел за старшую границу) Ч восстанавливаем
        for (unsigned k = 0; k < origLen; ++k) DATA[start + k] = orig[k];
        return false;
    }

    // 4) ones = r ^ orig (только дл€ области [start .. WORDS-1])
    //    будем обращатьс€ к ones по индексу s (0..origLen-1)
    //    и немедленно смещать их вправо на (t0 + 2) бит, OR-€ в DATA.
    unsigned shiftAmount = t0 + 2u;
    uint64_t totalBits = WORDS * 64u;
    if (shiftAmount < totalBits) {
        unsigned wordShift = shiftAmount >> 6;      // на сколько слов сдвиг
        unsigned bitShift = shiftAmount & 63u;     // остаток в битах

        // ƒл€ удобства: обработаем источники s=0..origLen-1
        // srcWordIndex = start + s
        // destWordIndex = (start + s) - wordShift  (может быть < 0 => игнорируем)
        // value = (ones[s] >> bitShift) | (ones[s+1] << (64-bitShift)) (если bitShift!=0)
        for (unsigned s = 0; s < origLen; ++s) {
            uint64_t ones = DATA[start + s] ^ orig[s];
            ptrdiff_t destIdxAbs = (ptrdiff_t)(start + s) - (ptrdiff_t)wordShift;
            if (destIdxAbs < 0) continue;               // попадает ниже нул€ Ч игнорируем
            if ((unsigned)destIdxAbs >= WORDS) continue; // вне диапазона Ч игнорируем

            uint64_t low = (bitShift == 0) ? ones : (ones >> bitShift);
            uint64_t high = 0;
            if (bitShift != 0) {
                // смотрим следующий source слово (s+1), если есть
                if (s + 1 < origLen) {
                    uint64_t ones_next = DATA[start + s + 1] ^ orig[s + 1];
                    high = ones_next << (64u - bitShift);
                }
            }
            uint64_t val = low | high;
            DATA[(unsigned)destIdxAbs] |= val;
        }
    }
    // иначе shiftAmount >= totalBits => ones_shifted == 0, ничего OR'ить не нужно
    // √отово Ч следующа€ маска записана в DATA
    return true;
}
void BitMask::initLowest(unsigned r) {
    std::fill(DATA, DATA + WORDS, 0ULL);
    for (unsigned i = 0; i < r; ++i) {
        unsigned wi = i >> 6;
        unsigned bi = i & 63u;
        DATA[wi] |= (1ULL << bi);
    }
}


void BitMask::setMask(uint64_t maskIdx, uint64_t** binomTable, unsigned maxComb) {
    if (COUNT == 0) {
        std::fill(DATA, DATA + WORDS, 0ULL);
        return;
    }
    if (COUNT > BITS) {
        throw std::invalid_argument("COUNT > BITS in BitMask::unrankInPlace");
    }

    // очистим маску
    std::fill(DATA, DATA + WORDS, 0ULL);

    // при возможности Ч проверим, что maskIdx в допустимом диапазоне (только если есть запись)
    if (COUNT <= maxComb) {
        uint64_t total = binomTable[BITS][COUNT];
        if (maskIdx >= total) {
            // диагностическа€ информаци€, лучше кинуть исключение или корректно обработать
            fprintf(stderr, "setMask: maskIdx (%llu) >= C(%u,%u)=%llu\n",
                (unsigned long long)maskIdx, BITS, COUNT, (unsigned long long)total);
            throw std::out_of_range("maskIdx out of range in setMask");
        }
    }

    unsigned nextPos = 0;  // минимальна€ позици€, в которую ещЄ можно ставить единицу

    for (unsigned i = COUNT; i > 0; --i) {  // ставим i-ую единицу
        unsigned j = nextPos;
        while (j <= BITS - i) {
            unsigned n_rem = BITS - j - 1;  // оставшиес€ позиции
            unsigned t_rem = i - 1;         // оставшиес€ единицы

            // если t_rem > n_rem Ч комбинаций нет, j не подходит
            if (t_rem > n_rem) break;

            // если таблица не содержит t_rem (t_rem > maxComb), не подставл€ем 0 Ч просто не пропускаем
            if (t_rem > maxComb) {
                // Ќет данных в binomTable; считаем, что C(n_rem,t_rem) > maskIdx, т.е. не пропускаем j.
                break;
            }

            uint64_t c = binomTable[n_rem][t_rem];

            if (c <= maskIdx) {
                maskIdx -= c;
                ++j;
            }
            else {
                break;
            }
        }
        // j Ч позици€ дл€ текущей единицы
        unsigned wi = j >> 6;
        unsigned bi = j & 63u;
        DATA[wi] |= (1ULL << bi);
        nextPos = j + 1;
    }
}


