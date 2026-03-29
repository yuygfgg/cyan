enum Resource {
    Raw(int)
};

impl drop(&mut Resource value) {
    return;
}

int stop_early() {
    while (true) {
        Resource value = Raw(3);
        break;
    }

    return 0;
}
