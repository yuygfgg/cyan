extern {
    char* take_chars(char* value);
    float pass_float(float value);
}

int main() {
    return sizeof(char) + sizeof(float);
}
