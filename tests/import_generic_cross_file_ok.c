#include <stdint.h>

int64_t *get_int_buffer(void) {
    static int64_t values[] = {7};
    return values;
}

double *get_float_buffer(void) {
    static double values[] = {3.5};
    return values;
}
