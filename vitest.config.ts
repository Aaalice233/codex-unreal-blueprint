import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["tests/ts/**/*.test.ts"],
    environment: "node",
    testTimeout: 5_000,
    coverage: { include: ["src/**/*.ts"] }
  }
});
