struct Pair {
    &int shared;
    &mut int unique;
};

int main() {
    int value = 0;
    &mut int alias = &mut value;
    Pair pair = {&*alias, alias};
    return 0;
}
