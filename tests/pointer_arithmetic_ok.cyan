extern {
    int* get_buffer();
}

int main() {
    unchecked {
        int* ptr = get_buffer();
        int* next = ptr + 1;
        *next = 8;
        return *(ptr + 1);
    }
}
