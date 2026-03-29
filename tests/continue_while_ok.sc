int main() {
    int sum = 0;
    int value = 0;

    while (value < 6) {
        value++;
        if (value % 2 == 0) {
            continue;
        }
        sum = sum + value;
    }

    if (sum == 9) {
        return 0;
    }
    return 1;
}
