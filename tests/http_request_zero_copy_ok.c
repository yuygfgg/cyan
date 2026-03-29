#include <string.h>

int mock_http_read(char *out, int capacity) {
    static const char kRequest[] =
        "POST /submit?lang=safe&mode=zero HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "User-Agent: safe-c/0\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    const int size = (int)(sizeof(kRequest) - 1);
    if (capacity < size) {
        return -1;
    }
    memcpy(out, kRequest, (size_t)size);
    return size;
}
