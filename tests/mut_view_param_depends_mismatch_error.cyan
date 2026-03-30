struct View {
    []const char data;
};

void overwrite_wrong(&mut View v, []const char expected, []const char actual)
    depends(v.data on expected) {
    v.data = actual;
}

int main() {
    return 0;
}
