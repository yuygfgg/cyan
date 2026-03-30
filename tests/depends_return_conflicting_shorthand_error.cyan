struct Pair {
    []const char left;
    []const char right;
};

Pair bad([]const char a, []const char b) depends(return on a, return on b) {
    return {a, a};
}
