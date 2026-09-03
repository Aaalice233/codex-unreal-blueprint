import type { JsonObject, JsonValue } from "./json.js";

export const ERROR_CODES = {
  INTERNAL: "INTERNAL_ERROR",
  INVALID_ARGUMENT: "INVALID_ARGUMENT",
  INVALID_RESPONSE: "INVALID_RESPONSE",
  FRAME_TOO_LARGE: "FRAME_TOO_LARGE",
  FRAME_MALFORMED: "FRAME_MALFORMED",
  SESSION_DIRECTORY_UNAVAILABLE: "SESSION_DIRECTORY_UNAVAILABLE",
  SESSION_INVALID: "SESSION_INVALID",
  SESSION_NOT_FOUND: "SESSION_NOT_FOUND",
  SESSION_AMBIGUOUS: "SESSION_AMBIGUOUS",
  SESSION_STALE: "SESSION_STALE",
  CONNECTION_FAILED: "CONNECTION_FAILED",
  CONNECTION_CLOSED: "CONNECTION_CLOSED",
  AUTHENTICATION_FAILED: "AUTHENTICATION_FAILED",
  REQUEST_TIMEOUT: "REQUEST_TIMEOUT",
  REQUEST_ABORTED: "REQUEST_ABORTED",
  REQUEST_CONFLICT: "REQUEST_CONFLICT",
  REQUEST_NOT_FOUND: "REQUEST_NOT_FOUND",
  REQUEST_INTERRUPTED: "REQUEST_INTERRUPTED",
  JOURNAL_CORRUPT: "JOURNAL_CORRUPT",
  JOURNAL_IO_ERROR: "JOURNAL_IO_ERROR",
  PROTOCOL_MISMATCH: "PROTOCOL_MISMATCH",
  RPC_ERROR: "RPC_ERROR",
  SETUP_UNAVAILABLE: "SETUP_UNAVAILABLE",
  SETUP_FAILED: "SETUP_FAILED",
  WRITE_PARTIAL: "WRITE_PARTIAL",
  WRITE_STATE_UNKNOWN: "WRITE_STATE_UNKNOWN",
  JOB_TERMINAL_FAILURE: "JOB_TERMINAL_FAILURE"
} as const;

export type ErrorCode = (typeof ERROR_CODES)[keyof typeof ERROR_CODES] | string;

export interface ErrorContext {
  readonly assetPath?: string;
  readonly operationIndex?: number;
  readonly ueCallsite?: string;
  readonly requestId?: string;
  readonly jobId?: string;
  readonly editorSessionId?: string;
  readonly compilerMessages?: readonly JsonValue[];
  readonly details?: JsonObject;
}

export interface SerializedError {
  readonly code: ErrorCode;
  readonly message: string;
  readonly retryable: boolean;
  readonly context?: ErrorContext;
}

export class UnrealBlueprintError extends Error {
  readonly code: ErrorCode;
  readonly retryable: boolean;
  readonly context: ErrorContext | undefined;
  override readonly cause: unknown;

  constructor(code: ErrorCode, message: string, options: {
    retryable?: boolean;
    context?: ErrorContext;
    cause?: unknown;
  } = {}) {
    super(message);
    this.name = "UnrealBlueprintError";
    this.code = code;
    this.retryable = options.retryable ?? false;
    this.context = options.context;
    this.cause = options.cause;
  }

  toJSON(): SerializedError {
    return {
      code: this.code,
      message: this.message,
      retryable: this.retryable,
      ...(this.context === undefined ? {} : { context: this.context })
    };
  }
}

export function asUnrealBlueprintError(error: unknown): UnrealBlueprintError {
  if (error instanceof UnrealBlueprintError) return error;
  if (error instanceof Error) {
    return new UnrealBlueprintError(ERROR_CODES.INTERNAL, error.message, { cause: error });
  }
  return new UnrealBlueprintError(ERROR_CODES.INTERNAL, "Unknown internal error", { cause: error });
}
