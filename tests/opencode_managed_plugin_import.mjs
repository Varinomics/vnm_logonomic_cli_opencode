import assert from "node:assert/strict"
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join } from "node:path"
import { pathToFileURL } from "node:url"

const packageRoot = process.argv[2]
const declaration = JSON.parse(readFileSync(join(packageRoot, "capabilities/managed-assets@1.json"), "utf8"))
const root = mkdtempSync(join(tmpdir(), "opencode-managed-assets-"))
try {
  for (const asset of declaration.payload.assets) {
    const target = join(root, asset)
    mkdirSync(dirname(target), { recursive: true })
    copyFileSync(join(packageRoot, asset), target)
  }
  const entry = join(root, "assets/logonomic_opencode_plugin.mjs")
  assert.equal(typeof (await import(pathToFileURL(entry))).Logonomic, "function")
} finally {
  rmSync(root, { recursive: true, force: true })
}
