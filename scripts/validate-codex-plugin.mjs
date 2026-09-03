import { readFile, stat } from "node:fs/promises";

const fail = (message) => { throw new Error(message); };
const manifest = JSON.parse(await readFile(".codex-plugin/plugin.json", "utf8"));
for (const field of ["name", "version", "description", "skills", "mcpServers"]) {
  if (typeof manifest[field] !== "string" || manifest[field].length === 0) fail(`plugin.json requires ${field}`);
}
if (manifest.name !== "codex-unreal-blueprint") fail("plugin name must be codex-unreal-blueprint");
if (!/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(manifest.version)) fail("plugin version must be SemVer");
for (const field of ["displayName", "shortDescription", "longDescription", "developerName", "category"]) {
  if (typeof manifest.interface?.[field] !== "string" || manifest.interface[field].length === 0) fail(`plugin interface requires ${field}`);
}
if (manifest.interface.category !== "Developer Tools") fail("plugin category must be Developer Tools");
if (JSON.stringify(manifest.interface.capabilities) !== JSON.stringify(["Read", "Write"])) fail("plugin capabilities must be Read and Write");
if (!Array.isArray(manifest.interface.defaultPrompt) || manifest.interface.defaultPrompt.length > 3) fail("defaultPrompt must have at most three entries");
for (const path of [manifest.skills, manifest.mcpServers, "skills/unreal-blueprint/SKILL.md", "dist/mcp/index.js"]) {
  await stat(path).catch(() => fail(`plugin path is missing: ${path}`));
}
const mcp = JSON.parse(await readFile(".mcp.json", "utf8"));
const server = mcp.mcpServers?.["codex-unreal-blueprint"];
if (server?.command !== "node" || server?.args?.[0] !== "dist/mcp/index.js" || server?.tool_timeout_sec !== 620) fail(".mcp.json server contract is invalid");
const skill = await readFile("skills/unreal-blueprint/SKILL.md", "utf8");
if (!skill.startsWith("---\n") || !skill.includes("\nname: unreal-blueprint\n") || !skill.includes("\ndescription:")) fail("Skill frontmatter is invalid");
console.log("Codex plugin and Skill manifests are valid.");
