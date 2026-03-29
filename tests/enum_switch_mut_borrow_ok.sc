enum BoxedInt {
    Value(int),
};

int set_and_read(BoxedInt value) {
    switch (&mut value) {
        case Value(payload):
            *payload = 41;
    }

    switch (&value) {
        case Value(payload):
            return *payload;
    }
}
