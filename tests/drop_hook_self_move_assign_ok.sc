extern {
    void record_drop();
    int dropped_count();
}

struct Token {
    int value;
};

impl drop(&mut Token token) {
    record_drop();
}

int main() {
    Token token = {1};
    token = move token;
    return dropped_count();
}
