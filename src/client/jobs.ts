import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { isJsonObject, type JsonObject, type JsonValue } from "../shared/json.js";

export const JOB_PHASES = [
  "Queued", "Preflight", "Modify", "Compile", "Save", "Reload", "Verify", "Stopping",
  "Succeeded", "Failed", "Cancelled"
] as const;

export type JobPhase = (typeof JOB_PHASES)[number];
export type JobAccess = "read" | "write";

export interface JobProgress {
  readonly jobId: string;
  readonly phase: JobPhase;
  readonly completed: number;
  readonly total: number;
  readonly message: string;
  readonly assetPath?: string;
  readonly timestamp: string;
}

export type RequestJournalState = "accepted" | "running" | "terminal";

export interface RequestJournalRecord {
  readonly version: 1;
  readonly requestId: string;
  readonly method: string;
  readonly payloadHash: string;
  readonly jobId: string;
  readonly state: RequestJournalState;
  readonly terminalPhase?: "Succeeded" | "Failed" | "Cancelled";
  readonly acceptedAt: string;
  readonly updatedAt: string;
  readonly interrupted: boolean;
  readonly recoveryRequired: boolean;
  readonly result?: JsonObject;
  readonly error?: JsonObject;
}

export interface RequestJournalStatus {
  readonly healthy: boolean;
  readonly directory: string;
  readonly recordCount: number;
  readonly accepted: number;
  readonly running: number;
  readonly terminal: number;
  readonly error?: JsonObject;
}

export interface JobSnapshot {
  readonly jobId: string;
  readonly requestId: string;
  readonly access: JobAccess;
  readonly phase: JobPhase;
  readonly terminal: boolean;
  readonly cancellationRequested: boolean;
  readonly cancellationSafe: boolean;
  readonly createdAt: string;
  readonly updatedAt: string;
  readonly result?: JsonObject;
  readonly error?: JsonObject;
  readonly progress: JobProgress[];
}

function isPhase(value: unknown): value is JobPhase {
  return typeof value === "string" && (JOB_PHASES as readonly string[]).includes(value);
}

export function parseJobSnapshot(value: JsonValue): JobSnapshot {
  if (!isJsonObject(value)
    || typeof value.jobId !== "string"
    || typeof value.requestId !== "string"
    || (value.access !== "read" && value.access !== "write")
    || !isPhase(value.phase)
    || typeof value.terminal !== "boolean"
    || typeof value.cancellationRequested !== "boolean"
    || typeof value.cancellationSafe !== "boolean"
    || typeof value.createdAt !== "string"
    || typeof value.updatedAt !== "string"
    || !Array.isArray(value.progress)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Editor returned an invalid job snapshot");
  }
  return value as unknown as JobSnapshot;
}

export function parseRequestJournalRecord(value: JsonValue): RequestJournalRecord {
  if (!isJsonObject(value)
    || value.version !== 1
    || typeof value.requestId !== "string"
    || typeof value.method !== "string"
    || (typeof value.payloadHash !== "string" && typeof value.paramsHash !== "string")
    || typeof value.jobId !== "string"
    || (value.state !== "accepted" && value.state !== "running" && value.state !== "terminal")
    || typeof value.acceptedAt !== "string"
    || typeof value.updatedAt !== "string"
    || typeof value.interrupted !== "boolean"
    || typeof value.recoveryRequired !== "boolean") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Editor returned an invalid request journal record");
  }
  if (value.state === "terminal"
    && value.terminalPhase !== undefined
    && value.terminalPhase !== "Succeeded"
    && value.terminalPhase !== "Failed"
    && value.terminalPhase !== "Cancelled") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Editor returned an invalid request journal terminal phase");
  }
  return {
    ...value,
    payloadHash: typeof value.payloadHash === "string" ? value.payloadHash : value.paramsHash as string
  } as unknown as RequestJournalRecord;
}

export function parseRequestJournalStatus(value: JsonValue): RequestJournalStatus {
  if (!isJsonObject(value)
    || typeof value.healthy !== "boolean"
    || typeof value.directory !== "string"
    || typeof value.recordCount !== "number"
    || typeof value.accepted !== "number"
    || typeof value.running !== "number"
    || typeof value.terminal !== "number") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Editor returned an invalid request journal status");
  }
  return value as unknown as RequestJournalStatus;
}

export function parseJobProgress(value: JsonValue): JobProgress {
  if (!isJsonObject(value)
    || typeof value.jobId !== "string"
    || !isPhase(value.phase)
    || typeof value.completed !== "number"
    || typeof value.total !== "number"
    || typeof value.message !== "string"
    || typeof value.timestamp !== "string") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Editor returned an invalid job progress notification");
  }
  return value as unknown as JobProgress;
}
