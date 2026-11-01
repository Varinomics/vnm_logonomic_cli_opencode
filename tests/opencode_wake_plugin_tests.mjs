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
    get: async ({ path }) => ({ data: { id: path.id } }),
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
  start()
  assert.equal((await notify("busy")).disposition, "deferred")
  complete()
  assert.deepEqual(await inspect(), { available: true })
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
  event("session.deleted", { info: { id: "ses_recipient" } })
  assert.equal((await notify("deleted")).reason, "provider_session_unavailable")
  await wake.dispose()
  wake = undefined
  await assert.rejects(stat(registration), { code: "ENOENT" })
  assert.equal(await create_session_wake({ session: {} }, registration), undefined)
  console.log("OpenCode wake plugin: identity, policy, native admission, replay, interruption and disposal passed")
} finally {
  await wake?.dispose()
  await rm(root, { recursive: true, force: true })
}
