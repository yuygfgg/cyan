import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

function candidateExecutableNames(): string[] {
  if (process.platform !== "win32") {
    return ["cyan"];
  }

  const pathExt = process.env.PATHEXT?.split(";").filter(Boolean) ?? [".exe"];
  return [
    "cyan",
    ...pathExt.map((extension) => `cyan${extension.toLowerCase()}`)
  ];
}

function existingExecutableInDirectory(directory: string): string | undefined {
  for (const executableName of candidateExecutableNames()) {
    const candidate = path.join(directory, executableName);
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }

  return undefined;
}

function resolveServerPathFromPath(): string | undefined {
  const pathValue = process.env.PATH;
  if (!pathValue) {
    return undefined;
  }

  const directories = pathValue.split(path.delimiter).filter(Boolean);
  for (const directory of directories) {
    const candidate = existingExecutableInDirectory(directory);
    if (candidate) {
      return candidate;
    }
  }

  return undefined;
}

function resolveConfiguredServerPath(
  output: vscode.OutputChannel
): string | undefined {
  const configuredPath = vscode.workspace
    .getConfiguration("cyan")
    .get<string>("languageServer.path", "")
    .trim();

  if (!configuredPath) {
    return undefined;
  }

  const normalizedPath = path.resolve(configuredPath);
  if (!fs.existsSync(normalizedPath)) {
    output.appendLine(
      `Configured cyan.languageServer.path does not exist: ${normalizedPath}`
    );
    return undefined;
  }

  const stat = fs.statSync(normalizedPath);
  if (stat.isDirectory()) {
    const candidate = existingExecutableInDirectory(normalizedPath);
    if (candidate) {
      return candidate;
    }
    output.appendLine(
      `Configured cyan.languageServer.path is a directory but no cyan executable was found inside it: ${normalizedPath}`
    );
    return undefined;
  }

  return normalizedPath;
}

export async function activate(
  context: vscode.ExtensionContext
): Promise<void> {
  const output = vscode.window.createOutputChannel("Cyan");
  context.subscriptions.push(output);

  const serverPath =
    resolveServerPathFromPath() ?? resolveConfiguredServerPath(output);
  if (!serverPath) {
    output.appendLine(
      "Cyan language server not found in PATH, and cyan.languageServer.path is empty or invalid."
    );
    output.show(true);
    void vscode.window.showErrorMessage(
      "Cyan language server not found. Put `cyan` in PATH, or set cyan.languageServer.path to the executable or its containing directory."
    );
    return;
  }

  output.appendLine(`Using Cyan language server: ${serverPath}`);

  const serverOptions: ServerOptions = {
    command: serverPath,
    args: ["--lsp"]
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "cyan" }],
    synchronize: {
      configurationSection: "cyan"
    },
    outputChannel: output,
    traceOutputChannel: output
  };

  client = new LanguageClient(
    "cyan-language-server",
    "Cyan Language Server",
    serverOptions,
    clientOptions
  );

  context.subscriptions.push(client);
  try {
    await client.start();
    output.appendLine("Cyan language server started.");
  } catch (error) {
    output.appendLine(`Failed to start Cyan language server: ${String(error)}`);
    output.show(true);
    void vscode.window.showErrorMessage(
      `Cyan language server failed to start: ${String(error)}`
    );
  }
}

export async function deactivate(): Promise<void> {
  if (client) {
    await client.stop();
    client = undefined;
  }
}
