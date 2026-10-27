import assert from "node:assert/strict"
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises"
import { connect } from "node:net"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { pathToFileURL } from "node:url"

const module_path = process.argv[2] ?? "package/assets/logonomic_opencode_tui.mjs"
const { create_tui_integration } = await import(pathToFileURL(resolve(module_path)))
const root = await mkdtemp(join(tmpdir(), "opencode-tui-contract-"))
const registration = join(root, "wake.json")
process.env.OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED = "1"
process.env.OPENCODE_LOGONOMIC_WAKE_REGISTRATION = registration
delete process.env.LOGONOMIC_PROVIDER_HELPERS_DIR
const capture = join(root, "events.jsonl")
if (process.argv[3] && process.argv[4]) {
  await writeFile(capture, "")
  Object.assign(process.env, {
    LOGONOMIC_VNMCTL_PATH: resolve(process.argv[4]),
    LOGONOMIC_SESSION_CAPABILITY: "a".repeat(64),
    LOGONOMIC_PROVIDER_PACKAGE_ID: "logonomic.cli.opencode",
    LOGONOMIC_PROVIDER_GENERATION_DIGEST: "b".repeat(64),
    LOGONOMIC_PROVIDER_GENERATION_NONCE: "c".repeat(32),
    LOGONOMIC_PROVIDER_HELPERS_DIR: dirname(resolve(process.argv[3])),
    OPENCODE_TEST_EVENT_CAPTURE: capture,
  })
}

let integration
try {
  let selected = "ses_recipient"
  let route_type = "session"
  let status = "idle"
  let outcome = "succeeded"
  let permissions = []
  let forms = []
  let pending = []
  let listener
  let before_active
  let admit
  const submissions = []
  const session = (id) => ({
    id, outcome, agent: "plan",
    model: { providerID: "fixture", id: "wake", variant: "deliberate" },
  })
  const context = {
    ui: { router: { current: () => ({ type: route_type, sessionID: selected }) } },
    data: {
      listen(callback) { listener = callback; return () => { listener = undefined } },
      session: {
        get: session,
        status: () => status,
        pending: { list: () => pending },
        permission: { list: () => permissions },
        form: { list: () => forms },
      },
    },
    client: {
      session: {
        get: async ({ sessionID }) => session(sessionID),
        active: async () => { await before_active?.(); return status === "running" ? { [selected]: { type: status } } : {} },
        inbox: { list: async () => pending },
        form: { list: async () => forms },
        synthetic: async (input, options) => {
          assert(options.signal instanceof AbortSignal)
          submissions.push(input)
          await admit?.()
          return { id: "msg_notification", sessionID: input.sessionID, type: "synthetic" }
        },
      },
      permission: { list: async () => permissions },
    },
  }
  integration = await create_tui_integration(context)
  integration.select()
  const binding = JSON.parse(await readFile(registration, "utf8"))
  const send = (operation, session_id = selected, nonce = "a".repeat(32), notification_id = "mail_update") =>
    new Promise((resolve, reject) => {
      const socket = connect(binding.port, "127.0.0.1")
      let response = ""
      socket.on("error", reject)
      socket.on("connect", () => socket.write(`${JSON.stringify({
        ...binding, operation, provider_session_id: session_id, transition_nonce: nonce,
        ...(operation === "notify" ? { notification_id, instruction: "Read your Logonomic mailbox." } : {}),
      })}\n`))
      socket.on("data", (chunk) => { response += chunk.toString("utf8") })
      socket.on("end", () => {
        try { resolve(JSON.parse(response)) } catch (error) { reject(error) }
      })
    })

  assert.deepEqual(await send("inspect"), { available: true })
  route_type = "home"
  integration.select()
  const hidden = await send("inspect")
  route_type = "session"
  integration.select()
  assert.deepEqual({ hidden, returned: await send("inspect") }, {
    hidden: { available: false, reason: "provider_session_unavailable" }, returned: { available: true },
  },
    "temporarily leaving the session view does not change the host's native session transition")
  selected = "ses_other"
  integration.select()
  assert.deepEqual(await send("inspect", selected, "b".repeat(32)), { available: true })
  selected = "ses_recipient"
  integration.select()
  const fresh = await send("inspect", selected, "c".repeat(32))
  const retired = await send("notify", selected, "a".repeat(32), "retired_mail")
  assert.deepEqual({ fresh, retired, submissions: submissions.length }, {
    fresh: { available: true },
    retired: { disposition: "unavailable", reason: "provider_correlation_changed" },
    submissions: 0,
  }, "returning to a native session must revoke its earlier Logonomic transition")

  const notify = (id) => send("notify", selected, "c".repeat(32), id)
  status = "running"
  assert.equal((await notify("busy")).disposition, "deferred")
  status = "idle"
  permissions = [{ id: "per_required" }]
  assert.equal((await notify("permission")).reason, "provider_awaiting_input")
  permissions = []
  forms = [{ id: "frm_required" }]
  assert.equal((await notify("form")).reason, "provider_awaiting_input")
  forms = []
  outcome = "interrupted"
  assert.equal((await notify("interrupted")).reason, "provider_interrupted")
  outcome = "succeeded"
  pending = [{ type: "user" }]
  assert.equal((await notify("pending")).disposition, "deferred")
  pending = []
  before_active = () => {
    listener({ details: { type: "session.execution.started", id: "evt_user_started", data: { sessionID: selected } } })
  }
  assert.equal((await notify("race")).reason, "provider_state_changed")
  before_active = undefined
  assert.equal(submissions.length, 0)

  const accepted = await notify("new_mail")
  assert.equal(accepted.disposition, "accepted")
  assert.deepEqual(await notify("new_mail"), accepted)
  assert.deepEqual(submissions, [{
    sessionID: "ses_recipient", text: "Read your Logonomic mailbox.",
    description: "Logonomic message", delivery: "queue", resume: true,
  }], "native synthetic input must leave agent, model, variant and the draft under native ownership")
  admit = () => { throw new Error("native admission response lost") }
  assert.equal((await notify("uncertain")).disposition, "uncertain")
  assert.equal((await notify("uncertain")).disposition, "uncertain")
  assert.equal(submissions.length, 2)
  await integration.dispose()
  integration = undefined
  if (process.argv[3]) {
    const events = (await readFile(capture, "utf8")).trim().split("\n").filter(Boolean).map(JSON.parse)
    assert.deepEqual(events.filter((event) => event.event === "logonomic.session.selected")
      .map((event) => event.session_id), ["ses_recipient", "ses_recipient", "ses_other", "ses_recipient"],
    "returning to the same root must relay re-eligibility so an unread arrival while hidden can retry")
  }
  assert.equal(listener, undefined)
  await assert.rejects(readFile(registration), { code: "ENOENT" })
  console.log("OpenCode TUI contract: transition revocation, native policy, queue admission, receipts and cleanup passed")
} finally {
  await integration?.dispose()
  await rm(root, { recursive: true, force: true })
}
