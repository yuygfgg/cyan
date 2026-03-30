enum Action {
    Skip,
    Add(int),
};

int sum_odd_values() {
    int sum = 0;

    for (int i = 0; i < 5; i++) {
        if (i % 2 == 0) {
            Action action = Skip();
            switch (move action) {
                case Skip:
                    continue;
                case Add(value):
                    sum = sum + value;
            }
        } else {
            Action action = Add(i);
            switch (move action) {
                case Skip:
                    continue;
                case Add(value):
                    sum = sum + value;
            }
        }
    }

    return sum;
}

int main() {
    if (sum_odd_values() == 4) {
        return 0;
    }
    return 1;
}
