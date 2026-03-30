interface<T> int span_len(&T value);

impl span_len(&[]const char value) {
    return len(*value);
}

struct Request {
    []const char path;
};

int sum([]&span_len values) {
    int i = 0;
    int total = 0;
    while (i < len(values)) {
        total = total + span_len(values[i]);
        i++;
    }
    return total;
}

int main() {
    []const char root = subslice("hello", 0, 5);
    Request request = {subslice("abc", 0, 3)};
    if (span_len(root) != 5) {
        return 1;
    }
    if (span_len(request.path) != 3) {
        return 2;
    }
    if (sum([root, request.path]) != 8) {
        return 3;
    }
    return 0;
}
