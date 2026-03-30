interface<T> void set_to(&mut T value, int next);

struct Box {
    int value;
};

impl set_to(&mut Box box, int next) {
    box.value = next;
}

void apply(&set_to action, int next) {
    set_to(action, next);
}

int main() {
    Box box = {1};
    apply(&mut box, 9);
    return box.value;
}
