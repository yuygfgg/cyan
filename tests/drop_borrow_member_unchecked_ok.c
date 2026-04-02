#include <stdint.h>

typedef struct {
    int64_t id;
} Token;

static Token g_token = {.id = 123};
static int g_drop_count = 0;
static int g_drop_value_matched = 0;

Token* token_ptr(void) { return &g_token; }

void record_drop(int64_t value) {
    g_drop_count += 1;
    if (value == g_token.id) {
        g_drop_value_matched = 1;
    }
}

int64_t dropped_ok(void) {
    return (g_drop_count == 1 && g_drop_value_matched != 0) ? 0 : 1;
}
