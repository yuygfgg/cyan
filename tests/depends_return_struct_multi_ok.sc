struct PairRefs {
    &int left;
    &int right;
};

PairRefs pair_refs(&int x, &int y)
    depends(return.left on x, return.right on y) {
    return {x, y};
}

int read(&int value) {
    return *value;
}

int main() {
    int left = 3;
    int right = 7;
    PairRefs refs = pair_refs(&left, &right);
    if (read(refs.left) != 3) {
        return 1;
    }
    if (read(refs.right) != 7) {
        return 2;
    }
    return 0;
}
