#pragma once
#include <iostream>
struct BitMask {
    unsigned  BITS;   // длина маски в битах
    unsigned  WORDS;  // число 64-битных слов для хранения маски
    unsigned  COUNT;  // число единиц в маске
    uint64_t* DATA = nullptr;

    BitMask(unsigned len, unsigned ones);
    BitMask(const BitMask& other);
    BitMask(BitMask&& other) noexcept;

    ~BitMask();

    // Печать маски
    void print() const;
    // nextMask заменить текущую маску на следующую комбинацию с тем же числом единиц
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
