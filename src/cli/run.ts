import { readFile } from "node:fs/promises";
import { CLI_COMMANDS, type CliCommand, type ToolName } from "../shared/contracts.js";
import { asUnrealBlueprintError, ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "../shared/json.js";
import { withUnrealClient, type UnrealClientOptions } from "../client/index.js";

export interface CliIo {
  readonly stdout: (text: string) => void;
  readonly stderr: (text: string) => void;
  readonly readTextFile: (path: string) => Promise<string>;
}

export interface CliDependencies {
  readonly invoke?: (tool: ToolName, params: JsonObject, options: UnrealClientOptions & { timeoutMs?: number }) => Promise<JsonValue>;
}

interface ParsedArguments {
  readonly command: CliCommand;
  readonly pluginAction?: "install" | "update" | "remove";
  readonly json: boolean;
  readonly input: JsonObject;
  readonly uproject?: string;
  readonly editorSessionId?: string;
  readonly timeoutMs?: number;
}

const HELP = `pi-unreal-blueprint <command> [options]

Commands:
  setup | doctor | status | search | inspect | capabilities
  validate | apply | job | verify | history | restore
  plugin install|update|remove

Options:
  --input <json|@file>   Command parameters as one JSON object
  --uproject <path>      Match the exact .uproject session
  --session <id>         Select an exact editorSessionId
  --timeout <ms>         RPC timeout
  --json                 Stable machine-readable output
  --help                  Show this help
`;

function requireValue(argv: readonly string[], index: number, flag: string): string {
  const value = argv[index + 1];
  if (value === undefined || value.startsWith("--")) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${flag} requires a value`);
  }
  return value;
}

async function parseInput(raw: string | undefined, io: CliIo): Promise<JsonObject> {
  if (raw === undefined) return {};
  const text = raw.startsWith("@") ? await io.readTextFile(raw.slice(1)) : raw;
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch (error) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--input must be valid JSON or @file", { cause: error });
  }
  assertJsonValue(value);
  if (!isJsonObject(value)) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--input root must be a JSON object");
  return value;
}

async function parseArguments(argv: readonly string[], io: CliIo): Promise<ParsedArguments | "help"> {
  if (argv.length === 0 || argv.includes("--help") || argv.includes("-h")) return "help";
  const commandValue = argv[0];
  if (commandValue === undefined || !CLI_COMMANDS.includes(commandValue as CliCommand)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `Unknown command: ${commandValue ?? ""}`);
  }
  const command = commandValue as CliCommand;
  let index = 1;
  let pluginAction: ParsedArguments["pluginAction"];
  if (command === "plugin") {
    const action = argv[index++];
    if (action !== "install" && action !== "update" && action !== "remove") {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "plugin requires install, update, or remove");
    }
    pluginAction = action;
  }
  let json = false;
  let rawInput: string | undefined;
  let uproject: string | undefined;
  let editorSessionId: string | undefined;
  let timeoutMs: number | undefined;
  while (index < argv.length) {
    const flag = argv[index];
    switch (flag) {
      case "--json":
        json = true;
        index += 1;
        break;
      case "--input":
        rawInput = requireValue(argv, index, flag);
        index += 2;
        break;
      case "--uproject":
        uproject = requireValue(argv, index, flag);
        index += 2;
        break;
      case "--session":
        editorSessionId = requireValue(argv, index, flag);
        index += 2;
        break;
      case "--timeout": {
        const rawTimeout = requireValue(argv, index, flag);
        timeoutMs = Number(rawTimeout);
        if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0 || timeoutMs > 600_000) {
          throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--timeout must be an integer from 1 to 600000");
        }
        index += 2;
        break;
      }
      default:
        throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `Unknown argument: ${flag ?? ""}`);
    }
  }
  return {
    command,
    ...(pluginAction === undefined ? {} : { pluginAction }),
    json,
    input: await parseInput(rawInput, io),
    ...(uproject === undefined ? {} : { uproject }),
    ...(editorSessionId === undefined ? {} : { editorSessionId }),
    ...(timeoutMs === undefined ? {} : { timeoutMs })
  };
}

function commandToInvocation(parsed: ParsedArguments): { tool: ToolName; params: JsonObject } {
  switch (parsed.command) {
    case "doctor": return { tool: "unreal_doctor", params: parsed.input };
    case "status": return { tool: "unreal_status", params: parsed.input };
    case "search": return { tool: "unreal_search", params: parsed.input };
    case "inspect": return { tool: "blueprint_inspect", params: parsed.input };
    case "capabilities": return { tool: "blueprint_capabilities", params: parsed.input };
    case "validate": return { tool: "blueprint_validate", params: parsed.input };
    case "apply": return { tool: "blueprint_apply", params: parsed.input };
    case "job": return { tool: "blueprint_job", params: parsed.input };
    case "verify": return { tool: "blueprint_verify", params: parsed.input };
    case "history": return { tool: "blueprint_history", params: { action: "list", ...parsed.input } };
    case "restore": return { tool: "blueprint_history", params: { ...parsed.input, action: "restore" } };
    case "setup":
      throw new UnrealBlueprintError(
        ERROR_CODES.SETUP_UNAVAILABLE,
        "setup is not available in this TypeScript baseline; no UE plugin installation was performed"
      );
    case "plugin":
      throw new UnrealBlueprintError(
        ERROR_CODES.SETUP_UNAVAILABLE,
        `plugin ${parsed.pluginAction ?? ""} is not available in this TypeScript baseline; no files were changed`
      );
  }
}

async function defaultInvoke(tool: ToolName, params: JsonObject, options: UnrealClientOptions & { timeoutMs?: number }): Promise<JsonValue> {
  return withUnrealClient(options, (client) => client.invoke(
    tool,
    params,
    options.timeoutMs === undefined ? {} : { timeoutMs: options.timeoutMs }
  ));
}

function writeSuccess(io: CliIo, parsed: ParsedArguments, result: JsonValue): void {
  if (parsed.json) {
    io.stdout(`${JSON.stringify({ ok: true, command: parsed.command, result })}\n`);
    return;
  }
  io.stdout(`${parsed.command}: success / 成功\n${JSON.stringify(result, null, 2)}\n`);
}

function writeFailure(io: CliIo, json: boolean, error: unknown): void {
  const normalized = asUnrealBlueprintError(error);
  if (json) {
    io.stdout(`${JSON.stringify({ ok: false, error: normalized.toJSON() })}\n`);
    return;
  }
  io.stderr(`Error / 错误 [${normalized.code}]: ${normalized.message}\n`);
}

export async function runCli(argv: readonly string[], io: CliIo, dependencies: CliDependencies = {}): Promise<number> {
  let json = argv.includes("--json");
  try {
    const parsed = await parseArguments(argv, io);
    if (parsed === "help") {
      io.stdout(HELP);
      return 0;
    }
    json = parsed.json;
    const invocation = commandToInvocation(parsed);
    const session = {
      ...(parsed.editorSessionId === undefined ? {} : { editorSessionId: parsed.editorSessionId }),
      ...(parsed.uproject === undefined ? {} : { uproject: parsed.uproject })
    };
    const invoke = dependencies.invoke ?? defaultInvoke;
    const result = await invoke(invocation.tool, invocation.params, {
      session,
      ...(parsed.timeoutMs === undefined ? {} : { timeoutMs: parsed.timeoutMs })
    });
    writeSuccess(io, parsed, result);
    return 0;
  } catch (error) {
    writeFailure(io, json, error);
    return 1;
  }
}

export const nodeCliIo: CliIo = {
  stdout: (text) => process.stdout.write(text),
  stderr: (text) => process.stderr.write(text),
  readTextFile: (path) => readFile(path, "utf8")
};
