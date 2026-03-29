extern {
    void* alloc_bytes(size_t n);
}

int main() {
    unchecked {
        int* ptr = alloc_bytes(sizeof(int));
        *ptr = 42;
        return *ptr;
    }
}
