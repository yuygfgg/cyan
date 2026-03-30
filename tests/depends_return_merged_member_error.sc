struct Box {
    int value;
};

&int choose(&Box a, &Box b, bool cond) depends(return on a) {
    &Box x = a;
    if (cond) {
        x = a;
    } else {
        x = b;
    }
    return &x.value;
}

int main() {
    Box a = {1};
    Box b = {2};
    return *choose(&a, &b, false);
}
