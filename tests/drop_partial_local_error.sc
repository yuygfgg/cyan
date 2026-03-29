struct Box {
    int value;
};

impl drop(&mut Box value) {}

struct Wrap {
    Box box;
};

int main() {
    Wrap wrap = {{1}};
    unchecked {
        drop(wrap.box);
    }
    return 0;
}
