static long storage = 0;

void* alloc_bytes(long n) {
    (void)n;
    return &storage;
}
