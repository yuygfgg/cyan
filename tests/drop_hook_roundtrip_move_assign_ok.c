static int g_dropped = 0;

void record_drop(void) { g_dropped += 1; }

int dropped_count(void) { return g_dropped; }
