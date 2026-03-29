#include <stddef.h>

static int g_dropped_count = 0;

int dropped_count(void) { return g_dropped_count; }
void record_drop(void) { g_dropped_count += 1; }
