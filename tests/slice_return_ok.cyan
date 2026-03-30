int sum([]int values) {
    int i = 0;
    int total = 0;
    while (i < len(values)) {
        total = total + values[i];
        i++;
    }
    return total;
}

[]int id([]int values) {
    return values;
}

[]int all(&int[3] values) {
    return values;
}

int main() {
    int[3] values = [2, 3, 4];
    if (sum(id(values)) != 9) {
        return 1;
    }
    if (sum(all(&values)) != 9) {
        return 1;
    }

    []int view = id(all(&values));
    if (view[1] != 3) {
        return 1;
    }
    return 0;
}
