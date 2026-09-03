import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer, type Server, type Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { RpcClient } from "../../src/client/rpc-client.js";
import { UnrealClient } from "../../src/client/unreal-client.js";
import type { EditorSession } from "../../src/client/session-discovery.js";
import { encodeFrame, FrameDecoder } from "../../src/shared/framing.js";
import { parseRpcMessage } from "../../src/shared/protocol.js";

let server: Server | undefined;
let client: RpcClient | undefined;
afterEach(async () => {
  client?.close();
  client = undefined;
  if (server !== undefined) await new Promise<void>((resolve) => server?.close(() => resolve()));
  server = undefined;
});

function session(port: number): EditorSession {
  return {
    editorSessionId: "editor-test",
    pid: process.pid,
    uproject: "E:/Master/LuaSocial.uproject",
    engineVersion: "4.27.2",
    host: "127.0.0.1",
    port,
    authToken: "secret",
    pluginVersion: "1.0.0",
    protocolVersion: "1.0.0",
    capabilities: {},
    startedAt: new Date().toISOString(),
    descriptorPath: "fixture.json"
  };
}

function handleSocket(socket: Socket): void {
  const decoder = new FrameDecoder();
  socket.on("data", (chunk) => {
    for (const value of decoder.push(chunk)) {
      const request = parseRpcMessage(value);
      if (!("method" in request) || !("id" in request)) continue;
      const result = request.method === "session.authenticate"
        ? { authenticated: true, protocolVersion: "1.0.0" }
        : { echoedMethod: request.method, params: request.params ?? {} };
      const response = encodeFrame({ jsonrpc: "2.0", id: request.id, result });
      socket.write(response.subarray(0, 3));
      socket.write(response.subarray(3));
    }
  });
}

async function listen(handler: (socket: Socket) => void): Promise<number> {
  server = createServer((socket) => {
    // 客户端超时或取消时会主动断开，测试服务端将对端重置视为预期清理。
    socket.on("error", () => undefined);
    handler(socket);
  });
  await new Promise<void>((resolve) => server?.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  if (address === null || typeof address === "string") throw new Error("missing test server port");
  return address.port;
}

describe("RPC client", () => {
  it("authenticates then executes a framed request", async () => {
    const port = await listen(handleSocket);
    client = await RpcClient.connect(session(port));
    await expect(client.request("unreal.status", { includeDirty: true })).resolves.toEqual({
      echoedMethod: "unreal.status",
      params: { includeDirty: true }
    });
  });

  it("rejects the wrong token during the first RPC handshake", async () => {
    const port = await listen((socket) => {
      const decoder = new FrameDecoder();
      socket.on("data", (chunk) => {
        for (const value of decoder.push(chunk)) {
          const request = parseRpcMessage(value);
          if (!("method" in request) || !("id" in request)) continue;
          const validToken = request.method === "session.authenticate" && request.params?.authToken === "expected-token";
          socket.write(encodeFrame({
            jsonrpc: "2.0",
            id: request.id,
            ...(validToken
              ? { result: { authenticated: true, protocolVersion: "1.0.0" } }
              : { error: { code: -32001, message: "Invalid authentication token", data: { stableCode: "AUTHENTICATION_FAILED" } } })
          }));
        }
      });
    });
    await expect(RpcClient.connect(session(port))).rejects.toMatchObject({
      code: "AUTHENTICATION_FAILED"
    });
  });

  it("rejects incompatible protocol majors before opening a socket", async () => {
    const incompatible = { ...session(65_534), protocolVersion: "2.0.0" };
    await expect(RpcClient.connect(incompatible)).rejects.toMatchObject({ code: "PROTOCOL_MISMATCH" });
  });

  it("times out a request, drops its late response, and keeps later requests valid", async () => {
    const port = await listen((socket) => {
      const decoder = new FrameDecoder();
      socket.on("data", (chunk) => {
        for (const value of decoder.push(chunk)) {
          const request = parseRpcMessage(value);
          if (!("method" in request) || !("id" in request)) continue;
          const result = request.method === "session.authenticate"
            ? { authenticated: true, protocolVersion: "1.2.0" }
            : { method: request.method };
          const delay = request.method === "slow.request" ? 40 : 0;
          setTimeout(() => socket.write(encodeFrame({ jsonrpc: "2.0", id: request.id, result })), delay);
        }
      });
    });
    client = await RpcClient.connect(session(port));
    await expect(client.request("slow.request", {}, { timeoutMs: 10 }))
      .rejects.toMatchObject({ code: "REQUEST_TIMEOUT", retryable: true });
    await expect(client.request("unreal.status")).resolves.toEqual({ method: "unreal.status" });
    expect(client.negotiatedProtocolVersion).toBe("1.2.0");
  });

  it("cancels an in-flight request through AbortSignal", async () => {
    const port = await listen((socket) => {
      const decoder = new FrameDecoder();
      socket.on("data", (chunk) => {
        for (const value of decoder.push(chunk)) {
          const request = parseRpcMessage(value);
          if (!("method" in request) || !("id" in request) || request.method !== "session.authenticate") continue;
          socket.write(encodeFrame({ jsonrpc: "2.0", id: request.id, result: { authenticated: true, protocolVersion: "1.0.0" } }));
        }
      });
    });
    client = await RpcClient.connect(session(port));
    const controller = new AbortController();
    const pending = client.request("unreal.search", {}, { signal: controller.signal });
    controller.abort();
    await expect(pending).rejects.toMatchObject({ code: "REQUEST_ABORTED" });
  });

  it("decodes a real socket response split one byte at a time", async () => {
    const port = await listen((socket) => {
      const decoder = new FrameDecoder();
      socket.on("data", (chunk) => {
        for (const value of decoder.push(chunk)) {
          const request = parseRpcMessage(value);
          if (!("method" in request) || !("id" in request)) continue;
          const result = request.method === "session.authenticate"
            ? { authenticated: true, protocolVersion: "1.0.0" }
            : { split: true };
          const response = encodeFrame({ jsonrpc: "2.0", id: request.id, result });
          for (const byte of response) socket.write(Buffer.from([byte]));
        }
      });
    });
    client = await RpcClient.connect(session(port));
    await expect(client.request("unreal.status")).resolves.toEqual({ split: true });
  });

  it("refuses two live Editors until an explicit session is selected", async () => {
    const directory = await mkdtemp(join(tmpdir(), "pi-ubp-live-editors-"));
    const editors = [createServer(handleSocket), createServer(handleSocket)];
    let selected: UnrealClient | undefined;
    try {
      await Promise.all(editors.map((editor) => new Promise<void>((resolve) => editor.listen(0, "127.0.0.1", resolve))));
      for (let index = 0; index < editors.length; index += 1) {
        const address = editors[index]?.address();
        if (address === null || address === undefined || typeof address === "string") throw new Error("missing Editor port");
        await writeFile(join(directory, `editor-${index}.json`), JSON.stringify({
          ...session(address.port),
          editorSessionId: `editor-${index}`
        }), "utf8");
      }
      const discovery = { sessionsDirectory: directory, isProcessAlive: () => true };
      await expect(UnrealClient.connect({
        session: { uproject: "E:/Master/LuaSocial.uproject" },
        discovery
      })).rejects.toMatchObject({ code: "SESSION_AMBIGUOUS" });

      selected = await UnrealClient.connect({ session: { editorSessionId: "editor-1" }, discovery });
      await expect(selected.invoke("unreal_status", {})).resolves.toMatchObject({ echoedMethod: "unreal.status" });
    } finally {
      selected?.close();
      await Promise.all(editors.map((editor) => new Promise<void>((resolve) => editor.close(() => resolve()))));
      await rm(directory, { recursive: true, force: true });
    }
  });
});
