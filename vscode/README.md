# Cyan VSCode Client

This directory contains a minimal VSCode TypeScript extension that launches the
local `cyan --lsp` language server.

## Quick start

1. Build the compiler:

   ```sh
   cmake --build ../build
   ```

2. Install the extension dependencies:

   ```sh
   npm install
   ```

3. Compile the extension:

   ```sh
   npm run compile
   ```

4. Open this `vscode/` directory in VSCode and press `F5`.

## Package the extension

From the repo root:

```sh
make package-vscode
```

That script rebuilds `cyan`, compiles the VSCode extension, and regenerates
`vscode/cyan-vscode-0.0.1.vsix`.

The extension first looks for `cyan` in `PATH`. If it is not present, it falls
back to `cyan.languageServer.path`.

`cyan.languageServer.path` may point to either:

- the `cyan` executable itself
- a directory containing the `cyan` executable

If the extension still does not react, open the `Cyan` output panel in
VSCode. Startup and path-resolution errors are written there now.
