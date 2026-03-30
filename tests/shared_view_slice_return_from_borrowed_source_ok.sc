[]&int id_from_borrowed(&[]&int values) depends(return on values) {
    return *values;
}

int main() {
    int left = 1;
    int right = 2;
    []&int values = [&left, &right];
    []&int same = id_from_borrowed(&values);
    return *same[1] - 2;
}
