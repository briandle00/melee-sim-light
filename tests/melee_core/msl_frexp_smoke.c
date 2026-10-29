// The MSL frexp (src/MSL/math_1.c) splits a double through MSL_HI and
// MSL_LO, its high and low words. Their order is the host's: on a
// little-endian host the high word is the second one in memory.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

double frexp(double x, int* exponent);

static uint64_t bits_of(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int main(void)
{
    static const struct {
        double x;
        double mantissa;
        int exponent;
    } cases[] = {
        { 8.0, 0.5, 4 },
        { -3.0, -0.75, 2 },
        { 0.1, 0.8, -3 },
        { 1.0, 0.5, 1 },
        { 0x1.8p-1030, 0.75, -1029 }, /* subnormal */
        { 0.0, 0.0, 0 },
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int exponent = 12345;
        double mantissa = frexp(cases[i].x, &exponent);
        if (bits_of(mantissa) != bits_of(cases[i].mantissa) ||
            exponent != cases[i].exponent)
        {
            fprintf(stderr,
                    "frexp(%a): got %a * 2^%d, want %a * 2^%d\n",
                    cases[i].x, mantissa, exponent, cases[i].mantissa,
                    cases[i].exponent);
            failures++;
        }
    }
    if (failures != 0) {
        return 1;
    }
    printf("frexp: %zu cases pass\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
