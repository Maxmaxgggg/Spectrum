#pragma once
#include <iostream>
#include <vector>

struct BitMask {
    unsigned  BITS;   // длина маски в битах
    unsigned  WORDS;  // число 64-битных слов для хранения маски
    unsigned  COUNT;  // число единиц в маске
    uint64_t* DATA = nullptr;
    uint64_t* lastMask; // последняя маска для данного числа единиц

    BitMask(unsigned len, unsigned ones);
    BitMask(const BitMask& other);
    BitMask(BitMask&& other) noexcept;

    ~BitMask();

    // Печать маски
    void print() const;

    template<typename F>
    void forEachSetBit(F f) const;
    // nextMask заменить текущую маску на следующую с тем же числом единиц
    bool nextMask();
    void setMask(uint64_t maskIdx, uint64_t** binomTable, unsigned maxComb);
    // Copy/Move assign
    BitMask& operator=(const BitMask& other) {
        if (this == &other) return *this;
        if (WORDS != other.WORDS) {
            free(DATA);
            WORDS = other.WORDS;
            DATA = (uint64_t*)calloc(WORDS, sizeof(uint64_t));
            if (!DATA) throw std::bad_alloc();
        }
        BITS = other.BITS;
        COUNT = other.COUNT;
        std::copy(other.DATA, other.DATA + WORDS, DATA);
        return *this;
    }
    BitMask& operator=(BitMask&& other) noexcept {
        if (this != &other) {
            free(DATA);
            BITS = other.BITS;
            WORDS = other.WORDS;
            COUNT = other.COUNT;
            DATA = other.DATA;
            other.DATA = nullptr;
            other.BITS = other.WORDS = other.COUNT = 0;
        }
        return *this;
    }


private:
    // Установить COUNT самых младших единиц
    void initLowest(unsigned r);
};


// Параметр F - функтор (н-р лямбда-функция)
template<typename F>
inline void BitMask::forEachSetBit(F f) const {
    for (unsigned wi = 0; wi < WORDS; ++wi) {
        // Работаем с локальной копией слова, не изменяем массив DATA
        uint64_t w = DATA[wi];
        // Если слово не равно нулевому ( есть ненулевые биты )
        while (w) {
            // Получаем локальную позицию ненулевого бита в слове
            unsigned long bi;
            _BitScanForward64(&bi, w);
            unsigned bit = (unsigned)bi;
            // Получаем глобальную позицию ненулевого бита в маске
            unsigned pos = (wi << 6) + bit;
            //
            f(pos);
            // сброс младшего бита
            w &= (w - 1);
        }
    }
}