import modules.buffer;

extern {
    int* get_int_buffer();
    float* get_float_buffer();
}

int main() {
    Buffer<int> ints = {get_int_buffer(), 1};
    Buffer<float> floats = {get_float_buffer(), 1};
    int left = buffer_first(&ints);
    float right = buffer_first(&floats);
    if (left == 7 && right == 3.5) {
        return 0;
    }
    return 1;
}
