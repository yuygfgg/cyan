int invalid_break_state() {
    int value = 0;
    &mut int borrow = &value;

    while (true) {
        &mut int again = borrow;
        break;
    }

    return 0;
}
