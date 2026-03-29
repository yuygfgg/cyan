struct Holder {
    &int value;
};

int main() {
    int fallback = 0;
    Holder holder = {&fallback};
    {
        int inner = 1;
        holder.value = &inner;
    }
    return 0;
}
