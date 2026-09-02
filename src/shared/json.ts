export type JsonPrimitive = null | boolean | number | string;
export type JsonValue = JsonPrimitive | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };

export function isJsonObject(value: unknown): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function assertJsonValue(value: unknown, path = "$"): asserts value is JsonValue {
  if (value === null || typeof value === "string" || typeof value === "boolean") return;
  if (typeof value === "number") {
    if (Number.isFinite(value)) return;
    throw new TypeError(`${path} must be a finite JSON number`);
  }
  if (Array.isArray(value)) {
    value.forEach((item, index) => assertJsonValue(item, `${path}[${index}]`));
    return;
  }
  if (isJsonObject(value)) {
    for (const [key, item] of Object.entries(value)) assertJsonValue(item, `${path}.${key}`);
    return;
  }
  throw new TypeError(`${path} is not JSON-serializable`);
}

export function jsonDepth(value: JsonValue): number {
  let childDepth = 0;
  if (Array.isArray(value)) {
    for (const item of value) childDepth = Math.max(childDepth, jsonDepth(item));
    return 1 + childDepth;
  }
  if (isJsonObject(value)) {
    for (const item of Object.values(value)) childDepth = Math.max(childDepth, jsonDepth(item));
    return 1 + childDepth;
  }
  return 0;
}
