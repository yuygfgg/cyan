struct Token {
    int value;
};

impl drop(&mut Token token) {}

int main() {
    Token x = {1};
    Token y = move x;
    x = move x;
    return 0;
}
