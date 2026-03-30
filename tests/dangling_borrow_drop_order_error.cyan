struct Token {
    int value;
};

impl drop(&mut Token token) {
    token.value = 0;
}

struct Holder {
    &Token ptr;
};

impl drop(&mut Holder holder) {
    int observed = holder.ptr.value;
    return;
}

int main() {
    Token stable = {0};
    Holder holder = {&stable};
    Token later = {1};
    holder.ptr = &later;
    return 0;
}
