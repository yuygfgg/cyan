int sum([]int values) {
    int i = 0;
    int total = 0;
    while (i < len(values)) {
        total = total + values[i];
        i++;
    }
    return total;
}

int main() {
    int[3] values = [3, 4, 5];
    if (sum(values) != 12) {
        return 1;
    }

    []int view = values;
    if (len(view) != 3) {
        return 1;
    }
    if (sum(view) != 12) {
        return 1;
    }
    return 0;
}
