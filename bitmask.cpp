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
    free(DATA);
    DATA = nullptr;
}

void BitMask::print() const {
    for (int i = static_cast<int>(BITS) - 1; i >= 0; --i) {
        unsigned wi = static_cast<unsigned>(i) >> 6;
        unsigned bi = static_cast<unsigned>(i) & 63u;
        std::cout << ((DATA[wi] >> bi) & 1ULL);
    }
    std::cout << "\n";
}

bool BitMask::nextMask() {
    // Быстрая граница для статических временных буферов (регулируй по нужде)
    static constexpr unsigned MAX_WORDS_STATIC = 8; // поддержка до 8192 бит без хипа

    int wordIndex = -1;
    unsigned long bitIndex = 0UL;
    for (int wi = 0; wi < WORDS; ++wi) {
        uint64_t w = DATA[wi];
        if (w != 0ULL) {

            _BitScanForward64(&bitIndex, w);
            wordIndex = wi;
            break;
        }
    }

    // 1.5) Проверка: если текущая маска уже "последняя" для COUNT, вернуть false.
    unsigned k = COUNT; // число единиц фиксировано
    unsigned first_pos = BITS - k; // глобальная позиция первой (самой левой) единицы в последней комбинации
    bool isLast = true;
    for (int j = static_cast<int>(WORDS) - 1; j >= 0; --j) {
        unsigned word_lo = static_cast<unsigned>(j) << 6;
        unsigned word_hi = word_lo + 63;
        if (word_hi > BITS - 1) word_hi = BITS - 1;

        unsigned overlap_lo = (first_pos > word_lo) ? first_pos : word_lo;
        unsigned overlap_hi = (BITS - 1 < word_hi) ? (BITS - 1) : word_hi;

        uint64_t expected = 0ULL;
        if (overlap_lo <= overlap_hi) {
            unsigned m = overlap_hi - overlap_lo + 1;      // число единиц в этом слове
            unsigned shift = overlap_lo - word_lo;        // позиция внутри слова
            if (m == 64) expected = ~0ULL;
            else expected = (((1ULL << m) - 1ULL) << shift);
        }
        if (DATA[j] != expected) { isLast = false; break; }
    }
    if (isLast) return false;

    // 2) подготовить временные буферы tmpOrig (orig) и tmpShift
    uint64_t* tmpOrig = nullptr;
    uint64_t* tmpShift = nullptr;
    bool usedHeap = false;

    if (WORDS <= MAX_WORDS_STATIC) {
        static thread_local uint64_t staticBuf1[MAX_WORDS_STATIC];
        static thread_local uint64_t staticBuf2[MAX_WORDS_STATIC];
        tmpOrig = staticBuf1;
        tmpShift = staticBuf2;
    }
    else {
        tmpOrig = (uint64_t*)malloc(sizeof(uint64_t) * WORDS);
        tmpShift = (uint64_t*)malloc(sizeof(uint64_t) * WORDS);
        if (!tmpOrig || !tmpShift) {
            free(tmpOrig);
            free(tmpShift);
            throw std::bad_alloc();
        }
        usedHeap = true;
    }

    // 3) копия исходного DATA в tmpOrig
    for (unsigned i = 0; i < WORDS; ++i) tmpOrig[i] = DATA[i];

    // 4) r = x + (1 << t) — выполним in-place в DATA, начиная с найденного слова
    unsigned wi = static_cast<unsigned>(wordIndex);
    unsigned bi = static_cast<unsigned>(bitIndex);
    uint64_t carry = (1ULL << bi);
    for (unsigned i = wi; i < WORDS && carry; ++i) {
        uint64_t old = DATA[i];
        uint64_t sum = old + carry;
        carry = (sum < old) ? 1ULL : 0ULL;
        DATA[i] = sum;
    }
    if (carry) {
        // переполнение (не ожидается обычно, т.к. мы проверяли isLast), восстановим и выйдем
        for (unsigned i = 0; i < WORDS; ++i) DATA[i] = tmpOrig[i];
        if (usedHeap) { free(tmpOrig); free(tmpShift); }
        return false;
    }

    // 5) ones = r ^ x  (tmpOrig := DATA ^ tmpOrig)
    for (unsigned i = 0; i < WORDS; ++i) tmpOrig[i] = DATA[i] ^ tmpOrig[i];

    // 6) ones_shifted = ones >> (t + 2) -> tmpShift
    unsigned t = (wi << 6) + bi;
    unsigned shift = t + 2;
    // очистим tmpShift
    for (unsigned i = 0; i < WORDS; ++i) tmpShift[i] = 0ULL;
    if (shift < WORDS * 64) {
        unsigned word_shift = shift >> 6;
        unsigned bit_shift = shift & 63u;
        if (word_shift < WORDS) {
            if (bit_shift == 0) {
                for (unsigned i = 0; i + word_shift < WORDS; ++i)
                    tmpShift[i] = tmpOrig[i + word_shift];
            }
            else {
                for (unsigned i = 0; i + word_shift < WORDS; ++i) {
                    uint64_t low = tmpOrig[i + word_shift] >> bit_shift;
                    uint64_t high = 0ULL;
                    if (i + word_shift + 1 < WORDS)
                        high = tmpOrig[i + word_shift + 1] << (64 - bit_shift);
                    tmpShift[i] = low | high;
                }
            }
        }
    }

    // 7) DATA = r | ones_shifted  (in-place)
    for (unsigned i = 0; i < WORDS; ++i) DATA[i] |= tmpShift[i];

    // 8) очистка лишних старших битов
    unsigned excess = (WORDS << 6) - BITS;
    if (excess) DATA[WORDS - 1] &= (~0ULL >> excess);

    // 9) очистка/освобождение буферов
    if (usedHeap) { free(tmpOrig); free(tmpShift); }

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

    unsigned nextPos = 0;  // минимальная позиция, в которую ещё можно ставить единицу

    for (unsigned i = COUNT; i > 0; --i) {  // ставим i-ую единицу
        unsigned j = nextPos;
        while (j <= BITS - i) {
            unsigned n_rem = BITS - j - 1;  // оставшиеся позиции
            unsigned t_rem = i - 1;         // оставшиеся единицы
            uint64_t c = 0;
            if (t_rem <= n_rem && t_rem <= maxComb) {
                c = binomTable[n_rem][t_rem];
            }
            else {
                c = 0;
            }

            if (c <= maskIdx) {
                maskIdx -= c;
                ++j;
            }
            else {
                break;
            }
        }
        // j — позиция для текущей единицы
        unsigned wi = j >> 6;
        unsigned bi = j & 63u;
        DATA[wi] |= (1ULL << bi);
        nextPos = j + 1;
    }
}