extern {
    int* get_buffer();
}

int main() {
    unchecked {
        int* ptr = get_buffer();
        &mut int borrow = &mut *ptr;
        *borrow = 5;
        return *ptr;
    }
}
