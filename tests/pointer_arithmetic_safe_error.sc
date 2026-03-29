extern {
    int* get_buffer();
}

int main() {
    int* ptr = get_buffer();
    int* next = ptr + 1;
    return 0;
}
