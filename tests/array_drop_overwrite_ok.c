static long long count = 0;

void record_drop(void) { count++; }

long long dropped_count(void) { return count; }
