export struct Buffer<T> {
    T* ptr;
    int len;
};

T buffer_load<T>(&Buffer<T> buffer, int index) {
    unchecked {
        return buffer.ptr[index];
    }
}

export T buffer_first<T>(&Buffer<T> buffer) {
    return buffer_load(buffer, 0);
}
