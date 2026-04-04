#include <stdint.h>

static int64_t g_drop_count = 0;

void record_drop(void) { g_drop_count += 1; }

int64_t dropped_count(void) { return g_drop_count; }
