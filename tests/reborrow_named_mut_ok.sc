int main() {
    int value = 0;
    {
        &mut int first = &value;
        {
            &mut int second = first;
            *second = 1;
        }
        *first = 2;
    }
    return value;
}
