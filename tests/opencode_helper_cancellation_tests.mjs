// Exercise the helper's owned cancellation protocol against its real SDK
// wrapper descendant. opencode_plugin_disposal_tests exercises the full plugin
// cutoff; the original PID-kill reproduction is retained with gate evidence.
import assert from "node:assert/strict"
import { spawn } from "node:child_process"
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"

const [helperArgument, wrapperArgument] = process.argv.slice(2)
assert(helperArgument && wrapperArgument)
const helperPath = resolve(helperArgument)
const scratch = mkdtempSync(join(tmpdir(), "opencode-helper-cancel-"))
const pidPath = join(scratch, "wrapper.pid")
let wrapperPid
const exists = (pid) => {
  try {
    process.kill(pid, 0)
    return true
  } catch (error) {
    if (error.code === "ESRCH") return false
    throw error
  }
}
const helper = spawn(helperPath, ["event", "--watch-parent"], {
  windowsHide: true,
  stdio: ["pipe", "ignore", "ignore"],
  env: {
    ...process.env,
    LOGONOMIC_VNMCTL_PATH: resolve(wrapperArgument),
    LOGONOMIC_SESSION_CAPABILITY: "a".repeat(64),
    LOGONOMIC_PROVIDER_PACKAGE_ID: "com.varinomics.cli.opencode",
    LOGONOMIC_PROVIDER_GENERATION_DIGEST: "b".repeat(64),
    LOGONOMIC_PROVIDER_GENERATION_NONCE: "c".repeat(32),
    LOGONOMIC_PROVIDER_HELPERS_DIR: dirname(helperPath),
    OPENCODE_TEST_EVENT_CAPTURE: join(scratch, "events.jsonl"),
    OPENCODE_TEST_DELIVERY_MODE: "wait-for-cancellation",
    OPENCODE_TEST_WRAPPER_PID: pidPath,
  },
})
const helperClosed = new Promise((resolve, reject) => {
  helper.once("error", reject)
  helper.once("close", resolve)
})
helper.stdin.write(JSON.stringify({
  event: "session.status",
  session_id: "session-primary",
  turn_id: "turn-user",
  status_type: "busy",
}) + "\n")
try {
  const deadline = Date.now() + 5000
  while (!existsSync(pidPath) && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 10))
  }
  assert(existsSync(pidPath), "wrapper descendant must start before cancellation")
  wrapperPid = Number(readFileSync(pidPath, "utf8"))
  assert(Number.isInteger(wrapperPid) && wrapperPid > 0)
  helper.stdin.end()
  await helperClosed
  assert(!exists(wrapperPid), "wrapper descendant survived event helper cancellation")
} finally {
  helper.kill()
  if (wrapperPid && exists(wrapperPid)) process.kill(wrapperPid, "SIGKILL")
  await helperClosed
  rmSync(scratch, { recursive: true, force: true })
}
