import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer, type Server } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { invokeWriteWithRecovery, UnrealClient } from "../../src/client/unreal-client.js";
import { encodeFrame, FrameDecoder } from "../../src/shared/framing.js";
import { parseRpcMessage } from "../../src/shared/protocol.js";

let server: Server | undefined;
let directory: string | undefined;
afterEach(async () => {
  if (server !== undefined) await new Promise<void>((resolve) => server?.close(() => resolve()));
  if (directory !== undefined) await rm(directory, { recursive: true, force: true });
  server = undefined;
  directory = undefined;
});

const snapshot = (phase: "Modify" | "Cancelled") => ({
  jobId: "job-1",
  requestId: "request-1",
  access: "write",
  phase,
  terminal: phase === "Cancelled",
  cancellationRequested: phase === "Cancelled",
  cancellationSafe: phase !== "Modify",
  createdAt: "2026-01-01T00:00:00Z",
  updatedAt: "2026-01-01T00:00:01Z",
  progress: []
});

describe("job client", () => {
  it("gets, waits, cancels, and receives typed progress", async () => {
    server = createServer((socket) => {
      const decoder = new FrameDecoder();
      socket.on("data", (chunk) => {
        for (const value of decoder.push(chunk)) {
          const request = parseRpcMessage(value);
          if (!("method" in request) || !("id" in request)) continue;
          if (request.method === "session.authenticate") {
            socket.write(encodeFrame({ jsonrpc: "2.0", id: request.id, result: { authenticated: true, protocolVersion: "1.0.0" } }));
            continue;
          }
          const action = request.params?.action;
          if (request.method === "blueprint.request") {
            socket.write(encodeFrame({ jsonrpc: "2.0", id: request.id, result: action === "status" ? {
              healthy: true, directory: "E:/Saved/CodexUnrealBlueprint", recordCount: 1, accepted: 0, running: 0, terminal: 1
            } : {
              version: 1, requestId: "request-1", method: "blueprint.apply", paramsHash: "abc", jobId: "job-1",
              state: "terminal", acceptedAt: "2026-01-01T00:00:00Z", updatedAt: "2026-01-01T00:00:01Z",
              interrupted: false, recoveryRequired: false, result: { changed: true }
            } }));
            continue;
          }
          if (action === "query") socket.write(encodeFrame({ jsonrpc: "2.0", method: "blueprint.job.progress", params: {
            jobId: "job-1", phase: "Modify", completed: 1, total: 2, message: "asset", timestamp: "2026-01-01T00:00:01Z"
          } }));
          socket.write(encodeFrame({ jsonrpc: "2.0", id: request.id, result: snapshot(action === "cancel" ? "Cancelled" : "Modify") }));
        }
      });
    });
    await new Promise<void>((resolve) => server?.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (address === null || typeof address === "string") throw new Error("missing port");
    directory = await mkdtemp(join(tmpdir(), "pi-ubp-job-"));
    await writeFile(join(directory, "session.json"), JSON.stringify({
      editorSessionId: "jobs", pid: process.pid, uproject: "E:/Fixture.uproject", engineVersion: "4.27.2",
      host: "127.0.0.1", port: address.port, authToken: "token", pluginVersion: "0.1.0",
      protocolVersion: "1.0.0", capabilities: {}, startedAt: new Date().toISOString()
    }), "utf8");

    const client = await UnrealClient.connect({ session: { editorSessionId: "jobs" }, discovery: { sessionsDirectory: directory, isProcessAlive: () => true } });
    try {
      const progress = new Promise<string>((resolve) => client.onJobProgress((item) => resolve(item.phase)));
      await expect(client.getJob("job-1")).resolves.toMatchObject({ phase: "Modify", cancellationSafe: false });
      await expect(client.waitJob("job-1", { timeoutMs: 10 })).resolves.toMatchObject({ phase: "Modify" });
      await expect(client.cancelJob("job-1")).resolves.toMatchObject({ phase: "Cancelled", terminal: true });
      await expect(client.queryWriteRequest("request-1")).resolves.toMatchObject({ state: "terminal", result: { changed: true } });
      await expect(client.getRequestJournalStatus()).resolves.toMatchObject({ healthy: true, terminal: 1 });
      await expect(progress).resolves.toBe("Modify");
      await expect(client.waitJob("job-1", { timeoutMs: 600_001 })).rejects.toMatchObject({ code: "INVALID_ARGUMENT" });
    } finally {
      client.close();
    }
  });

  it("queries the journal instead of replaying after a lost write response", async () => {
    let applyCount = 0;
    server = createServer((socket) => {
      const decoder = new FrameDecoder();
      socket.on("data", (chunk) => {
        for (const value of decoder.push(chunk)) {
          const request = parseRpcMessage(value);
          if (!("method" in request) || !("id" in request)) continue;
          if (request.method === "session.authenticate") {
            socket.write(encodeFrame({ jsonrpc: "2.0", id: request.id, result: { authenticated: true, protocolVersion: "1.0.0" } }));
          } else if (request.method === "blueprint.apply") {
            applyCount += 1;
            socket.destroy();
          } else if (request.method === "blueprint.request") {
            socket.write(encodeFrame({ jsonrpc: "2.0", id: request.id, result: {
              version: 1, requestId: "lost-response", method: "blueprint.apply", paramsHash: "hash", jobId: "job-lost",
              state: "terminal", acceptedAt: "2026-01-01T00:00:00Z", updatedAt: "2026-01-01T00:00:01Z",
              interrupted: false, recoveryRequired: false, result: { changed: true }
            } }));
          }
        }
      });
    });
    await new Promise<void>((resolve) => server?.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (address === null || typeof address === "string") throw new Error("missing port");
    directory = await mkdtemp(join(tmpdir(), "pi-ubp-recovery-"));
    await writeFile(join(directory, "session.json"), JSON.stringify({
      editorSessionId: "recovery", pid: process.pid, uproject: "E:/Fixture.uproject", engineVersion: "4.27.2",
      host: "127.0.0.1", port: address.port, authToken: "token", pluginVersion: "0.1.0",
      protocolVersion: "1.0.0", capabilities: {}, startedAt: new Date().toISOString()
    }), "utf8");

    await expect(invokeWriteWithRecovery(
      { session: { editorSessionId: "recovery" }, discovery: { sessionsDirectory: directory, isProcessAlive: () => true } },
      "blueprint_apply",
      { requestId: "lost-response", operations: [] },
      { queryAttempts: 2, queryDelayMs: 1 }
    )).resolves.toMatchObject({ state: "terminal", result: { changed: true } });
    expect(applyCount).toBe(1);
  });
});
