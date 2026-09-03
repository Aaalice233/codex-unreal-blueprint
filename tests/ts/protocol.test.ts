import { describe, expect, it } from "vitest";
import {
  negotiateProtocolVersion,
  parseProtocolVersion,
  parseRpcMessage,
  rpcFailureToError
} from "../../src/shared/protocol.js";

 describe("JSON-RPC boundary", () => {
  it("accepts a strict success response", () => {
    expect(parseRpcMessage({ jsonrpc: "2.0", id: 7, result: { ok: true } })).toEqual({
      jsonrpc: "2.0", id: 7, result: { ok: true }
    });
  });

  it("rejects unknown envelope fields", () => {
    expect(() => parseRpcMessage({ jsonrpc: "2.0", id: 7, result: {}, extra: true })).toThrow(/unknown fields/);
  });

  it("negotiates compatible SemVer minors and rejects a different major", () => {
    expect(parseProtocolVersion("1.12.3-beta.1+build.7")).toMatchObject({ major: 1, minor: 12, patch: 3 });
    expect(negotiateProtocolVersion("1.0.0", "1.12.3")).toBe("1.12.3");
    expect(() => negotiateProtocolVersion("1.0.0", "2.0.0")).toThrow(/Incompatible protocol versions/);
    expect(() => parseProtocolVersion("1.x")).toThrow(/valid SemVer/);
  });

  it("maps stable UE error context without hiding compiler output", () => {
    const message = parseRpcMessage({
      jsonrpc: "2.0",
      id: 3,
      error: {
        code: -32050,
        message: "Blueprint compile failed",
        data: {
          stableCode: "BLUEPRINT_COMPILE_FAILED",
          retryable: false,
          assetPath: "/Game/CodexAutomation/Run/BP_Test",
          operationIndex: 2,
          ueCallsite: "FKismetEditorUtilities::CompileBlueprint",
          compilerMessages: [{ severity: "Error", message: "Missing pin" }]
        }
      }
    });
    if (!("error" in message)) throw new Error("expected failure response");
    const error = rpcFailureToError(message);
    expect(error.code).toBe("BLUEPRINT_COMPILE_FAILED");
    expect(error.context?.operationIndex).toBe(2);
    expect(error.context?.compilerMessages).toEqual([{ severity: "Error", message: "Missing pin" }]);
    expect(error.context?.details?.rpcCode).toBe(-32050);
  });

  it("rejects malformed structured error fields", () => {
    expect(() => parseRpcMessage({
      jsonrpc: "2.0",
      id: 1,
      error: { code: -32000, message: "bad", data: { operationIndex: -1 } }
    })).toThrow(/operationIndex/);
  });
});
