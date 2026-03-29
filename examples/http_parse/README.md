# HTTP Parse Example

This example starts a tiny loopback HTTP server in Safe C, receives one real
socket request, parses it into zero-copy slice-backed views, prints the parsed
structure with `println`, sends a `200 OK`, then exits.

## Build

```sh
./build/sc examples/http_parse/main.sc -o /tmp/http_parse.o
cc /tmp/http_parse.o \
  examples/http_parse/println.c \
  examples/http_parse/socket_ffi.c \
  -o /tmp/http_parse
```

## Run

Start the server in one terminal:

```sh
/tmp/http_parse
```

In another terminal, send a request:

```sh
python3 examples/http_parse/send_http.py
```

The server prints the parsed request fields to stdout and the Python script
prints the HTTP response.
