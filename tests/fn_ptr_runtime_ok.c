#include <stdint.h>

typedef int64_t (*cyan_unary_i64_fn)(int64_t);

int64_t call_i64(void *raw, int64_t value) {
    return ((cyan_unary_i64_fn)raw)(value);
}
