#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int write_all(int fd, const char *text, int len) {
    int sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, text + sent, (size_t)(len - sent), 0);
        if (n <= 0) {
            return -1;
        }
        sent += (int)n;
    }
    return sent;
}

int http_listen_loopback(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    const int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int http_accept_client(int server_fd) { return accept(server_fd, NULL, NULL); }

int http_recv_bytes(int fd, char *out, int capacity) {
    return (int)recv(fd, out, (size_t)capacity, 0);
}

int http_send_cstring(int fd, const char *text) {
    return write_all(fd, text, (int)strlen(text));
}

void http_close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
