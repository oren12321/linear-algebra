#ifndef COMPUTOC_UTILS_H
#define COMPUTOC_UTILS_H

#include <computoc/concepts.h>
#include <computoc/math.h>

namespace computoc {
    namespace details {
        template <Arithmetic T1, Arithmetic T2>
        bool is_equal(const T1& a, const T2& b, const decltype(abs(a - b))& eps = sqrt(epsilon <typename decltype(abs(a - b))>()))
        {
            return abs(a - b) <= eps;
        }
    }

    using details::is_equal;
}

#endif
