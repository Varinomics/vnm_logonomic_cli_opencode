// Drive the unchanged public plugin disposal path at its production deadline.
// The slow wrapper holds real SDK calls so the last delivery crosses the drain
// cutoff; disposal must settle that owned descendant before returning.
import assert from "node:assert/strict"
import { existsSync, mkdtempSync, readFileSync, rmSync, statSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { pathToFileURL } from "node:url"

const [pluginArgument, helperArgument, wrapperArgument] = process.argv.slice(2)
assert(pluginArgument && helperArgument && wrapperArgument)
const scratch = mkdtempSync(join(tmpdir(), "opencode-plugin-dispose-"))
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
Object.assign(process.env, {
  LOGONOMIC_VNMCTL_PATH: resolve(wrapperArgument),
  LOGONOMIC_SESSION_CAPABILITY: "a".repeat(64),
  LOGONOMIC_PROVIDER_PACKAGE_ID: "com.varinomics.cli.opencode",
  LOGONOMIC_PROVIDER_GENERATION_DIGEST: "b".repeat(64),
  LOGONOMIC_PROVIDER_GENERATION_NONCE: "c".repeat(32),
  LOGONOMIC_PROVIDER_HELPERS_DIR: dirname(resolve(helperArgument)),
  OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED: "1",
  OPENCODE_TEST_EVENT_CAPTURE: join(scratch, "events.jsonl"),
  OPENCODE_TEST_DELIVERY_MODE: "wait-for-cancellation",
  OPENCODE_TEST_WRAPPER_PID: pidPath,
})
const { Logonomic } = await import(pathToFileURL(resolve(pluginArgument)))
const plugin = await Logonomic({ client: { session: {} } })
try {
  plugin.event({ event: { type: "session.created", properties: { info: { id: "session-primary" } } } })
  await plugin["chat.message"]({ sessionID: "session-primary" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  plugin.event({ event: { type: "message.updated", properties: { info: {
    id: "assistant-primary", sessionID: "session-primary", parentID: "turn-user", role: "assistant",
  } } } })
  for (let index = 0; index < 6; ++index) {
    plugin.event({ event: { type: "session.status", properties: {
      sessionID: "session-primary", status: { type: "busy" },
    } } })
  }
  const deadline = Date.now() + 5000
  while (!existsSync(pidPath) && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 10))
  }
  assert(existsSync(pidPath), "plugin must start a wrapper descendant before disposal")
  await plugin.dispose()
  wrapperPid = Number(readFileSync(pidPath, "utf8"))
  assert(Number.isInteger(wrapperPid) && wrapperPid > 0)
  // The SDK owns a ten-second wrapper deadline. A younger final descendant
  // could not have settled by that deadline; its absence proves cutoff cleanup
  // rather than an ordinary fully drained delivery.
  assert(Date.now() - statSync(pidPath).mtimeMs < 10_000,
    "last wrapper must still be inside its own deadline when disposal completes")
  assert(!exists(wrapperPid), "wrapper descendant survived plugin disposal")
} finally {
  if (!wrapperPid && existsSync(pidPath)) wrapperPid = Number(readFileSync(pidPath, "utf8"))
  if (wrapperPid && exists(wrapperPid)) process.kill(wrapperPid, "SIGKILL")
  rmSync(scratch, { recursive: true, force: true })
}
