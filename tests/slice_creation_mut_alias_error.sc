int main() {
    int[1] values = [0];
    &mut int slot = &values[0];
    []int view = values;
    return *slot + view[0];
}
