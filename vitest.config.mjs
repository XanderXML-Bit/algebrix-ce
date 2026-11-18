import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    globalSetup: ["./tests/helpers/native-suite-global-setup.mjs"],
    // The native build result cache relies on sequential test files.
    fileParallelism: false,
    include: [
      "**/*.{test,spec}.?(c|m)[jt]s?(x)",
      "tests/*.js",
    ],
  },
});
