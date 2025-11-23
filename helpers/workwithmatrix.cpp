#include "workwithmatrix.h"

uint64_t gcd(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// Функция для поиска максимального биноминального коэффициента, влезающего в uint64_t
uint64_t maxCombIndex(uint64_t k) {
    constexpr uint64_t U = std::numeric_limits<uint64_t>::max();

    if (k == 0) return 0;

    uint64_t C = 1;     
    uint64_t best_i = 0;

    uint64_t half = k / 2;
    for (uint64_t i = 1; i <= half; ++i) {
        uint64_t num = k - i + 1ULL;
        uint64_t den = i;
        uint64_t g = gcd(num, den);
        num /= g;
        den /= g;
        uint64_t g2 = gcd(C, den);
        C /= g2;
        den /= g2;
        if (C != 0 && num > U / C) {
            break;
        }
        uint64_t next = C * num;
        next /= den;
        C = next;
        best_i = i;
    }
    return best_i;
}
