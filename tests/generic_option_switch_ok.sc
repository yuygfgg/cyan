enum Option<T> {
    None,
    Some(T),
};

int main() {
    Option<int> value = Some(9);
    switch (move value) {
        case None:
            return 0;
        case Some(payload):
            return payload;
    }
}
