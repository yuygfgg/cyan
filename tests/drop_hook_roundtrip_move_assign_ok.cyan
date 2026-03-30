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

Token id(Token token) {
    return token;
}

int main() {
    Token token = {1};
    token = id(token);
    return dropped_count();
}
