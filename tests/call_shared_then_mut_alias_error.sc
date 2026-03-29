void bump(&int before, &mut int target) {
    *target = *before + 1;
}

int main() {
    int value = 0;
    &mut int alias = &mut value;
    bump(alias, alias);
    return value;
}
