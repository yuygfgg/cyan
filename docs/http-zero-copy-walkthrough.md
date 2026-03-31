# Zero-Copy HTTP Walkthrough

This document is a close reading of the repository's zero-copy HTTP request example. It shows what Cyan's features feel like when they are all pointed at one concrete problem: take a real HTTP request, keep it in one buffer, and still expose a structured result for method, path, headers, and body.

If the main tutorial explains Cyan’s vocabulary, this walkthrough explains Cyan’s accent.

## 1. Why Zero-Copy Parsing Is Hard In The First Place

Zero-copy parsing sounds simple when stated casually. “Just avoid allocating new strings.” In practice, that sentence hides three separate jobs.

First, you need one owner for the bytes. That part is easy enough. A stack array or heap buffer can own the request.

Second, you need a convenient representation for “the method is bytes 0 through 3,” “the path is bytes 5 through 11,” and “the body starts after the blank line.” That is what slices are for.

Third, and this is the part that languages often leave to discipline, you need to prove that all of those views still point into live storage. In C, this proof usually lives in comments and social rules. In Cyan, that proof is part of the function signatures.

That example matters because it solves all three jobs at once.

## 2. The Actual Request Bytes

Before touching the Cyan source, look at the companion C fixture. It contains the exact request used by the example:

```text
POST /submit?lang=safe&mode=zero HTTP/1.1\r\n
Host: example.test\r\n
User-Agent: cyan/0\r\n
Content-Type: text/plain\r\n
Content-Length: 5\r\n
\r\n
hello
```

That one string is the entire world for the parser. The Cyan code never needs to allocate a second copy of `POST`, `/submit`, `example.test`, or `hello`. It only needs to remember where each interesting region begins and how long it is.

The easiest mental picture is this:

```text
owner buffer
┌──────────────────────────────────────────────────────────────────────────────┐
│ POST /submit?lang=safe&mode=zero HTTP/1.1\r\nHost: ... \r\n\r\nhello       │
└──────────────────────────────────────────────────────────────────────────────┘
  └── method ──┘
       └────────────── target.raw ──────────────┘
       └─ path ─┘ └────── query ──────┘
                                                          └ body ┘
```

The whole parser is a disciplined way of building those windows.

## 3. Data Shapes First, Parsing Second

The file starts with plain structs:

```cyan
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
    i64 parsed_count;
};

struct HttpRequest {
    RequestLine line;
    HeaderBag headers;
    []const char body;
    i64 content_length_value;
};
```

It is worth slowing down here. These structs are ordinary data shapes. The interesting decision is the field types. Every textual field is `[]const char`. The result object is a tree of views over one source buffer.

This is a very Cyan move. The language does not force you into a custom parsing framework. It lets ordinary user-defined data types carry precisely the kinds of references you need.

## 4. Why The File Builds Empty Views First

The next helpers look repetitive until you understand their purpose:

```cyan
[]const char empty_span([]const char bytes) depends(return on bytes) {
    return subslice(bytes, 0, 0);
}

HttpHeader empty_header([]const char bytes)
    depends(return on bytes) {
    []const char empty = empty_span(bytes);
    return {empty, empty};
}

RequestTarget empty_target([]const char bytes)
    depends(return on bytes) {
    []const char empty = empty_span(bytes);
    return {empty, empty, empty};
}

RequestLine empty_line([]const char bytes)
    depends(return on bytes) {
    []const char empty = empty_span(bytes);
    RequestTarget target = empty_target(bytes);
    return {empty, target, empty};
}

HttpRequest blank_request([]const char bytes)
    depends(return on bytes) {
    HttpHeader empty = empty_header(bytes);
    RequestLine line = empty_line(bytes);
    HeaderBag headers = {empty, empty, empty, empty, 0};
    return {line, headers, empty_span(bytes), 0};
}
```

The parser uses valid empty views.

`empty_span(bytes)` returns `subslice(bytes, 0, 0)`. That is an empty slice, but it is still a real slice derived from `bytes`. Once you have that, you can build an empty `HttpHeader`, an empty `RequestTarget`, an empty `RequestLine`, and finally a fully initialized `HttpRequest` whose view fields are all valid, even though they point at empty ranges.

This is a subtle design choice with a big payoff. It means `parse_http_request` can always start from a valid `HttpRequest` shape. It never has to juggle “initialized versus uninitialized” string fields, and it never needs a second representation for “nothing here yet.”

The `depends(return on bytes)` annotation on every helper matters just as much as the bodies. It tells the compiler that these empty views still come from the source slice `bytes`.

## 5. The Scanner Helpers Are Deliberately Ordinary

Before the parser starts producing structured results, it defines a few low-level search helpers:

```cyan
bool span_eq([]const char left, []const char right) {
    if (len(left) != len(right)) {
        return false;
    }

    i64 i = 0;
    while (i < len(left)) {
        if (left[i] != right[i]) {
            return false;
        }
        i++;
    }
    return true;
}

i64 find_byte_in_range([]const char bytes, i64 start, i64 end, char needle) {
    i64 i = start;
    while (i < end) {
        if (bytes[i] == needle) {
            return i;
        }
        i++;
    }
    return -1;
}

i64 find_crlf([]const char bytes, i64 start) {
    i64 i = start;
    while (i + 1 < len(bytes)) {
        if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
            return i;
        }
        i++;
    }
    return -1;
}
```

These helpers are intentionally plain. That matters for two reasons.

First, it shows that Cyan’s special sauce lives in plain code. The low-level mechanics are just loops and indexing.

Second, it keeps the real parsing logic readable. Once `find_byte_in_range` exists, `parse_header_line` can talk about colons and bounds instead of about character-by-character loop scaffolding.

There is also a design lesson here. The file uses slices for the input and stays close to bytes and offsets.

## 6. Parsing The Target Without Copying

The request target is the first place where the code returns several related views at once:

```cyan
RequestTarget parse_target([]const char bytes, i64 start, i64 end)
    depends(return on bytes) {
    []const char raw = subslice(bytes, start, end - start);
    i64 query_mark = find_byte_in_range(bytes, start, end, '?');
    if (query_mark < 0) {
        return {raw, raw, empty_span(bytes)};
    }
    []const char path = subslice(bytes, start, query_mark - start);
    []const char query =
        subslice(bytes, query_mark + 1, end - (query_mark + 1));
    return {raw, path, query};
}
```

Read the function as if you were doing the job by hand with a pencil.

`raw` is the whole target span. For the test request, that means `/submit?lang=safe&mode=zero`.

Then the parser searches for `?`. If it does not exist, the path and raw target are the same thing and the query is empty. That is why the no-query branch returns `{raw, raw, empty_span(bytes)}`.

If `?` does exist, the code builds two more views. `path` covers everything before the question mark. `query` covers everything after it. Again, nothing is copied. The parser is just describing byte ranges.

The crucial part is still the signature: `depends(return on bytes)`. A `RequestTarget` contains three slice fields, and the compiler needs to know where each one came from. The shorthand says they all derive from `bytes`.

## 7. Parsing One Header Line

Now look at the most compact and most revealing helper in the file:

```cyan
HttpHeader parse_header_line([]const char bytes, i64 line_start, i64 line_end)
    depends(return on bytes) {
    i64 colon = find_byte_in_range(bytes, line_start, line_end, ':');
    []const char name = subslice(bytes, line_start, colon - line_start);
    i64 value_start = colon + 1;
    if (value_start < line_end && bytes[value_start] == ' ') {
        value_start++;
    }
    []const char value = subslice(bytes, value_start, line_end - value_start);
    return {name, value};
}
```

This function is where Cyan’s style becomes extremely concrete.

`colon` is just an index. The parser finds the split point for the header line.

`name` is the left slice. `value_start = colon + 1;` moves to the byte after the colon. The optional-space branch handles the common HTTP style `Header: value`.

Then `value` becomes the right slice. The final `return {name, value};` packages both windows into a struct.

If you have only written parsers in high-level languages that hand you substrings or tokens, this is the moment to pause. Cyan is asking you to think in offsets and views, and it does so with ordinary expressions and an explicit provenance contract. The file stays free of scattered raw pointer math.

## 8. Building The Request Line

The request line is assembled by one more helper:

```cyan
RequestLine build_request_line([]const char bytes,
                               i64 method_end,
                               i64 target_end,
                               i64 line_end)
    depends(return on bytes) {
    []const char method = subslice(bytes, 0, method_end);
    RequestTarget target = parse_target(bytes, method_end + 1, target_end);
    []const char version =
        subslice(bytes, target_end + 1, line_end - (target_end + 1));
    return {method, target, version};
}
```

The test request line is:

```text
POST /submit?lang=safe&mode=zero HTTP/1.1
```

`method_end` points at the first space. `target_end` points at the second space. `line_end` points at the `\r` before the newline. Once those three indices exist, the actual slicing is straightforward.

`method` becomes `POST`.

`target` is delegated to `parse_target`, which is good design. The request-line parser only needs the boundary where the target ends.

`version` becomes `HTTP/1.1`.

The helper is short because earlier code already separated “find the boundaries” from “cut the views.” That separation is one reason the whole file stays readable.

## 9. The Heart Of The File: `parse_http_request`

Here is the top half of the main parser:

```cyan
bool parse_http_request([]const char bytes, &mut HttpRequest out)
    depends(out on bytes) {
    *out = blank_request(bytes);
    if (len(bytes) == 0) {
        return false;
    }

    i64 line_end = find_crlf(bytes, 0);
    if (line_end < 0) {
        return false;
    }

    i64 method_end = find_byte_in_range(bytes, 0, line_end, ' ');
    if (method_end < 0) {
        return false;
    }

    i64 target_end = find_byte_in_range(bytes, method_end + 1, line_end, ' ');
    if (target_end < 0) {
        return false;
    }

    RequestLine line = build_request_line(bytes, method_end, target_end,
                                          line_end);
```

The first line of the signature is the parser’s contract. `[]const char bytes` is the source request. `&mut HttpRequest out` is the output object that will be filled in place. `depends(out on bytes)` tells the compiler that the views stored inside `out` are all derived from `bytes` unless later code says otherwise.

`*out = blank_request(bytes);` is not defensive fluff. It establishes a valid baseline request shape whose internal slices already derive from `bytes`. That means every later overwrite is a refinement of a known-good state.

The early `false` returns handle malformed input incrementally. Empty request? Reject. No end-of-line marker? Reject. Missing space between method and target? Reject. Missing space between target and version? Reject.

Only after those structural boundaries exist does the parser call `build_request_line(...)`. This ordering keeps the later code from having to wonder whether indices are valid.

Now the second half:

```cyan
    HttpHeader host = empty_header(bytes);
    HttpHeader user_agent = empty_header(bytes);
    HttpHeader content_type = empty_header(bytes);
    HttpHeader content_length = empty_header(bytes);
    i64 parsed_count = 0;
    i64 content_length_value = 0;

    i64 cursor = line_end + 2;
    while (cursor < len(bytes)) {
        i64 header_end = find_crlf(bytes, cursor);
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
```

The four header locals start as empty header views derived from `bytes`. This is the same initialization pattern you saw earlier, now used to accumulate results through the loop.

`cursor = line_end + 2` skips the request-line CRLF. From there, each pass through the loop finds the next header line ending.

The branch `if (header_end == cursor)` is the blank-line check. In HTTP, the blank line marks the end of the headers. Once that happens, the body begins at `cursor + 2`. The code slices the remainder of `bytes` as `body`, packages the collected headers into `HeaderBag`, writes the final structured result to `out`, and returns `true`.

If the line is not blank, the parser slices one header with `parse_header_line(...)`, increments `parsed_count`, and then classifies the header by name. Matching headers are stored in dedicated locals. `Content-Length` gets one extra step: the parser converts the value slice into an integer with `parse_decimal(...)`.

At the bottom of the loop, `cursor = header_end + 2` advances past the CRLF and continues scanning.

Notice how little memory management code appears in this function. The parser is busy with protocol structure, not with allocation bookkeeping. That is the practical value of “one owner, many views.”

## 10. The Entry Point Makes The Ownership Story Obvious

Here is the top-level `main()` from the test:

```cyan
i64 main() {
    char[256] buffer;
    i64 byte_count;
    unchecked {
        byte_count = mock_http_read(&mut buffer[0], len(buffer));
    }
    if (byte_count <= 0) {
        return 1;
    }

    []const char bytes = subslice(buffer, 0, byte_count);
    HttpRequest parsed = blank_request(bytes);
    if (!parse_http_request(bytes, &mut parsed)) {
        return 2;
    }
```

This is the ownership story in its cleanest form.

`char[256] buffer;` is the owner.

`mock_http_read(&mut buffer[0], len(buffer));` is the FFI boundary. It sits in `unchecked` because it crosses into raw memory and C.

`[]const char bytes = subslice(buffer, 0, byte_count);` is the first borrowed view.

`HttpRequest parsed = blank_request(bytes);` creates a valid structured placeholder whose views are all initially empty but still tied to `bytes`.

`parse_http_request(bytes, &mut parsed)` rewrites that placeholder into the real parsed result.

Everything the test checks after that is a proof that the views point at the expected places:

```cyan
if (!span_eq(parsed.line.method, subslice("POST", 0, 4))) {
    return 3;
}
if (!span_eq(parsed.line.target.path, subslice("/submit", 0, 7))) {
    return 5;
}
if (!span_eq(parsed.headers.host.value,
             subslice("example.test", 0, 12))) {
    return 8;
}
if (!span_eq(parsed.body, subslice("hello", 0, 5))) {
    return 14;
}
```

The structured fields are useful precisely because they still point at live bytes.

## 11. Failure Paths Are Part Of The Design

One easy way to misread this test is to treat the `return false;` branches as dull guard clauses. They are more important than that. They show where the parser refuses to claim structure it cannot justify.

If there is no CRLF, the request line is incomplete. If there is no first space, there is no method/target split. If there is no second space, there is no target/version split. If a header line has no CRLF terminator, the parser refuses to keep going. If the blank line is missing, the body start is unknown.

That behavior matters because zero-copy parsing magnifies structural mistakes. Once you decide that a field is a view into the original buffer, a wrong boundary means the structured result is literally looking at the wrong bytes.

The file keeps that risk under control by computing boundaries first and only then slicing.

## 12. What The Socket Example Adds

The repository also contains `examples/http_parse/main.cyan`, which uses the same parsing ideas against a real loopback socket. Two additions are worth noticing there.

First, the example computes an expected request size before trying to parse. That lets it distinguish “incomplete read” from “malformed request.”

Second, once parsing succeeds, it prints the structured fields with `/std.println`:

```cyan
println("method={} target={} version={}",
        [request.line.method, request.line.target.raw, request.line.version]);
println("path={} query={}",
        [request.line.target.path, request.line.target.query]);
println("host={} user-agent={}",
        [request.headers.host.value, request.headers.user_agent.value]);
println("body={}", [request.body]);
```

That is a nice example of Cyan’s pieces reinforcing each other. The parser produces slice-backed views. The printing library accepts slice-backed values through its formatting interface. No extra string copying is needed just to display the result.

## 13. What To Remember After Reading This File

The core lesson of this example is that Cyan's distinctive features work best together.

Slices make zero-copy views ergonomic.

`depends(...)` makes the provenance of those views explicit and checkable.

Ordinary structs let the parsed result stay readable.

`unchecked` keeps the FFI boundary explicit instead of leaking raw memory rules across the whole parser.

Once those pieces line up, the code reads like a straightforward parser again. That is the best outcome a systems language can hope for. The hard parts are still there, but they are named, localized, and checked.
