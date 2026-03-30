struct Box {
    int value;
};

struct OneBox {
    Box* ptr;
};

impl drop(&mut Box value) {
    record_drop();
    return;
}

impl drop(&mut OneBox boxes) {
    unchecked {
        drop(boxes.ptr[0]);
    }
}

extern {
    Box* get_box_buffer();
    int dropped_count();
    void record_drop();
}

int compute() {
    OneBox boxes = {get_box_buffer()};
    return 0;
}

int main() {
    compute();
    return dropped_count();
}
