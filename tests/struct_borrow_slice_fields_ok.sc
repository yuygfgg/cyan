struct Token {
    int value;
};

struct ViewAndHead {
    []Token view;
    &Token head;
};

int read(&Token token) {
    return token.value;
}

ViewAndHead expose([]Token values)
    depends(return.view on values, return.head on values) {
    return {values, &values[0]};
}

int main() {
    Token[2] items = [{4}, {9}];
    ViewAndHead result = expose(&items);
    if (read(result.head) != 4) {
        return 1;
    }
    if (read(result.view[1]) != 9) {
        return 2;
    }
    return 0;
}
