#include <stddef.h>

struct Box {
    long long value;
};

static struct Box g_box = {42};
static int g_dropped_count = 0;

struct Box *get_box_buffer(void) { return &g_box; }
int dropped_count(void) { return g_dropped_count; }
void record_drop(void) { g_dropped_count += 1; }
