extern {
    int* get_buffer();
}

int main() {
    int result = 0;
    unchecked {
        int* ptr = get_buffer();
        ptr[1] = 9;
        result = ptr[0] + ptr[1];
    }
    return result;
}
