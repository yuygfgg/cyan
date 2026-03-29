int inner<T>(const char* label, []T values) {
    return len(values);
}

int outer<T>(const char* label, []T values) {
    return inner(label, values);
}

int main() {
    int[2] values = [3, 4];
    return outer("count", values) - 2;
}
