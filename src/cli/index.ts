#!/usr/bin/env node
import { pathToFileURL } from "node:url";
import { nodeCliIo, runCli } from "./run.js";

export { runCli } from "./run.js";

const entry = process.argv[1];
if (entry !== undefined && import.meta.url === pathToFileURL(entry).href) {
  process.exitCode = await runCli(process.argv.slice(2), nodeCliIo);
}
