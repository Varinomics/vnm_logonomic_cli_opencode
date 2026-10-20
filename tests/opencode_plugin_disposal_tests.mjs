// Drive the public disposal cutoff while the real SDK wrapper is still alive.
// Only the plugin's new deadline timers are expired by the fixture; process
// cancellation and reaping still go through the production helper and SDK.
import assert from "node:assert/strict"
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { mock } from "node:test"
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
  plugin.event({ event: { type: "session.status", properties: {
    sessionID: "session-primary", status: { type: "busy" },
  } } })
  const deadline = Date.now() + 5000
  while (!existsSync(pidPath) && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 10))
  }
  assert(existsSync(pidPath), "plugin must start a wrapper descendant before disposal")
  wrapperPid = Number(readFileSync(pidPath, "utf8"))
  assert(Number.isInteger(wrapperPid) && wrapperPid > 0)
  const deadlines = []
  let notifyDeadlineArmed
  const deadlineArmed = new Promise((resolve) => { notifyDeadlineArmed = resolve })
  const schedule = globalThis.setTimeout
  const timers = mock.method(globalThis, "setTimeout", (callback, delay, ...arguments_) => {
    const timer = schedule(callback, delay, ...arguments_)
    deadlines.push(() => {
      clearTimeout(timer)
      callback(...arguments_)
    })
    notifyDeadlineArmed(true)
    return timer
  })
  try {
    const disposal = plugin.dispose()
    assert.equal(await Promise.race([deadlineArmed, disposal.then(() => false)]), true,
      "disposal must arm a deadline while delivery is pending")
    assert(exists(wrapperPid), "disposal must reach its cutoff with a live wrapper")
    for (const expire of deadlines) expire()
    await disposal
  } finally {
    timers.mock.restore()
  }
  assert(!exists(wrapperPid), "wrapper descendant survived plugin disposal")
  const deliveries = readFileSync(process.env.OPENCODE_TEST_EVENT_CAPTURE, "utf8")
    .trim().split("\n")
  assert.equal(deliveries.length, 1, "the disposal cutoff must cancel the delivery without retrying")
} finally {
  if (!wrapperPid && existsSync(pidPath)) wrapperPid = Number(readFileSync(pidPath, "utf8"))
  if (wrapperPid && exists(wrapperPid)) process.kill(wrapperPid, "SIGKILL")
  rmSync(scratch, { recursive: true, force: true })
}
