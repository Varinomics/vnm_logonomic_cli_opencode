import assert from "node:assert/strict"
import { spawn, spawnSync } from "node:child_process"
import { once } from "node:events"
import { mkdtemp, readFile, rm, stat } from "node:fs/promises"
import { connect } from "node:net"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { pathToFileURL } from "node:url"

const module_path = process.argv[2] ?? "package/assets/logonomic_opencode_wake.mjs"
const { create_session_wake } = await import(pathToFileURL(resolve(module_path)))
const root = await mkdtemp(join(tmpdir(), "opencode-wake-"))
const registration = join(root, "registration.json")
let wake
try {
  const prompts = []
  let status_hook
  let prompt_hook
  const client = { session: {
    get: async ({ path }) => ({ data: {
      id: path.id, ...(path.id.startsWith("child-") ? { parentID: "ses_previous" } : {}),
    } }),
    status: async (options) => {
      assert.equal(options.throwOnError, true)
      await status_hook?.()
      return { data: {} }
    },
    promptAsync: async (request) => {
      prompts.push(request)
      await prompt_hook?.(request)
      return {}
    },
  } }
  wake = await create_session_wake(client, registration)
  assert(wake)
  let binding = JSON.parse(await readFile(registration, "utf8"))
  if (process.platform !== "win32") assert.equal((await stat(registration)).mode & 0o777, 0o600)
  const other_registration = join(root, "other-launch.json")
  const other = spawnSync(process.execPath, ["--input-type=module", "-e", `
    const { create_session_wake } = await import(process.argv[1])
    const wake = await create_session_wake(
      { session: { get() {}, status() {}, promptAsync() {} } }, process.argv[2])
    if (!wake) process.exitCode = 1
  `, pathToFileURL(resolve(module_path)).href, other_registration], { encoding: "utf8" })
  assert.equal(other.status, 0, other.stderr)
  await assert.rejects(stat(other_registration), { code: "ENOENT" })
  assert.deepEqual(JSON.parse(await readFile(registration, "utf8")), binding)
  const incomplete = connect(binding.port, "127.0.0.1")
  let progress
  let deadline
  try {
    await once(incomplete, "connect")
    const closed = once(incomplete, "close")
    incomplete.write("{")
    progress = setInterval(() => incomplete.write(" "), 250)
    const completed = await Promise.race([
      closed.then(() => true),
      new Promise((resolve) => { deadline = setTimeout(() => resolve(false), 6500) }),
    ])
    assert.equal(completed, true, "partial request progress must not extend the connection deadline")
  } finally {
    clearInterval(progress)
    clearTimeout(deadline)
    incomplete.destroy()
  }
  const send = (request) => new Promise((resolve, reject) => {
    const socket = connect(binding.port, "127.0.0.1")
    let response = ""
    socket.on("error", reject)
    socket.on("connect", () => socket.write(`${JSON.stringify({
      ...binding, provider_session_id: "ses_recipient", transition_nonce: "d".repeat(32), ...request,
    })}\n`))
    socket.on("data", (chunk) => { response += chunk.toString("utf8") })
    socket.on("end", () => {
      try { resolve(JSON.parse(response)) } catch (error) { reject(error) }
    })
  })
  const inspect = () => send({ operation: "inspect" })
  const send_bytes = (chunks) => new Promise((resolve, reject) => {
    const socket = connect(binding.port, "127.0.0.1")
    let response = ""
    socket.on("error", reject)
    socket.on("connect", async () => {
      for (const chunk of chunks) {
        if (socket.destroyed) break
        socket.write(chunk)
        await new Promise((resolve) => setImmediate(resolve))
      }
    })
    socket.on("data", (chunk) => { response += chunk.toString("utf8") })
    socket.on("close", () => resolve(response))
  })
  const notify = (notification_id) => send({
    operation: "notify", notification_id,
    instruction: "Read the new message in your Logonomic mailbox.",
  })
  const event = (type, properties) => wake.event({ type, properties })
  const start = (variant = "deliberate reasoning") => {
    event("session.created", { info: { id: "ses_recipient" } })
    wake.chat_message({ sessionID: "ses_recipient" }, { message: {
      id: "msg_user_turn", agent: "plan",
      model: { providerID: "isolated", modelID: "fixture", variant },
    } })
  }
  const complete = () => {
    event("message.updated", { info: {
      sessionID: "ses_recipient", role: "assistant", parentID: "msg_user_turn",
      finish: "stop", time: { completed: Date.now() },
    } })
    event("session.status", { sessionID: "ses_recipient", status: { type: "idle" } })
  }
  assert.equal((await inspect()).reason, "provider_session_unavailable")
  event("session.created", { info: { id: "ses_previous" } })
  for (let child = 0; child < 130; ++child) {
    const id = `child-${child}`
    event("session.created", { info: { id, parentID: "ses_previous" } })
    event("session.status", { sessionID: id, status: { type: "idle" } })
    event("message.updated", { info: { sessionID: id, role: "assistant" } })
    event("question.asked", { sessionID: id, id: "child-question" })
    event("question.replied", { sessionID: id, requestID: "child-question" })
  }
  start()
  assert.equal((await notify("busy")).disposition, "deferred")
  complete()
  assert.deepEqual(await inspect(), { available: true })
  const framed = Buffer.from(`${JSON.stringify({
    ...binding, provider_session_id: "ses_recipient", transition_nonce: "d".repeat(32),
    operation: "inspect", padding: "résumé",
  })}\n`)
  const split = framed.indexOf(Buffer.from("é")) + 1
  assert.deepEqual(JSON.parse(await send_bytes([
    framed.subarray(0, split), framed.subarray(split, framed.length - 1), framed.subarray(-1),
  ])), { available: true }, "fragmented UTF-8 and a separate newline preserve the request")
  const at_limit = Buffer.concat([
    framed.subarray(0, framed.length - 1), Buffer.alloc(32768 - framed.length, 32), Buffer.from("\n"),
  ])
  assert.deepEqual(JSON.parse(await send_bytes([at_limit])), { available: true })
  assert.equal(await send_bytes([at_limit.subarray(0, at_limit.length - 1), Buffer.from(" \n")]), "",
    "the byte limit includes the final newline")
  assert.equal(await send_bytes([Buffer.concat([framed, framed])]), "",
    "two records on one connection are rejected")
  assert.equal(await send_bytes([Buffer.from("{\n")]), "", "malformed JSON is rejected")
  assert.equal((await send({ operation: "inspect", token: "0".repeat(64) })).error, "invalid_binding")
  assert.equal((await send({ operation: "inspect", instance_id: "retired" })).error, "invalid_binding")
  assert.equal((await send({ operation: "inspect", provider_session_id: "ses_other" })).reason,
    "provider_session_unavailable")
  assert.equal((await send({ operation: "inspect", transition_nonce: "e".repeat(32) })).reason,
    "provider_correlation_changed")

  const accepted = await notify("mail_one")
  assert.equal(accepted.disposition, "accepted")
  assert.deepEqual(await notify("mail_one"), accepted)
  assert.equal(prompts.length, 1)
  assert.equal(prompts[0].path.id, "ses_recipient")
  assert.equal(prompts[0].body.agent, "plan")
  assert.deepEqual(prompts[0].body.model, { providerID: "isolated", modelID: "fixture" })
  assert.equal(prompts[0].body.variant, "deliberate reasoning")
  assert.equal(prompts[0].body.tools, undefined)
  assert.equal(prompts[0].body.noReply, undefined)

  event("permission.asked", { sessionID: "ses_recipient", id: "perm_one" })
  assert.equal((await notify("blocked")).reason, "provider_awaiting_input")
  event("permission.replied", { sessionID: "ses_recipient", requestID: "perm_one" })
  event("question.asked", { sessionID: "ses_recipient", id: "question_one" })
  assert.equal((await notify("question")).disposition, "deferred")
  event("question.rejected", { sessionID: "ses_recipient", requestID: "question_one" })

  status_hook = () => start()
  assert.equal((await notify("racing_user")).reason, "provider_state_changed")
  assert.equal(prompts.length, 1)
  status_hook = undefined
  event("session.idle", { sessionID: "ses_recipient" })
  assert.equal((await notify("interrupted")).reason, "provider_interrupted")
  complete()

  prompt_hook = () => { throw new Error("lost response after native admission") }
  assert.equal((await notify("unknown")).disposition, "uncertain")
  assert.equal((await notify("unknown")).disposition, "uncertain")
  assert.equal(prompts.length, 2)
  prompt_hook = undefined
  if (process.argv[3]) {
    complete()
    const exit = await new Promise((resolve, reject) => {
      const child = spawn(process.argv[3], [process.argv[4], registration], {
        stdio: "inherit", windowsHide: true,
      })
      child.on("error", reject)
      child.on("exit", resolve)
    })
    assert.equal(exit, 0)
    assert.equal(prompts.length, 3, "adapter retry must submit one native prompt")
  }
  const submitted = prompts.length
  for (const variant of [42, "x".repeat(1025)]) {
    start(variant)
    complete()
    assert.deepEqual(await notify("unrepresentable_variant"), {
      disposition: "unavailable", reason: "provider_turn_context_unavailable",
    })
  }
  assert.equal(prompts.length, submitted, "invalid native context must not silently select a default")
  start()
  complete()
  status_hook = () => event("session.deleted", { info: { id: "ses_recipient" } })
  assert.equal((await notify("deleted-during-status")).disposition, "unavailable")
  status_hook = undefined
  assert.equal(prompts.length, submitted, "a revoked in-flight operation cannot submit")
  assert.equal((await notify("deleted")).reason, "provider_correlation_changed")
  start()
  complete()
  const new_epoch = { transition_nonce: "f".repeat(32) }
  assert.equal((await inspect()).reason, "provider_correlation_changed")
  assert.deepEqual(await send({ operation: "inspect", ...new_epoch }), { available: true })
  const first_record = event("session.idle", { sessionID: "ses_recipient" })
  const previous_record = event("session.idle", { sessionID: "ses_previous" })
  wake.select_root("ses_recipient", first_record)
  wake.select_root("ses_previous", previous_record)
  assert.equal((await send({ operation: "inspect", ...new_epoch })).reason, "provider_correlation_changed")
  wake.select_root("ses_recipient", first_record)
  const returned = { transition_nonce: "b".repeat(32) }
  assert.deepEqual(await send({ operation: "inspect", ...returned }), { available: true })
  assert.deepEqual(await send({
    operation: "notify", notification_id: "mail_one", instruction: "existing notification", ...returned,
  }), accepted)
  assert.equal(prompts.length, submitted, "session deletion must retain submitted receipts")
  for (let i = 0; i <= 128; ++i) {
    event("question.asked", { sessionID: `unclassified-${i}`, id: `pending-${i}` })
  }
  assert.deepEqual(await send({ operation: "inspect", ...returned }), { available: true })
  event("session.created", { info: { id: "unclassified-128" } })
  wake.select_root("unclassified-128", event("session.idle", { sessionID: "unclassified-128" }))
  assert.equal((await send({ operation: "inspect", provider_session_id: "unclassified-128" })).reason,
    "provider_session_capacity", "a dropped question cannot be forgotten on later root admission")
  await wake.dispose()
  wake = undefined
  await assert.rejects(stat(registration), { code: "ENOENT" })
  assert.equal(await create_session_wake({ session: {} }, registration), undefined)
  // Drive the real hooks and asynchronous lifecycle classification. An event
  // queued before a deletion cannot resurrect its wake record. Failed lookups
  // for unrelated turns must leave room for a root whose first fact was an ask.
  delete process.env.LOGONOMIC_SESSION_CAPABILITY
  delete process.env.OPENCODE_LOGONOMIC_RESUME_SESSION
  process.env.OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED = "1"
  const { Logonomic } = await import(pathToFileURL(join(dirname(resolve(module_path)), "logonomic_opencode_plugin.mjs")))
  let lookup_available = false
  client.session.get = async ({ path }) => {
    if (!lookup_available) throw new Error("temporary lookup failure")
    return { data: { id: path.id } }
  }
  const plugin = await Logonomic({ client }, { session_wake: { registration_file: registration } })
  wake = { event: (event) => plugin.event({ event }), chat_message: plugin["chat.message"], dispose: plugin.dispose }
  binding = JSON.parse(await readFile(registration, "utf8"))
  const turn = (id) => wake.chat_message({ sessionID: id }, {
    message: { id: "user-turn", agent: "plan", model: { providerID: "fixture", modelID: "model" } },
    parts: [{ synthetic: false }],
  })
  const working = (id) => {
    event("message.updated", { info: { id: "reply", sessionID: id, role: "assistant",
      parentID: "user-turn", finish: "stop", time: { completed: Date.now() } } })
    event("session.status", { sessionID: id, status: { type: "busy" } })
  }
  const drain = () => new Promise((resolve) => setImmediate(resolve))
  event("session.created", { info: { id: "deleted" } })
  await turn("deleted")
  working("deleted")
  event("session.deleted", { info: { id: "deleted" } })
  await drain()
  assert.equal((await send({ operation: "inspect", provider_session_id: "deleted" })).reason,
    "provider_session_unavailable")
  event("question.asked", { sessionID: "ses_recipient", id: "ask-before-chat" })
  for (let i = 0; i < 130; ++i) {
    const id = `unresolved-turn-${i}`
    await turn(id)
    event("session.status", { sessionID: id, status: { type: "idle" } })
    await drain()
  }
  lookup_available = true
  await turn("ses_recipient")
  working("ses_recipient")
  event("session.idle", { sessionID: "ses_recipient" })
  await drain()
  assert.equal((await inspect()).reason, "provider_awaiting_input")
  event("question.rejected", { sessionID: "ses_recipient", requestID: "ask-before-chat" })
  assert.deepEqual(await inspect(), { available: true })
  for (let i = 0; i < 127; ++i) {
    const id = `blocked-root-${i}`
    event("session.created", { info: { id } })
    event("permission.asked", { sessionID: id, id: "p" })
    await drain()
  }
  const later = "next-root"
  event("session.created", { info: { id: later } })
  event("question.asked", { sessionID: later, id: "pending-at-capacity" })
  await turn(later)
  working(later)
  event("session.idle", { sessionID: later })
  await drain()
  assert.equal((await inspect()).reason, "provider_correlation_changed")
  const inspect_later = () => send({ operation: "inspect", provider_session_id: later })
  assert.equal((await inspect_later()).available, false)
  event("session.deleted", { info: { id: "blocked-root-0" } })
  event("session.updated", { info: { id: later } })
  assert.equal((await inspect_later()).reason, "provider_awaiting_input")
  event("question.rejected", { sessionID: later, requestID: "pending-at-capacity" })
  assert.deepEqual(await inspect_later(), { available: true })
  console.log("OpenCode wake plugin: identity, policy, native admission, replay, interruption and disposal passed")
} finally {
  await wake?.dispose()
  await rm(root, { recursive: true, force: true })
}
