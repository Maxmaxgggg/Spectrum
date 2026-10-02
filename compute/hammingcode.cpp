#include "hammingcode.h"

#include "bchcode.h"

namespace Hamming {

namespace {

Code make(int r, bool extend, int shorten, bool withRows)
{
    Code code;
    if (r < MIN_R || r > MAX_R)
        return code;
    const int n = (1 << r) - 1;
    const int k = n - r;
    if (shorten < 0 || shorten >= k)
        return code;
    code.n = n - shorten + (extend ? 1 : 0);
    code.k = k - shorten;
    code.d = extend ? 4 : 3;
    if (!withRows)
        return code;
    const Bch::Code cyclic = Bch::cyclic(r, Bch::reciprocal(Bch::primitivePolynomial(r)), extend, shorten);
    code.rows = cyclic.rows;
    return code;
}

} // namespace

Code build(int r, bool extend, int shorten)    { return make(r, extend, shorten, true); }
Code describe(int r, bool extend, int shorten) { return make(r, extend, shorten, false); }

} // namespace Hamming
