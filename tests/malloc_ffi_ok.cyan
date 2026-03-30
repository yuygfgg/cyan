extern {
    void* malloc(size_t n);
    void free(void* p);
}

int main() {
    unchecked {
        int* ptr = malloc(sizeof(int));
        *ptr = 7;
        int value = *ptr;
        free(ptr);
        if (value == 7) {
            return 0;
        }
        return 1;
    }
}
