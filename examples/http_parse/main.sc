import println;

extern {
    int http_listen_loopback(int port);
    int http_accept_client(int server_fd);
    int http_recv_bytes(int fd, char* out, int capacity);
    int http_send_cstring(int fd, const char* text);
    void http_close_fd(int fd);
}

struct HttpHeader {
    []const char name;
    []const char value;
};

struct RequestTarget {
    []const char raw;
    []const char path;
    []const char query;
};

struct RequestLine {
    []const char method;
    RequestTarget target;
    []const char version;
};

struct HeaderBag {
    HttpHeader host;
    HttpHeader user_agent;
    HttpHeader content_type;
    HttpHeader content_length;
    int parsed_count;
};

struct HttpRequest {
    RequestLine line;
    HeaderBag headers;
    []const char body;
    int content_length_value;
};

[]const char empty_span([]const char bytes) depends(return on bytes) {
    return subslice(bytes, 0, 0);
}

HttpHeader empty_header([]const char bytes)
    depends(return.name on bytes, return.value on bytes) {
    []const char empty = empty_span(bytes);
    return {empty, empty};
}

RequestTarget empty_target([]const char bytes)
    depends(return.raw on bytes, return.path on bytes, return.query on bytes) {
    []const char empty = empty_span(bytes);
    return {empty, empty, empty};
}

RequestLine empty_line([]const char bytes)
    depends(return.method on bytes,
            return.target.raw on bytes,
            return.target.path on bytes,
            return.target.query on bytes,
            return.version on bytes) {
    []const char empty = empty_span(bytes);
    RequestTarget target = empty_target(bytes);
    return {empty, target, empty};
}

HttpRequest blank_request([]const char bytes)
    depends(return.line.method on bytes,
            return.line.target.raw on bytes,
            return.line.target.path on bytes,
            return.line.target.query on bytes,
            return.line.version on bytes,
            return.headers.host.name on bytes,
            return.headers.host.value on bytes,
            return.headers.user_agent.name on bytes,
            return.headers.user_agent.value on bytes,
            return.headers.content_type.name on bytes,
            return.headers.content_type.value on bytes,
            return.headers.content_length.name on bytes,
            return.headers.content_length.value on bytes,
            return.body on bytes) {
    HttpHeader empty = empty_header(bytes);
    RequestLine line = empty_line(bytes);
    HeaderBag headers = {empty, empty, empty, empty, 0};
    return {line, headers, empty_span(bytes), 0};
}

bool span_eq([]const char left, []const char right) {
    if (len(left) != len(right)) {
        return false;
    }

    int i = 0;
    while (i < len(left)) {
        if (left[i] != right[i]) {
            return false;
        }
        i++;
    }
    return true;
}

int find_byte_in_range([]const char bytes, int start, int end, char needle) {
    int i = start;
    while (i < end) {
        if (bytes[i] == needle) {
            return i;
        }
        i++;
    }
    return -1;
}

int find_crlf([]const char bytes, int start) {
    int i = start;
    while (i + 1 < len(bytes)) {
        if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
            return i;
        }
        i++;
    }
    return -1;
}

int find_double_crlf([]const char bytes, int start) {
    int i = start;
    while (i + 3 < len(bytes)) {
        if (bytes[i] == '\r' &&
            bytes[i + 1] == '\n' &&
            bytes[i + 2] == '\r' &&
            bytes[i + 3] == '\n') {
            return i;
        }
        i++;
    }
    return -1;
}

RequestTarget parse_target([]const char bytes, int start, int end)
    depends(return.raw on bytes, return.path on bytes, return.query on bytes) {
    []const char raw = subslice(bytes, start, end - start);
    int query_mark = find_byte_in_range(bytes, start, end, '?');
    if (query_mark < 0) {
        return {raw, raw, empty_span(bytes)};
    }
    []const char path = subslice(bytes, start, query_mark - start);
    []const char query =
        subslice(bytes, query_mark + 1, end - (query_mark + 1));
    return {raw, path, query};
}

RequestLine build_request_line([]const char bytes,
                               int method_end,
                               int target_end,
                               int line_end)
    depends(return.method on bytes,
            return.target.raw on bytes,
            return.target.path on bytes,
            return.target.query on bytes,
            return.version on bytes) {
    []const char method = subslice(bytes, 0, method_end);
    RequestTarget target = parse_target(bytes, method_end + 1, target_end);
    []const char version =
        subslice(bytes, target_end + 1, line_end - (target_end + 1));
    return {method, target, version};
}

HttpHeader parse_header_line([]const char bytes, int line_start, int line_end)
    depends(return.name on bytes, return.value on bytes) {
    int colon = find_byte_in_range(bytes, line_start, line_end, ':');
    []const char name = subslice(bytes, line_start, colon - line_start);
    int value_start = colon + 1;
    if (value_start < line_end && bytes[value_start] == ' ') {
        value_start++;
    }
    []const char value = subslice(bytes, value_start, line_end - value_start);
    return {name, value};
}

int parse_decimal([]const char digits) {
    int i = 0;
    int value = 0;
    while (i < len(digits)) {
        value = value * 10 + ((digits[i] as int) - ('0' as int));
        i++;
    }
    return value;
}

bool is_host_header([]const char name) {
    return span_eq(name, subslice("Host", 0, 4));
}

bool is_user_agent_header([]const char name) {
    return span_eq(name, subslice("User-Agent", 0, 10));
}

bool is_content_type_header([]const char name) {
    return span_eq(name, subslice("Content-Type", 0, 12));
}

bool is_content_length_header([]const char name) {
    return span_eq(name, subslice("Content-Length", 0, 14));
}

int parse_content_length_header([]const char bytes, int headers_end) {
    int line_end = find_crlf(bytes, 0);
    if (line_end < 0) {
        return -1;
    }

    int cursor = line_end + 2;
    while (cursor < headers_end) {
        int header_end = find_crlf(bytes, cursor);
        if (header_end < 0 || header_end > headers_end) {
            return -1;
        }
        if (header_end == cursor) {
            return 0;
        }

        HttpHeader header = parse_header_line(bytes, cursor, header_end);
        if (is_content_length_header(header.name)) {
            return parse_decimal(header.value);
        }
        cursor = header_end + 2;
    }

    return 0;
}

int expected_request_size([]const char bytes) {
    int headers_end = find_double_crlf(bytes, 0);
    if (headers_end < 0) {
        return 0;
    }

    int content_length = parse_content_length_header(bytes, headers_end);
    if (content_length < 0) {
        return -1;
    }
    return headers_end + 4 + content_length;
}

bool parse_http_request([]const char bytes, &mut HttpRequest out)
    depends(out.line.method on bytes,
            out.line.target.raw on bytes,
            out.line.target.path on bytes,
            out.line.target.query on bytes,
            out.line.version on bytes,
            out.headers.host.name on bytes,
            out.headers.host.value on bytes,
            out.headers.user_agent.name on bytes,
            out.headers.user_agent.value on bytes,
            out.headers.content_type.name on bytes,
            out.headers.content_type.value on bytes,
            out.headers.content_length.name on bytes,
            out.headers.content_length.value on bytes,
            out.body on bytes) {
    *out = blank_request(bytes);
    if (len(bytes) == 0) {
        return false;
    }

    int line_end = find_crlf(bytes, 0);
    if (line_end < 0) {
        return false;
    }

    int method_end = find_byte_in_range(bytes, 0, line_end, ' ');
    if (method_end < 0) {
        return false;
    }

    int target_end = find_byte_in_range(bytes, method_end + 1, line_end, ' ');
    if (target_end < 0) {
        return false;
    }

    RequestLine line = build_request_line(bytes, method_end, target_end,
                                          line_end);

    HttpHeader host = empty_header(bytes);
    HttpHeader user_agent = empty_header(bytes);
    HttpHeader content_type = empty_header(bytes);
    HttpHeader content_length = empty_header(bytes);
    int parsed_count = 0;
    int content_length_value = 0;

    int cursor = line_end + 2;
    while (cursor < len(bytes)) {
        int header_end = find_crlf(bytes, cursor);
        if (header_end < 0) {
            return false;
        }
        if (header_end == cursor) {
            cursor = cursor + 2;
            []const char body = subslice(bytes, cursor, len(bytes) - cursor);
            HeaderBag headers = {host, user_agent, content_type,
                                 content_length, parsed_count};
            *out = {line, headers, body, content_length_value};
            return true;
        }

        HttpHeader header = parse_header_line(bytes, cursor, header_end);
        parsed_count++;
        if (is_host_header(header.name)) {
            host = header;
        } else {
            if (is_user_agent_header(header.name)) {
                user_agent = header;
            } else {
                if (is_content_type_header(header.name)) {
                    content_type = header;
                } else {
                    if (is_content_length_header(header.name)) {
                        content_length = header;
                        content_length_value = parse_decimal(header.value);
                    }
                }
            }
        }
        cursor = header_end + 2;
    }

    return false;
}

void print_request(&HttpRequest request) {
    println("method={} target={} version={}",
            [request.line.method, request.line.target.raw, request.line.version]);
    println("path={} query={}",
            [request.line.target.path, request.line.target.query]);
    println("host={} user-agent={}",
            [request.headers.host.value, request.headers.user_agent.value]);
    println("content-type={} content-length={}",
            [request.headers.content_type.value,
             request.headers.content_length.value]);
    println("parsed-headers={} body-bytes={}",
            [request.headers.parsed_count, request.content_length_value]);
    println("body={}", [request.body]);
}

int main() {
    int port = 18080;
    int server_fd = http_listen_loopback(port);
    if (server_fd < 0) {
        println0("listen failed");
        return 1;
    }

    println("listening on 127.0.0.1:{}", [port]);

    int client_fd = http_accept_client(server_fd);
    if (client_fd < 0) {
        println0("accept failed");
        http_close_fd(server_fd);
        return 2;
    }

    char[4096] buffer;
    int request_size;
    unchecked {
        request_size =
            http_recv_bytes(client_fd, &mut buffer[0], sizeof(char[4096]));
    }
    if (request_size <= 0) {
        println0("read failed");
        http_close_fd(client_fd);
        http_close_fd(server_fd);
        return 3;
    }

    []const char bytes = subslice(buffer, 0, request_size);
    int expected_size = expected_request_size(bytes);
    if (expected_size != request_size) {
        println0("request incomplete");
        http_close_fd(client_fd);
        http_close_fd(server_fd);
        return 4;
    }

    HttpRequest parsed = blank_request(bytes);
    if (!parse_http_request(bytes, &mut parsed)) {
        println0("parse failed");
        http_send_cstring(
            client_fd,
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 5\r\nConnection: close\r\n\r\nerror");
        http_close_fd(client_fd);
        http_close_fd(server_fd);
        return 5;
    }

    print_request(&parsed);
    if (http_send_cstring(
            client_fd,
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok") <
        0) {
        println0("response write failed");
        http_close_fd(client_fd);
        http_close_fd(server_fd);
        return 6;
    }

    http_close_fd(client_fd);
    http_close_fd(server_fd);
    return 0;
}
