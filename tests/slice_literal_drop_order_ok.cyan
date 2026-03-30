struct Token {
    int value;
};

impl drop(&mut Token token) {}

struct Holder {
    []Token view;
};

impl drop(&mut Holder holder) {
    int observed = holder.view[0].value;
    return;
}

int main() {
    Holder holder = {[{4}, {8}]};
    return 0;
}
