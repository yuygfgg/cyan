#!/usr/bin/env python3

from __future__ import annotations

import socket
import sys
import time


REQUEST = (
    b"POST /submit?lang=safe&mode=zero HTTP/1.1\r\n"
    b"Host: example.test\r\n"
    b"User-Agent: python-client/1\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: 5\r\n"
    b"\r\n"
    b"hello"
)


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    deadline = time.time() + 5.0
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5) as sock:
                sock.sendall(REQUEST)
                sock.shutdown(socket.SHUT_WR)
                chunks: list[bytes] = []
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    chunks.append(chunk)
                sys.stdout.write(b"".join(chunks).decode("utf-8", errors="replace"))
                return 0
        except OSError:
            if time.time() >= deadline:
                raise
            time.sleep(0.05)


if __name__ == "__main__":
    raise SystemExit(main())
