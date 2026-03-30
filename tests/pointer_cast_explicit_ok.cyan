extern {
    void* alloc_bytes(size_t n);
}

int main() {
    unchecked {
        int* ptr = alloc_bytes(sizeof(int)) as int*;
        *ptr = 41;
        return *ptr + 1;
    }
}
