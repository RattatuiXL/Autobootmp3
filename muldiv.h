#ifndef AUTOMUSIC_MULDIV_H
#define AUTOMUSIC_MULDIV_H

#include <psptypes.h>

/* Calculate floor(a * b / divisor) without relying on 64-bit division. */
static unsigned int muldiv_u32(unsigned int a, unsigned int b,
                               unsigned int divisor)
{
    unsigned long long product, quotient = 0, remainder = 0;
    int bit;

    if (!divisor) return 0xFFFFFFFFu;
    product = (unsigned long long)a * b;

    for (bit = 63; bit >= 0; bit--) {
        remainder = (remainder << 1) | ((product >> bit) & 1u);
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= (unsigned long long)1 << bit;
        }
    }

    return quotient > 0xFFFFFFFFu ? 0xFFFFFFFFu : (unsigned int)quotient;
}

#endif
