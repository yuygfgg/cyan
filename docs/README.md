# Cyan Docs

Read the documentation in this directory beside the repository.

- [Cyan Tutorial](cyan-tutorial.md) is the main guide. It starts from a tiny program, then spends most of its time on slices, borrows, `switch`, `depends(...)`, interfaces, `unchecked`, `/std.view`, `/std.println`, `/std.atomic`, `/std.sync`, `/std.thread`, `/std.abi`, and `/std.ptr`.
- [Stdlib Reference](stdlib-reference.md) lists every builtin module under `/std.*`, its public exports, and the useful ways to use it.
- [Zero-Copy HTTP Walkthrough](http-zero-copy-walkthrough.md) is the companion deep dive into the repository's zero-copy HTTP example and the socket-based variant in `examples/http_parse/`.

The standalone teaching files live in `docs/snippets/`. Longer excerpts in the prose are adapted from repository examples and stdlib code.

If you want to sanity-check the snippets while reading, a good starting loop is:

```sh
cyan docs/snippets/hello_cyan.cyan --check
cyan docs/snippets/core_syntax.cyan --check
cyan docs/snippets/depends_and_views.cyan --check
cyan docs/snippets/interfaces.cyan --check
cyan docs/snippets/custom_fmt.cyan --check
cyan docs/snippets/drop_and_unchecked.cyan --check
cyan docs/snippets/null_ptrs.cyan --check
```
