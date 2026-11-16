// Native event fixtures preserve the adopted root/child correlation and bounded
// projection contract while exercising the packaged SDK helper subprocess.
import assert from "node:assert/strict"
import { spawnSync } from "node:child_process"
import { appendFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { pathToFileURL } from "node:url"

const [pluginPath, helperPath, wrapperPath, verifierPath, packagePath] = process.argv.slice(2)
assert(pluginPath && helperPath && wrapperPath && verifierPath && packagePath,
  "Expected plugin, packaged helper, test vnmctl, reply verifier and package paths")
const reply_cases = []
const scratch = mkdtempSync(join(tmpdir(), "opencode-plugin-"))
const capturePath = join(scratch, "events.jsonl")
process.once("exit", () => rmSync(scratch, { recursive: true, force: true }))
Object.assign(process.env, {
  LOGONOMIC_VNMCTL_PATH: resolve(wrapperPath),
  LOGONOMIC_SESSION_CAPABILITY: "a".repeat(64),
  LOGONOMIC_PROVIDER_PACKAGE_ID: "com.varinomics.cli.opencode",
  LOGONOMIC_PROVIDER_GENERATION_DIGEST: "b".repeat(64),
  LOGONOMIC_PROVIDER_GENERATION_NONCE: "c".repeat(32),
  LOGONOMIC_PROVIDER_HELPERS_DIR: dirname(resolve(helperPath)),
  OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED: "1",
  OPENCODE_TEST_EVENT_CAPTURE: capturePath,
})
const { Logonomic } = await import(pathToFileURL(resolve(pluginPath)))
const privateCanary = "PRIVATE_OPENCODE_CHILD_CONTENT_MUST_NOT_LEAK"

const resetCapture = () => writeFileSync(capturePath, "")
const captured = ({ writing = false } = {}) => {
  const text = readFileSync(capturePath, "utf8")
  const end = text.lastIndexOf("\n") + 1
  // A live append can expose its tail before the record's newline is visible.
  assert(writing || end === text.length, "captured event record is incomplete after delivery settled")
  return text.slice(0, end).split("\n").slice(0, -1).map(JSON.parse)
}
const waitForCapturedLength = async (length) => {
  const deadline = Date.now() + 10_000
  while (Date.now() < deadline) {
    if (captured({ writing: true }).length >= length) return
    await new Promise((resolve) => setTimeout(resolve, 10))
  }
  assert.fail(`timed out waiting for ${length} captured events`)
}
const event = (plugin, type, properties) =>
  plugin.event({ event: { type, properties } })
const publishRoot = async (plugin, sessionID) => {
  const turnID = `published-turn-${sessionID}`
  event(plugin, "session.created", { info: { id: sessionID } })
  await plugin["chat.message"](
    { sessionID },
    { message: { id: turnID }, parts: [{ synthetic: false }] },
  )
  event(plugin, "session.status", {
    sessionID,
    status: { type: "busy" },
  })
  event(plugin, "message.updated", {
    info: {
      id: `assistant-${turnID}`,
      sessionID,
      parentID: turnID,
      role: "assistant",
    },
  })
  event(plugin, "session.status", {
    sessionID,
    status: { type: "busy" },
  })
  await waitForCapturedLength(1)
  await new Promise((resolve) => setTimeout(resolve, 100))
  resetCapture()
}
const settle = async (plugin) => {
  await plugin.dispose()
  assert(!JSON.stringify(captured()).includes(privateCanary))
}
const clientFor = (sessions, failedSessions = new Set()) => ({
  session: {
    get: async ({ path, signal }) => {
      assert(signal instanceof AbortSignal)
      if (failedSessions.has(path.id)) throw new Error("lookup failed")
      return { data: sessions.get(path.id) }
    },
    messages: () => {
      throw new Error("full message hydration is forbidden")
    },
  },
})

{
  const first = { event: "session.status" }
  const second = { event: "logonomic.subagent.started", agent_id: "capture-child" }
  const prefix = '{"event":"logonomic.subagent.started",'
  writeFileSync(capturePath, JSON.stringify(first) + "\n" + prefix)
  assert.deepEqual(captured({ writing: true }), [first])
  assert.throws(() => captured(), /captured event record is incomplete/)
  appendFileSync(capturePath, '"agent_id":"capture-child"}')
  assert.deepEqual(captured({ writing: true }), [first], "a record requires its newline")
  assert.throws(() => captured(), /captured event record is incomplete/)
  appendFileSync(capturePath, "\n")
  assert.deepEqual(captured(), [first, second])
  writeFileSync(capturePath, '{"event":}\n' + prefix)
  assert.throws(() => captured({ writing: true }), SyntaxError,
    "a partial tail does not hide a malformed complete record")
}

resetCapture()
{
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  await publishRoot(plugin, "switch-A")
  await plugin["chat.message"]({ sessionID: "switch-A" }, {
    message: { id: "input-before-B" }, parts: [{ synthetic: false }],
  })
  event(plugin, "session.created", { info: { id: "never-published" } })
  await plugin["chat.message"]({ sessionID: "never-published" }, {
    message: { id: "never-published-input" }, parts: [{ synthetic: false }],
  })
  await publishRoot(plugin, "switch-B")
  event(plugin, "message.updated", { info: {
    id: "late-first-confirmation", parentID: "never-published-input", sessionID: "never-published", role: "assistant",
  } })
  event(plugin, "session.status", { sessionID: "never-published", status: { type: "busy" } })
  event(plugin, "session.idle", { sessionID: "never-published" })
  event(plugin, "message.updated", { info: {
    id: "late-confirmation", parentID: "input-before-B", sessionID: "switch-A", role: "assistant",
  } })
  event(plugin, "session.status", { sessionID: "switch-A", status: { type: "retry" } })
  event(plugin, "session.idle", { sessionID: "switch-A" })
  await plugin["chat.message"]({ sessionID: "switch-A" }, {
    message: { id: "return-to-A" }, parts: [{ synthetic: false }],
  })
  event(plugin, "message.updated", { info: {
    id: "reply-after-return", parentID: "return-to-A", sessionID: "switch-A", role: "assistant",
  } })
  event(plugin, "session.status", { sessionID: "switch-A", status: { type: "busy" } })
  event(plugin, "session.status", { sessionID: "switch-A", status: { type: "retry" } })
  await settle(plugin)
  assert.deepEqual(captured().map((item) => [item.event, item.turn_id, item.fresh_user_turn]), [
    ["session.status", "input-before-B", undefined],
    ["session.idle", "input-before-B", undefined],
    ["session.status", "return-to-A", true],
    ["session.status", "return-to-A", undefined],
  ], "only the first confirmed working status for the new input can readmit root A")
}

resetCapture()
{
  const sessions = new Map([
    ["root-native", { id: "root-native", title: privateCanary }],
  ])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(plugin, "root-native")
  event(plugin, "session.created", {
    info: {
      id: "child-native",
      parentID: "root-native",
      agent: "explore",
      title: privateCanary,
      prompt: privateCanary,
    },
  })
  event(plugin, "session.status", {
    sessionID: "child-native",
    status: { type: "busy" },
  })
  event(plugin, "session.status", {
    sessionID: "child-native",
    status: { type: "retry" },
  })
  event(plugin, "session.status", {
    sessionID: "child-native",
    status: { type: "idle" },
  })
  event(plugin, "session.idle", { sessionID: "child-native" })
  event(plugin, "session.status", {
    sessionID: "child-native",
    status: { type: "busy" },
  })
  event(plugin, "session.error", {
    sessionID: "child-native",
    error: { name: "Error", message: privateCanary },
  })
  event(plugin, "session.error", {
    sessionID: "child-native",
    error: { name: "Error", message: privateCanary },
  })
  event(plugin, "session.status", {
    sessionID: "child-native",
    status: { type: "busy" },
  })
  event(plugin, "session.deleted", {
    info: {
      id: "child-native",
      parentID: "root-native",
      agent: "explore",
      title: privateCanary,
    },
  })
  event(plugin, "session.deleted", {
    info: {
      id: "child-native",
      parentID: "root-native",
      agent: "explore",
      title: privateCanary,
    },
  })
  event(plugin, "session.idle", { sessionID: "child-native" })
  await settle(plugin)
  assert.deepEqual(captured(), [
    {
      event: "logonomic.subagent.started",
      session_id: "root-native",
      agent_id: "child-native",
      agent_label: "explore",
    },
    {
      event: "logonomic.subagent.stopped",
      session_id: "root-native",
      agent_id: "child-native",
      agent_label: "explore",
    },
    {
      event: "logonomic.subagent.started",
      session_id: "root-native",
      agent_id: "child-native",
      agent_label: "explore",
    },
    {
      event: "logonomic.subagent.stopped",
      session_id: "root-native",
      agent_id: "child-native",
      agent_label: "explore",
    },
    {
      event: "logonomic.subagent.started",
      session_id: "root-native",
      agent_id: "child-native",
      agent_label: "explore",
    },
    {
      event: "logonomic.subagent.stopped",
      session_id: "root-native",
      agent_id: "child-native",
      agent_label: "explore",
    },
  ])
}

resetCapture()
{
  const sessions = new Map([
    ["root-nested", { id: "root-nested" }],
    ["parent-nested", {
      id: "parent-nested",
      parentID: "root-nested",
      agent: "planner",
      title: privateCanary,
    }],
    ["child-nested", {
      id: "child-nested",
      parentID: "parent-nested",
      title: privateCanary,
    }],
  ])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(plugin, "root-nested")
  event(plugin, "session.status", {
    sessionID: "child-nested",
    status: { type: "busy" },
  })
  event(plugin, "session.idle", { sessionID: "child-nested" })
  await settle(plugin)
  assert.deepEqual(captured(), [
    {
      event: "logonomic.subagent.started",
      session_id: "root-nested",
      agent_id: "child-nested",
    },
    {
      event: "logonomic.subagent.stopped",
      session_id: "root-nested",
      agent_id: "child-nested",
    },
  ])
}

resetCapture()
{
  const sessions = new Map([
    ["root-boundary", { id: "root-boundary" }],
    ["child-boundary", {
      id: "child-boundary",
      parentID: "root-boundary",
      agent: "reviewer",
      title: privateCanary,
    }],
  ])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(plugin, "root-boundary")
  event(plugin, "session.status", {
    sessionID: "child-boundary",
    status: { type: "busy" },
  })
  event(plugin, "session.deleted", {
    info: { id: "root-boundary", title: privateCanary },
  })
  event(plugin, "session.idle", { sessionID: "child-boundary" })
  await settle(plugin)
  assert.deepEqual(captured(), [
    {
      event: "logonomic.subagent.started",
      session_id: "root-boundary",
      agent_id: "child-boundary",
      agent_label: "reviewer",
    },
    {
      event: "session.deleted",
      session_id: "root-boundary",
    },
  ])
}

resetCapture()
{
  const sessions = new Map([
    ["failed-child", {
      id: "failed-child",
      parentID: "failed-parent",
      agent: "private-agent",
      title: privateCanary,
    }],
    ["cycle-a", { id: "cycle-a", parentID: "cycle-b" }],
    ["cycle-b", { id: "cycle-b", parentID: "cycle-a" }],
    ["oversized-child", {
      id: "oversized-child",
      parentID: "root-oversized",
      agent: "x".repeat(257),
      title: privateCanary,
    }],
    ["root-oversized", { id: "root-oversized" }],
    ["malformed-child", {
      id: "malformed-child",
      parentID: "root-malformed\ninvalid",
      title: privateCanary,
    }],
  ])
  for (let index = 0; index < 18; ++index) {
    sessions.set(`deep-${index}`, {
      id: `deep-${index}`,
      parentID: index === 17 ? "root-deep" : `deep-${index + 1}`,
    })
  }
  sessions.set("root-deep", { id: "root-deep" })
  const plugin = await Logonomic({
    client: clientFor(sessions, new Set(["failed-parent"])),
  })
  await publishRoot(plugin, "root-oversized")
  for (const sessionID of [
    "failed-child",
    "cycle-a",
    "deep-0",
    "oversized-child",
    "malformed-child",
  ]) {
    event(plugin, "session.status", {
      sessionID,
      status: { type: "busy" },
    })
  }
  event(plugin, "session.idle", { sessionID: "oversized-child" })
  await settle(plugin)
  assert.deepEqual(captured(), [
    {
      event: "logonomic.subagent.started",
      session_id: "root-oversized",
      agent_id: "oversized-child",
    },
    {
      event: "logonomic.subagent.stopped",
      session_id: "root-oversized",
      agent_id: "oversized-child",
    },
  ])
}

resetCapture()
{
  const sessions = new Map([["root-capacity", { id: "root-capacity" }]])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  // Seed inactive metadata before publishing so a pending helper delivery cannot
  // turn this cache-eviction fixture into an event-queue overflow.
  for (let index = 0; index < 129; ++index) {
    const id = `capacity-child-${index}`
    const info = {
      id,
      parentID: "root-capacity",
      agent: "worker",
      title: privateCanary,
    }
    sessions.set(id, info)
    event(plugin, "session.created", { info })
    if (index % 8 === 7) {
      await new Promise((resolve) => setImmediate(resolve))
    }
  }
  await publishRoot(plugin, "root-capacity")
  event(plugin, "session.status", {
    sessionID: "capacity-child-0",
    status: { type: "busy" },
  })
  event(plugin, "session.status", {
    sessionID: "capacity-child-128",
    status: { type: "busy" },
  })
  await settle(plugin)
  assert.deepEqual(captured(), [
    {
      event: "logonomic.subagent.started",
      session_id: "root-capacity",
      agent_id: "capacity-child-0",
      agent_label: "worker",
    },
    {
      event: "logonomic.subagent.started",
      session_id: "root-capacity",
      agent_id: "capacity-child-128",
      agent_label: "worker",
    },
  ])
}

resetCapture()
{
  const sessions = new Map([
    ["root-active-capacity", { id: "root-active-capacity" }],
  ])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(plugin, "root-active-capacity")
  for (let index = 0; index < 128; ++index) {
    const id = `active-capacity-child-${index}`
    const info = {
      id,
      parentID: "root-active-capacity",
      title: privateCanary,
    }
    sessions.set(id, info)
    event(plugin, "session.created", { info })
    event(plugin, "session.status", {
      sessionID: id,
      status: { type: "busy" },
    })
    await waitForCapturedLength(index + 1)
  }
  const overflowInfo = {
    id: "active-capacity-child-overflow",
    parentID: "root-active-capacity",
    title: privateCanary,
  }
  sessions.set(overflowInfo.id, overflowInfo)
  event(plugin, "session.created", { info: overflowInfo })
  await waitForCapturedLength(129)
  await settle(plugin)
  const projected = captured()
  assert.equal(projected.length, 129)
  assert.deepEqual(projected.at(-1), {
    event: "logonomic.delivery_overflow",
  })
}

// Each relay delivery runs helper processes, while native events arrive one
// event-loop turn apart. These schedules therefore arrive while one real
// delivery is still in flight, as the OpenCode 1.18.33 queue trace observed.
const native_turn = () => new Promise((resolve) => setImmediate(resolve))
const unused_tokens = { input: 0, output: 0, reasoning: 0, cache: { read: 0, write: 0 } }
// OpenCode's processor starts the step of each assistant record it dispatches.
const step_start = (plugin, session_id, message_id) => event(plugin, "message.part.updated", {
  part: { id: `step-${message_id}`, sessionID: session_id, messageID: message_id, type: "step-start" },
})

resetCapture()
{
  // Observed native order: root A's first reply completes, a mailbox wake turn
  // runs in A, and a new root B is created and prompted, all behind A's start.
  const root_a = "ses_native_a"
  const root_b = "ses_native_b"
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  const user = (session_id, id) =>
    ({ info: { id, sessionID: session_id, role: "user", time: { created: 1 } } })
  const reply = (session_id, id, parent_id, completed = false) => ({ info: {
    id, sessionID: session_id, parentID: parent_id, role: "assistant",
    providerID: "fixture", modelID: "wake", tokens: unused_tokens,
    ...(completed ? { finish: "stop", time: { created: 1, completed: 2 } } : { time: { created: 1 } }),
  } })
  const touch = (session_id) => () =>
    event(plugin, "session.updated", { info: { id: session_id, title: privateCanary } })
  const status = (session_id, type) => () =>
    event(plugin, "session.status", { sessionID: session_id, status: { type } })
  const idle = (session_id) => () => event(plugin, "session.idle", { sessionID: session_id })
  const updated = (value) => () => event(plugin, "message.updated", value)
  const step = (session_id, message_id) => () => step_start(plugin, session_id, message_id)
  const prompt = (session_id, id) => () => plugin["chat.message"]({ sessionID: session_id }, {
    message: { id }, parts: [{ type: "text", text: "Read the new mailbox notification." }],
  })
  const text = (session_id, message_id) => () => plugin["experimental.text.complete"](
    { sessionID: session_id, messageID: message_id, partID: `part-${message_id}` },
    { text: "The isolated mailbox notification was received." })
  // The processor publishes busy before the step of the record it dispatches.
  const turn = (session_id, user_id, reply_id) => [
    updated(user(session_id, user_id)), touch(session_id), status(session_id, "busy"),
    updated(reply(session_id, reply_id, user_id)), touch(session_id),
    updated(reply(session_id, reply_id, user_id)), status(session_id, "busy"), step(session_id, reply_id),
  ]
  const completion = (session_id, user_id, reply_id) => [
    text(session_id, reply_id), updated(reply(session_id, reply_id, user_id)),
    updated(reply(session_id, reply_id, user_id, true)), status(session_id, "busy"),
    status(session_id, "idle"), idle(session_id), touch(session_id),
  ]
  const schedule = [
    () => event(plugin, "session.created", { info: { id: root_a } }), touch(root_a),
    prompt(root_a, "msg_a_user_1"), ...turn(root_a, "msg_a_user_1", "msg_a_reply_1").slice(0, 4),
    touch(root_a), updated(reply(root_a, "msg_a_reply_1", "msg_a_user_1")), touch(root_a),
    // A's confirmed start is delivered while every following arrival is queued.
    status(root_a, "busy"), step(root_a, "msg_a_reply_1"),
    ...completion(root_a, "msg_a_user_1", "msg_a_reply_1"), updated(user(root_a, "msg_a_user_1")),
    prompt(root_a, "msg_a_user_2"), ...turn(root_a, "msg_a_user_2", "msg_a_reply_2"),
    ...completion(root_a, "msg_a_user_2", "msg_a_reply_2"), updated(user(root_a, "msg_a_user_2")),
    () => event(plugin, "session.created", { info: { id: root_b } }), touch(root_b),
    prompt(root_b, "msg_b_user_1"), ...turn(root_b, "msg_b_user_1", "msg_b_reply_1"), touch(root_b),
    ...completion(root_b, "msg_b_user_1", "msg_b_reply_1").slice(0, 6), touch(root_b),
  ]
  for (const arrival of schedule) {
    await arrival()
    await native_turn()
  }
  await settle(plugin)
  const delivered = captured()
  assert(!delivered.some((item) => item.event === "logonomic.delivery_overflow"),
    "accepted native work within the bound must not degrade lifecycle while one delivery is in flight")
  assert.deepEqual(delivered.filter((item) => item.event === "session.status" &&
      (item.fresh_user_turn || item.status_type === "idle"))
    .map((item) => [item.session_id, item.turn_id, item.status_type, item.fresh_user_turn === true]), [
    [root_a, "msg_a_user_1", "busy", true],
    [root_a, "msg_a_user_1", "idle", false],
    [root_a, "msg_a_user_2", "busy", true],
    [root_a, "msg_a_user_2", "idle", false],
    [root_b, "msg_b_user_1", "busy", true],
    [root_b, "msg_b_user_1", "idle", false],
  ], "every observed root turn starts and completes, and the new root B is selected")
  const starts = delivered.filter((item) => item.fresh_user_turn)
  const [nonce_a, continued_a, nonce_b] = starts.map((item) => item.activation_nonce)
  assert.match(nonce_a, /^[a-f0-9]{32}$/)
  assert.match(nonce_b, /^[a-f0-9]{32}$/)
  assert.equal(continued_a, nonce_a, "a later input in the same root keeps its activation")
  assert.notEqual(nonce_b, nonce_a, "root B opens its own transition")
  const b_selected = delivered.indexOf(starts[2])
  const facts = delivered.flatMap((item, index) =>
    (item.model_state?.updates ?? []).map((update) => [index, update]))
  assert.deepEqual([...new Set(facts.map(([, update]) => update.provider_session_id))], [root_a, root_b],
    "each root's served model is reported")
  assert(facts.every(([index, update]) => update.agent_id === "" && (update.provider_session_id === root_a
    ? update.transition_nonce === nonce_a
    : update.provider_session_id === root_b && update.transition_nonce === nonce_b && index >= b_selected)),
  "optional model facts stay with their own root activation")
}

resetCapture()
{
  // Repeated native observations of one serving model are optional evidence and
  // cannot consume the capacity that the core lifecycle depends on.
  const session_id = "model-pressure-root"
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  await publishRoot(plugin, session_id)
  event(plugin, "session.status", { sessionID: session_id, status: { type: "busy" } })
  await native_turn()
  for (let index = 0; index < 40; ++index) {
    const message_id = `assistant-${index}-published-turn-${session_id}`
    event(plugin, "message.updated", { info: {
      id: message_id, sessionID: session_id,
      parentID: `published-turn-${session_id}`, role: "assistant",
      providerID: "fixture", modelID: "wake", tokens: unused_tokens, time: { created: 1 },
    } })
    step_start(plugin, session_id, message_id)
    await native_turn()
  }
  event(plugin, "session.status", { sessionID: session_id, status: { type: "idle" } })
  await settle(plugin)
  const delivered = captured()
  assert(!delivered.some((item) => item.event === "logonomic.delivery_overflow"),
    "optional model observations must not degrade the core lifecycle")
  assert(delivered.some((item) => item.event === "session.status" && item.status_type === "idle" &&
    item.turn_id === `published-turn-${session_id}`), "the turn's completion is delivered")
  const updates = delivered.flatMap((item) => item.model_state?.updates ?? [])
  assert(updates.length > 0, "the served model is reported")
  assert(updates.every((update, index) => update.provider_session_id === session_id &&
    update.agent_id === "" && update.transition_nonce === updates[0].transition_nonce &&
    (index === 0 || BigInt(update.sequence) >= BigInt(updates[index - 1].sequence))),
  "any delivered model fact keeps its root activation and source order")
}

resetCapture()
{
  // A root usage update holds serial projection in the native catalogue lookup
  // until the explicit release, which runs before any timer can. Model steps
  // keep their arrival positions among core events meanwhile:
  // - repeated parts of one dispatched message share one slot, so beyond the
  //   128 step slots another child's step still keeps its own;
  // - each root record and loop status stays a core barrier for its step, so
  //   every root status carries the model served before it, ending with the newest.
  const root_id = "slot-root"
  let lookup_started
  const lookup_called = new Promise((resolve) => { lookup_started = resolve })
  let release_lookup
  const plugin = await Logonomic({ client: { ...clientFor(new Map()), provider: { list: () => {
    lookup_started()
    return new Promise((resolve) => {
      release_lookup = () => resolve({ data: { all: [{
        id: "fixture", models: { "root-model": { limit: { context: 4000 } } },
      }] } })
    })
  } } } })
  const record = (session_id, id, model_id, update = {}) => ({ info: {
    id, sessionID: session_id, parentID: `turn-${session_id}`, role: "assistant",
    providerID: "fixture", modelID: model_id, tokens: unused_tokens, time: { created: 1 }, ...update,
  } })
  const status = (session_id, type) => event(plugin, "session.status", { sessionID: session_id, status: { type } })
  event(plugin, "session.created", { info: { id: root_id } })
  await plugin["chat.message"]({ sessionID: root_id }, {
    message: { id: `turn-${root_id}` }, parts: [{ synthetic: false }],
  })
  event(plugin, "message.updated", record(root_id, "slot-root-reply", "root-model"))
  status(root_id, "busy")
  step_start(plugin, root_id, "slot-root-reply")
  for (const id of ["slot-child-a", "slot-child-b"]) {
    event(plugin, "session.created", { info: { id, parentID: root_id } })
    status(id, "busy")
    event(plugin, "message.updated", record(id, `${id}-reply`, `${id}-model`))
    const deadline = Date.now() + 5000
    while (!captured({ writing: true }).some((item) =>
      item.event === "logonomic.subagent.started" && item.agent_id === id)) {
      assert(Date.now() < deadline, `child ${id} did not start`)
      await new Promise((resolve) => setTimeout(resolve, 10))
    }
  }
  const usage = { input: 1000, output: 80, reasoning: 20, cache: { read: 200, write: 30 } }
  plugin.event({ event: { id: "evt-slot-root-usage", type: "message.updated", properties: record(root_id,
    "slot-root-reply", "root-model", { tokens: usage, finish: "tool-calls", time: { created: 1, completed: 2 } }) } })
  let handshake
  await Promise.race([lookup_called, new Promise((_, reject) => {
    handshake = setTimeout(() => reject(new Error("the root usage projection must start its catalogue lookup")), 5000)
  })]).finally(() => clearTimeout(handshake))
  for (let index = 0; index < 256; ++index) step_start(plugin, "slot-child-a", "slot-child-a-reply")
  step_start(plugin, "slot-child-b", "slot-child-b-reply")
  for (let index = 1; index <= 3; ++index) {
    status(root_id, "busy")
    event(plugin, "message.updated", record(root_id, `slot-root-step-${index}`, `root-model-${index}`))
    status(root_id, "busy")
    step_start(plugin, root_id, `slot-root-step-${index}`)
  }
  status(root_id, "busy")
  status(root_id, "idle")
  release_lookup()
  await settle(plugin)
  const delivered = captured()
  const nonce = delivered.find((item) => item.fresh_user_turn)?.activation_nonce
  const facts = [...new Map(delivered.flatMap((item) => item.model_state?.updates ?? [])
    .map((update) => [`${update.transition_nonce}/${update.sequence}`, update])).values()]
  assert.deepEqual({
    overflow: delivered.some((item) => item.event === "logonomic.delivery_overflow"),
    core: delivered.filter((item) => item.event !== "message.updated").map((item) =>
      [item.event, item.agent_id ?? "", item.status_type ?? "", item.fresh_user_turn === true]),
    root_carried: delivered.filter((item) => item.event === "session.status" && !item.fresh_user_turn)
      .map((item) => item.model_state?.updates.find((update) => !update.agent_id)?.model),
    children: facts.filter((update) => update.agent_id)
      .map((update) => [update.agent_id, update.model, update.transition_nonce]),
  }, {
    overflow: false,
    core: [
      ["session.status", "", "busy", true],
      ["logonomic.subagent.started", "slot-child-a", "", false],
      ["logonomic.subagent.started", "slot-child-b", "", false],
      ...Array.from({ length: 7 }, () => ["session.status", "", "busy", false]),
      ["session.status", "", "idle", false],
    ],
    root_carried: ["root-model", "root-model", "root-model-1", "root-model-1", "root-model-2", "root-model-2",
      "root-model-3", "root-model-3"].map((model) => `fixture/${model}`),
    children: [
      ["slot-child-a", "fixture/slot-child-a-model", nonce],
      ["slot-child-b", "fixture/slot-child-b-model", nonce],
    ],
  }, "model steps keep their own slots and arrival positions among core events while projection is held")
}

resetCapture()
{
  // Two observed roots run their loops concurrently, and OpenCode hands each
  // event to the plugin without waiting (plugin/index.ts:255-262). Each loop
  // publishes busy (prompt.ts:1089), writes its assistant record (1186-1201),
  // yields while it prepares the request (1213-1263), publishes the processor's
  // busy (processor.ts:653), then starts that record's step (424-432). A's second
  // record arrives while A's first activation remains current; B, then A, are
  // selected before their steps. Each arrival drains before the next.
  const root_a = "pending-turn-a"
  const root_b = "pending-turn-b"
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  const record = (session_id, id, parent_id, model_id, update = {}) => ({ info: {
    id, sessionID: session_id, parentID: parent_id, role: "assistant",
    providerID: "fixture", modelID: model_id, tokens: unused_tokens, time: { created: 1 }, ...update,
  } })
  const created = (session_id) => () => event(plugin, "session.created", { info: { id: session_id } })
  const prompt = (session_id, id) => () => plugin["chat.message"]({ sessionID: session_id }, {
    message: { id }, parts: [{ synthetic: false }],
  })
  const updated = (value) => () => event(plugin, "message.updated", value)
  const status = (session_id, type) => () =>
    event(plugin, "session.status", { sessionID: session_id, status: { type } })
  const step = (session_id, message_id) => () => step_start(plugin, session_id, message_id)
  const schedule = [
    created(root_a), prompt(root_a, "a1"), updated(record(root_a, "ma1", "a1", "model-a1")),
    status(root_a, "busy"), step(root_a, "ma1"), status(root_a, "idle"),
    created(root_b), prompt(root_b, "b1"),
    status(root_b, "busy"), // B's loop: no input is confirmed yet.
    updated(record(root_b, "mb1", "b1", "model-b1")),
    prompt(root_a, "a2"),
    status(root_a, "busy"), // A's loop: its first turn ended, and a2 is not yet confirmed.
    updated(record(root_a, "ma2", "a2", "model-a2")),
    status(root_b, "busy"), // B's processor selects B.
    step(root_b, "mb1"),
    status(root_a, "busy"), // A's processor selects A for a2.
    step(root_a, "ma2"),
    updated(record(root_a, "ma2", "a2", "model-a2", {
      tokens: { input: 1000, output: 80, reasoning: 20, cache: { read: 200, write: 30 } },
      finish: "stop", time: { created: 1, completed: 2 },
    })),
    status(root_a, "busy"), status(root_a, "idle"),
  ]
  for (const arrival of schedule) {
    await arrival()
    await native_turn()
  }
  await settle(plugin)
  const delivered = captured()
  const starts = delivered.filter((item) => item.fresh_user_turn)
  const facts = [...new Map(delivered.flatMap((item) => item.model_state?.updates ?? [])
    .map((update) => [`${update.transition_nonce}/${update.sequence}`, update])).values()]
  assert.deepEqual({
    starts: starts.map((item) => [item.session_id, item.turn_id]),
    nonces: new Set(starts.map((item) => item.activation_nonce)).size,
    facts: facts.map((update) => [update.provider_session_id, update.agent_id, update.model, update.transition_nonce]),
  }, {
    starts: [[root_a, "a1"], [root_b, "b1"], [root_a, "a2"]],
    nonces: 3,
    facts: [
      [root_a, "", "fixture/model-a1", starts[0]?.activation_nonce],
      [root_b, "", "fixture/model-b1", starts[1]?.activation_nonce],
      [root_a, "", "fixture/model-a2", starts[2]?.activation_nonce],
    ],
  }, "a record of a root's next input confirms its model under the activation that input opens")
}

resetCapture()
{
  // Distinct core lifecycle work beyond the bound still degrades honestly
  // instead of retaining an unbounded backlog behind a delivery in flight.
  const root_id = "burst-capacity-root"
  const plugin = await Logonomic({ client: clientFor(new Map([[root_id, { id: root_id }]])) })
  await publishRoot(plugin, root_id)
  const children = Array.from({ length: 40 }, (_, index) => `burst-capacity-child-${index}`)
  for (const id of children) {
    event(plugin, "session.created", { info: { id, parentID: root_id, title: privateCanary } })
    await native_turn()
  }
  event(plugin, "session.status", { sessionID: root_id, status: { type: "busy" } })
  await native_turn()
  for (const id of children) {
    event(plugin, "session.status", { sessionID: id, status: { type: "busy" } })
    await native_turn()
  }
  await settle(plugin)
  const delivered = captured()
  assert.deepEqual(delivered.at(-1), { event: "logonomic.delivery_overflow" },
    "a backlog of distinct child starts beyond the bound degrades the lifecycle")
  assert(delivered.filter((item) => item.event === "logonomic.subagent.started").length < children.length)
}

for (const terminalEvent of [
  {
    type: "session.status",
    properties: {
      sessionID: "fresh-instance-child",
      status: { type: "idle" },
    },
  },
  {
    type: "session.error",
    properties: {
      sessionID: "fresh-instance-child",
      error: { name: "Error", message: privateCanary },
    },
  },
  {
    type: "session.deleted",
    properties: {
      info: {
        id: "fresh-instance-child",
        parentID: "fresh-instance-root",
        title: privateCanary,
      },
    },
  },
]) {
  resetCapture()
  const sessions = new Map([
    ["fresh-instance-root", { id: "fresh-instance-root" }],
    ["fresh-instance-child", {
      id: "fresh-instance-child",
      parentID: "fresh-instance-root",
      agent: "worker",
      title: privateCanary,
    }],
  ])
  const firstPlugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(firstPlugin, "fresh-instance-root")
  event(firstPlugin, "session.status", {
    sessionID: "fresh-instance-child",
    status: { type: "busy" },
  })
  await settle(firstPlugin)

  const secondPlugin = await Logonomic({ client: clientFor(sessions) })
  event(secondPlugin, terminalEvent.type, terminalEvent.properties)
  event(secondPlugin, terminalEvent.type, terminalEvent.properties)
  await settle(secondPlugin)
  assert.deepEqual(captured(), [
    {
      event: "logonomic.subagent.started",
      session_id: "fresh-instance-root",
      agent_id: "fresh-instance-child",
      agent_label: "worker",
    },
    {
      event: "logonomic.subagent.stopped",
      session_id: "fresh-instance-root",
      agent_id: "fresh-instance-child",
      ...(terminalEvent.type === "session.deleted"
        ? {}
        : { agent_label: "worker" }),
    },
  ])
}

resetCapture()
{
  const sessions = new Map([
    ["corrected-root", { id: "corrected-root" }],
    ["corrected-session", { id: "corrected-session" }],
  ])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(plugin, "corrected-root")
  event(plugin, "session.created", {
    info: { id: "corrected-session", title: privateCanary },
  })
  const correctedInfo = {
    id: "corrected-session",
    parentID: "corrected-root",
    agent: "worker",
    title: privateCanary,
  }
  sessions.set(correctedInfo.id, correctedInfo)
  event(plugin, "session.updated", { info: correctedInfo })
  event(plugin, "session.status", {
    sessionID: "corrected-session",
    status: { type: "busy" },
  })
  event(plugin, "session.idle", { sessionID: "corrected-session" })
  await settle(plugin)
  assert.deepEqual(captured(), [
    {
      event: "logonomic.subagent.started",
      session_id: "corrected-root",
      agent_id: "corrected-session",
      agent_label: "worker",
    },
    {
      event: "logonomic.subagent.stopped",
      session_id: "corrected-root",
      agent_id: "corrected-session",
      agent_label: "worker",
    },
  ])
}

resetCapture()
{
  const sessionID = "malformed-correction"
  const turnID = "malformed-correction-turn"
  const sessions = new Map([[sessionID, { id: sessionID }]])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  event(plugin, "session.created", { info: sessions.get(sessionID) })
  sessions.set(sessionID, {
    id: sessionID,
    parentID: "malformed\nparent",
  })
  event(plugin, "session.updated", { info: sessions.get(sessionID) })
  await plugin["chat.message"](
    { sessionID },
    { message: { id: turnID }, parts: [{ synthetic: false }] },
  )
  event(plugin, "message.updated", {
    info: {
      id: `assistant-${turnID}`,
      sessionID,
      parentID: turnID,
      role: "assistant",
    },
  })
  event(plugin, "session.status", {
    sessionID,
    status: { type: "busy" },
  })
  await settle(plugin)
  assert.deepEqual(captured(), [])
}

resetCapture()
{
  const sessionID = "persistent-lookup-failure"
  const turnID = "persistent-lookup-failure-turn"
  const sessions = new Map([[sessionID, { id: sessionID }]])
  const plugin = await Logonomic({
    client: clientFor(sessions, new Set([sessionID])),
  })
  await plugin["chat.message"](
    { sessionID },
    { message: { id: turnID }, parts: [{ synthetic: false }] },
  )
  event(plugin, "message.updated", {
    info: {
      id: `assistant-${turnID}`,
      sessionID,
      parentID: turnID,
      role: "assistant",
    },
  })
  for (let index = 0; index < 2; ++index) {
    event(plugin, "session.status", {
      sessionID,
      status: { type: "busy" },
    })
  }
  event(plugin, "session.deleted", {
    info: sessions.get(sessionID),
  })
  await settle(plugin)
  assert.deepEqual(captured(), [])
}

for (const deletedInfo of [
  {
    id: "fallback-child",
    parentID: "conflicting-parent",
    title: privateCanary,
  },
  {
    id: "fallback-child",
    title: privateCanary,
  },
  {
    id: "fallback-child",
    parentID: "unparsable\nparent",
    title: privateCanary,
  },
]) {
  resetCapture()
  const sessions = new Map([
    ["fallback-root", { id: "fallback-root" }],
    ["fallback-child", {
      id: "fallback-child",
      parentID: "fallback-root",
      agent: "worker",
      title: privateCanary,
    }],
  ])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(plugin, "fallback-root")
  event(plugin, "session.status", {
    sessionID: "fallback-child",
    status: { type: "busy" },
  })
  event(plugin, "session.deleted", {
    info: deletedInfo,
  })
  await settle(plugin)
  assert.deepEqual(captured(), [
    {
      event: "logonomic.subagent.started",
      session_id: "fallback-root",
      agent_id: "fallback-child",
      agent_label: "worker",
    },
    {
      event: "logonomic.subagent.stopped",
      session_id: "fallback-root",
      agent_id: "fallback-child",
      agent_label: "worker",
    },
  ])
}

resetCapture()
{
  const session_id = "prompt-root"
  const turn_id = `published-turn-${session_id}`
  const sessions = new Map([
    [session_id, { id: session_id }],
    ["prompt-child-a", { id: "prompt-child-a", parentID: session_id }],
    ["prompt-child-b", { id: "prompt-child-b", parentID: session_id }],
    ["unmanaged-root", { id: "unmanaged-root" }],
    ["unmanaged-child", { id: "unmanaged-child", parentID: "unmanaged-root" }],
  ])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(plugin, session_id)

  // OpenCode's root UI presents direct-child prompts. Their native request
  // and tool identities must survive projection onto that root.
  for (const child of ["a", "b"]) {
    event(plugin, "permission.asked", {
      sessionID: `prompt-child-${child}`, id: `per_child_${child}`,
      permission: privateCanary,
    })
    event(plugin, "question.asked", {
      sessionID: `prompt-child-${child}`, id: `que_child_${child}`,
      questions: [{ question: privateCanary }],
      tool: { messageID: `assistant-child-${child}`, callID: `call-child-${child}` },
    })
  }
  event(plugin, "permission.asked", { sessionID: session_id, id: "per_root" })
  event(plugin, "permission.replied", { sessionID: "prompt-child-b", requestID: "per_child_a" })
  event(plugin, "question.replied", {
    sessionID: "prompt-child-b", requestID: "que_child_a", answers: [[privateCanary]],
  })
  event(plugin, "permission.replied", { sessionID: session_id, requestID: "per_child_a" })
  event(plugin, "permission.asked", { sessionID: "unmanaged-child", id: "per_unmanaged" })
  event(plugin, "question.asked", { sessionID: "unmanaged-child", id: "que_unmanaged" })

  await plugin["chat.message"]({ sessionID: session_id }, {
    message: { id: "next-prompt-turn" }, parts: [{ synthetic: false }],
  })
  event(plugin, "message.updated", { info: {
    sessionID: session_id, id: "assistant-next-prompt", parentID: "next-prompt-turn", role: "assistant",
  } })
  event(plugin, "session.status", { sessionID: session_id, status: { type: "busy" } })
  event(plugin, "permission.replied", { sessionID: "prompt-child-a", requestID: "per_child_a" })
  await waitForCapturedLength(7)
  const partial = captured()
  assert.deepEqual(partial.map((item) => [item.event, item.agent_id, item.permission_id ?? item.question_id]), [
    ["permission.asked", "prompt-child-a", "per_child_a"],
    ["question.asked", "prompt-child-a", "que_child_a"],
    ["permission.asked", "prompt-child-b", "per_child_b"],
    ["question.asked", "prompt-child-b", "que_child_b"],
    ["permission.asked", undefined, "per_root"],
    ["session.status", undefined, undefined],
    ["permission.replied", "prompt-child-a", "per_child_a"],
  ], "a reply resolves only its originating request; other native prompts remain pending")

  event(plugin, "permission.replied", { sessionID: "prompt-child-b", requestID: "per_child_b" })
  event(plugin, "question.replied", {
    sessionID: "prompt-child-b", requestID: "que_child_b", answers: [[privateCanary]],
  })
  event(plugin, "question.rejected", { sessionID: "prompt-child-a", requestID: "que_child_a" })
  event(plugin, "permission.replied", { sessionID: session_id, requestID: "per_root" })
  await settle(plugin)
  const observed = captured()
  assert.deepEqual(observed.slice(7).map((item) =>
    [item.event, item.agent_id, item.permission_id ?? item.question_id]), [
    ["permission.replied", "prompt-child-b", "per_child_b"],
    ["question.replied", "prompt-child-b", "que_child_b"],
    ["question.rejected", "prompt-child-a", "que_child_a"],
    ["permission.replied", undefined, "per_root"],
  ])
  assert(observed.every((item) => item.session_id === session_id))
  assert(observed.filter((item) => !item.agent_id && item.event !== "session.status")
    .every((item) => item.turn_id === turn_id), "root replies retain the turn that requested attention")
  assert(observed.filter((item) => item.agent_id).every((item) => item.turn_id === undefined),
    "child requests do not borrow an unrelated root turn")
  assert.equal(observed[8].tool_message_id, "assistant-child-b")
  assert.equal(observed[8].tool_call_id, "call-child-b")
  assert.equal(observed[8].answer_nonempty, true)
  assert.equal(observed[9].answer_nonempty, undefined)
}

for (const [terminal, properties] of [
  ["session.status", { status: { type: "idle" } }],
  ["session.idle", {}],
  ["session.error", { error: { name: "Error", message: privateCanary } }],
]) {
  resetCapture()
  const session_id = "background-root"
  const child_id = "background-child"
  const late_child_id = "background-late-child"
  const sessions = new Map([
    [session_id, { id: session_id }],
    [child_id, { id: child_id, parentID: session_id }],
    [late_child_id, { id: late_child_id, parentID: session_id }],
  ])
  const plugin = await Logonomic({ client: clientFor(sessions) })
  await publishRoot(plugin, session_id)
  event(plugin, "session.status", { sessionID: child_id, status: { type: "busy" } })
  event(plugin, "permission.asked", { sessionID: child_id, id: "per_before_idle" })
  event(plugin, "question.asked", { sessionID: child_id, id: "que_before_idle",
    tool: { messageID: "assistant-background-child", callID: "call-before-idle" } })
  event(plugin, terminal, { sessionID: session_id, ...properties })
  event(plugin, "session.updated", { info: { id: child_id, parentID: session_id, title: privateCanary } })
  event(plugin, "permission.replied", { sessionID: child_id, requestID: "per_before_idle" })
  event(plugin, "question.replied", {
    sessionID: child_id, requestID: "que_before_idle", answers: [[privateCanary]],
  })
  event(plugin, "permission.asked", { sessionID: child_id, id: "per_after_idle" })
  event(plugin, "question.asked", { sessionID: child_id, id: "que_after_idle" })
  // A child's first observed prompt may arrive after its parent has finished;
  // native session ownership is sufficient even without an observed child start.
  event(plugin, "permission.asked", { sessionID: late_child_id, id: "per_late_child" })
  event(plugin, "question.asked", { sessionID: late_child_id, id: "que_late_child" })
  await plugin["chat.message"]({ sessionID: session_id }, {
    message: { id: "next-background-turn" }, parts: [{ synthetic: false }],
  })
  event(plugin, "message.updated", { info: {
    sessionID: session_id, id: "assistant-next-background", parentID: "next-background-turn", role: "assistant",
  } })
  event(plugin, "session.status", { sessionID: session_id, status: { type: "busy" } })
  event(plugin, "permission.replied", { sessionID: child_id, requestID: "per_after_idle" })
  event(plugin, "question.rejected", { sessionID: child_id, requestID: "que_after_idle" })
  event(plugin, "permission.replied", { sessionID: late_child_id, requestID: "per_late_child" })
  event(plugin, "question.replied", {
    sessionID: late_child_id, requestID: "que_late_child", answers: [[privateCanary]],
  })
  await settle(plugin)
  const observed = captured()
  assert.deepEqual(observed.map((item) => [item.event, item.permission_id ?? item.question_id]), [
    ["logonomic.subagent.started", undefined],
    ["permission.asked", "per_before_idle"],
    ["question.asked", "que_before_idle"],
    [terminal, undefined],
    ["permission.replied", "per_before_idle"],
    ["question.replied", "que_before_idle"],
    ["permission.asked", "per_after_idle"],
    ["question.asked", "que_after_idle"],
    ["permission.asked", "per_late_child"],
    ["question.asked", "que_late_child"],
    ["session.status", undefined],
    ["permission.replied", "per_after_idle"],
    ["question.rejected", "que_after_idle"],
    ["permission.replied", "per_late_child"],
    ["question.replied", "que_late_child"],
  ], `${terminal} does not settle background child prompts`)
  const prompts = observed.filter((item) => item.permission_id || item.question_id)
  assert(prompts.every((item) => item.session_id === session_id && item.turn_id === undefined))
  assert(prompts.every((item) => item.agent_id ===
    ((item.permission_id ?? item.question_id).endsWith("late_child") ? late_child_id : child_id)))
  assert.equal(observed[5].tool_message_id, "assistant-background-child")
  assert.equal(observed[5].tool_call_id, "call-before-idle")
  assert.equal(observed[10].turn_id, "next-background-turn")
}

resetCapture()
{
  const session_id = "question-root"
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  await publishRoot(plugin, session_id)
  const ask = (id) => event(plugin, "question.asked", {
    id, sessionID: session_id, questions: [{ question: privateCanary }],
    tool: { messageID: `assistant-published-turn-${session_id}`, callID: `call-${id}` },
  })
  ask("que_answer")
  ask("que_answer")
  event(plugin, "question.replied", {
    sessionID: "unrelated", requestID: "que_answer", answers: [[privateCanary]],
  })
  event(plugin, "question.replied", {
    sessionID: session_id, requestID: "que_answer", answers: [[privateCanary]],
  })
  event(plugin, "question.replied", {
    sessionID: session_id, requestID: "que_answer", answers: [[privateCanary]],
  })
  ask("que_empty")
  event(plugin, "question.replied", {
    sessionID: session_id, requestID: "que_empty", answers: [[]],
  })
  ask("que_reject")
  event(plugin, "question.rejected", { sessionID: session_id, requestID: "que_reject" })
  ask("que_pending_at_disposal")
  await settle(plugin)
  const observed = captured()
  assert.deepEqual(observed.map((item) => item.event), [
    "question.asked", "question.replied", "question.asked", "question.replied",
    "question.asked", "question.rejected", "question.asked",
  ])
  assert.equal(observed[1].answer_nonempty, true)
  assert.equal(observed[3].answer_nonempty, false)
  assert.equal(observed[5].answer_nonempty, undefined)
  assert(observed.every((item) => item.turn_id === `published-turn-${session_id}`))
  assert.equal(observed[1].tool_call_id, "call-que_answer")
  assert(!JSON.stringify(observed).includes('"answers"'))
  event(plugin, "question.replied", {
    sessionID: session_id, requestID: "que_pending_at_disposal", answers: [[privateCanary]],
  })
  assert.deepEqual(captured(), observed)
}

// A normal attributed reply still carries its final question after a long
// explanation; the native text-complete hook supplies the complete text part.
resetCapture()
{
  const session_id = "long-reply-question"
  const turn_id = `published-turn-${session_id}`
  const message_id = `assistant-${turn_id}`
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  await publishRoot(plugin, session_id)
  const text = privateCanary + "An explanation. ".repeat(400) + "\n\nWhich target should I use?"
  await plugin["experimental.text.complete"](
    { sessionID: session_id, messageID: message_id, partID: "part-long" }, { text })
  event(plugin, "message.updated", { info: {
    id: message_id, sessionID: session_id, parentID: turn_id, role: "assistant",
    finish: "stop", time: { completed: 1234 },
  } })
  event(plugin, "session.status", { sessionID: session_id, status: { type: "idle" } })
  await settle(plugin)
  const body = captured().find((item) => item.event === "session.status")
  assert.equal(body.normal_completion, true, "a long normal reply lost its closing conversational question")
  assert.equal(body.assistant_reply_evidence.suffix, "Which target should I use?")
  reply_cases.push({ text, body, hint: true })
}


// Text parts can split a Markdown opener, a CRLF boundary, or a long paragraph.
// The complete attributed reply remains the oracle, regardless of that split.
const long_text = "A complete explanation. ".repeat(350)
const stream_cases = [
  { parts: [long_text, " Which target?"], hint: true },
  { parts: ["Please provide ", long_text, "."], hint: true },
  { parts: ["Which target?", " ".repeat(5000)], hint: true },
  { parts: ["``", "`text\n", long_text, "\n\nWhich target?"], hint: false },
  { parts: ["~~~~text\r", "\n", long_text, "\n~~~\nWhich target?"], hint: false },
  { parts: ["```text\n", long_text, "\n```\n\nWhich target?"], hint: true },
  { parts: ["~~~text\n", long_text, "\n~~~ trailing text\nWhich target?"], hint: false },
  { parts: ["> ", long_text, "\nWhich target?"], hint: false },
  { parts: ["Prose\n> Quoted text", "\nWhich target?"], hint: false },
  { parts: ["> Quote\n", "\nWhich target?"], hint: true },
  { parts: ["Complete.\r", "\n \t\r", "\nWhich target?"], hint: true },
  { parts: ["Which target?\n\n", "    code question?"], hint: false },
  { parts: ["Which target?\n\n", "> quoted question?"], hint: false },
  { parts: ["界".repeat(750), "选择哪个目标？"], hint: true },
  { parts: [long_text, "\n\nComplete."], hint: false },
]
for (const [index, { parts, hint }] of stream_cases.entries()) {
  resetCapture()
  const session_id = `stream-reply-${index}`
  const turn_id = `published-turn-${session_id}`
  const message_id = `assistant-${turn_id}`
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  await publishRoot(plugin, session_id)
  for (const [part, text] of parts.entries()) {
    const input = { sessionID: session_id, messageID: message_id, partID: `part-${part}` }
    await plugin["experimental.text.complete"](input, { text })
    // Repeated delivery of the same complete part must not duplicate its bytes.
    await plugin["experimental.text.complete"](input, { text })
  }
  event(plugin, "message.updated", { info: {
    id: message_id, sessionID: session_id, parentID: turn_id, role: "assistant",
    finish: "stop", time: { completed: 1234 },
  } })
  event(plugin, "session.status", { sessionID: session_id, status: { type: "idle" } })
  await settle(plugin)
  const body = captured().find((item) => item.event === "session.status")
  assert(body, "normal completion was lost while scanning text parts")
  reply_cases.push({ text: parts.join(""), body, hint })
}

for (const disposition of ["normal", "intermediate", "aborted", "oversized", "unicode", "pending-question",
  "lone-high", "lone-low", "surrogate-pair", "replacement-character", "nul"]) {
  resetCapture()
  const session_id = `text-${disposition}`
  const turn_id = `published-turn-${session_id}`
  const message_id = `assistant-${turn_id}`
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  await publishRoot(plugin, session_id)
  const text = disposition === "oversized" ? "界".repeat(700) + "?"
    : disposition === "unicode" ? "选择哪个目标？"
    : disposition === "lone-high" ? "Choose \ud800?"
    : disposition === "lone-low" ? "Choose \udc00?"
    : disposition === "surrogate-pair" ? "Choose \ud83d\ude00?"
    : disposition === "replacement-character" ? "Choose \ufffd?"
    : disposition === "nul" ? "Choose \0?" : "Which target should I use?"
  await plugin["experimental.text.complete"](
    { sessionID: session_id, messageID: message_id, partID: "part-answer" }, { text })
  event(plugin, "message.updated", { info: {
    id: message_id, sessionID: session_id, parentID: turn_id, role: "assistant",
    finish: disposition === "intermediate" ? "tool-calls" : "stop", time: { completed: 1234 },
  } })
  if (disposition === "pending-question") {
    event(plugin, "question.asked", { id: "que_unanswered", sessionID: session_id })
  }
  if (disposition === "aborted") {
    event(plugin, "session.error", { sessionID: session_id, error: { name: "MessageAbortedError" } })
  }
  event(plugin, "session.status", { sessionID: session_id, status: { type: "idle" } })
  event(plugin, "session.idle", { sessionID: session_id })
  // Wait for the first terminal fact before sending the next real user turn.
  await waitForCapturedLength(disposition === "pending-question" ? 2 : 1)
  await plugin["chat.message"]({ sessionID: session_id }, {
    message: { id: `next-${turn_id}` }, parts: [{ synthetic: false }],
  })
  await settle(plugin)
  const observed = captured()
  const completion = observed.find((item) => item.event === "session.status")
  if (["normal", "oversized", "unicode", "surrogate-pair", "replacement-character"].includes(disposition)) {
    assert.equal(completion.assistant_reply_evidence.scope, "final_paragraph_edges")
    assert.equal(completion.assistant_reply_evidence.context, "prose")
    for (const edge of ["prefix", "suffix"]) {
      assert(Buffer.byteLength(completion.assistant_reply_evidence[edge], "utf8") <= 128)
    }
    reply_cases.push({ text, body: completion, hint: true })
    assert.equal(completion.normal_completion, true)
    assert.equal(observed.filter((item) => item.event === "session.status").length, 1)
    assert.equal(observed.at(-1).event, "chat.message")
  } else {
    assert(observed.every((item) => item.assistant_reply_evidence === undefined))
    assert(observed.every((item) => item.event !== "chat.message"))
  }
}

resetCapture()
{
  const session_id = "question-turn-binding"
  const plugin = await Logonomic({ client: clientFor(new Map()) })
  await publishRoot(plugin, session_id)
  event(plugin, "question.asked", { sessionID: session_id, id: "que_original" })
  await plugin["chat.message"]({ sessionID: session_id }, {
    message: { id: "next-turn" }, parts: [{ synthetic: false }],
  })
  event(plugin, "message.updated", { info: {
    sessionID: session_id, id: "assistant-next", parentID: "next-turn", role: "assistant",
  } })
  event(plugin, "session.status", { sessionID: session_id, status: { type: "busy" } })
  event(plugin, "question.replied", {
    sessionID: session_id, requestID: "que_original", answers: [[privateCanary]],
  })
  await settle(plugin)
  assert.equal(captured().at(-1).turn_id, `published-turn-${session_id}`)
  assert.equal(captured().at(-1).question_id, "que_original")
}

// A resumed session opens without any native event; the plugin reports it once
// the resumed OpenCode's own store has it, and only a root session.
const resumedPlugin = async (sessions, resumedSessionID, client = clientFor(sessions)) => {
  process.env.OPENCODE_LOGONOMIC_RESUME_SESSION = resumedSessionID
  try {
    return await Logonomic({ client })
  } finally {
    delete process.env.OPENCODE_LOGONOMIC_RESUME_SESSION
  }
}
resetCapture()
{
  const plugin = await resumedPlugin(
    new Map([["ses_resumed", { id: "ses_resumed", title: privateCanary }]]),
    "ses_resumed",
  )
  await waitForCapturedLength(1)
  await settle(plugin)
  const nonce = captured()[0].activation_nonce
  assert.match(nonce, /^[a-f0-9]{32}$/,
    "a resume opens its root transition under the plugin's source-captured activation nonce")
  assert.deepEqual(captured(), [
    { event: "logonomic.session.resumed", session_id: "ses_resumed", activation_nonce: nonce },
  ])
}
for (const [sessions, description] of [
  [new Map(), "an unknown session"],
  [new Map([["ses_resumed", { id: "ses_other" }]]), "another session"],
  [new Map([["ses_resumed", { id: "ses_resumed", parentID: "ses_root" }]]), "a child session"],
]) {
  resetCapture()
  const plugin = await resumedPlugin(sessions, "ses_resumed")
  await new Promise((resolve) => setTimeout(resolve, 200))
  await settle(plugin)
  assert.deepEqual(captured(), [], `a resume of ${description} is not reported`)
}
// The lookup waits for a slow instance bootstrap, and disposal ends the wait.
const slowClient = (info, milliseconds) => ({
  session: {
    get: ({ path, signal }) => new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => resolve({ data: path.id === info.id ? info : undefined }),
        milliseconds,
      )
      signal.addEventListener("abort", () => {
        clearTimeout(timer)
        reject(new Error("lookup aborted"))
      }, { once: true })
    }),
  },
})
resetCapture()
{
  const plugin = await resumedPlugin(undefined, "ses_resumed",
    slowClient({ id: "ses_resumed" }, 11_000))
  const deadline = Date.now() + 20_000
  while (captured({ writing: true }).length === 0 && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50))
  }
  await settle(plugin)
  const nonce = captured()[0]?.activation_nonce
  assert.match(nonce, /^[a-f0-9]{32}$/,
    "a resume confirmed after a slow bootstrap opens its transition under the source-captured nonce")
  assert.deepEqual(captured(), [
    { event: "logonomic.session.resumed", session_id: "ses_resumed", activation_nonce: nonce },
  ], "a resume confirmed after a slow bootstrap is reported")
}
resetCapture()
{
  const plugin = await resumedPlugin(undefined, "ses_resumed",
    slowClient({ id: "ses_resumed" }, 60_000))
  await new Promise((resolve) => setTimeout(resolve, 100))
  const disposal = Date.now()
  await settle(plugin)
  assert(Date.now() - disposal < 5_000, "disposal ends a pending resume lookup")
  assert.deepEqual(captured(), [])
}
resetCapture()
{
  // A resumed session is started once; its first turn does not start it again.
  const plugin = await resumedPlugin(
    new Map([["ses_resumed", { id: "ses_resumed" }]]),
    "ses_resumed",
  )
  await waitForCapturedLength(1)
  await plugin["chat.message"](
    { sessionID: "ses_resumed" },
    { message: { id: "turn-after-resume" }, parts: [{ synthetic: false }] },
  )
  event(plugin, "message.updated", { info: {
    id: "assistant-after-resume", sessionID: "ses_resumed",
    parentID: "turn-after-resume", role: "assistant",
  } })
  event(plugin, "session.status", { sessionID: "ses_resumed", status: { type: "busy" } })
  await waitForCapturedLength(2)
  await settle(plugin)
  assert.deepEqual(captured().map((item) => item.event),
    ["logonomic.session.resumed", "session.status"])
  assert.equal(captured()[1].turn_id, "turn-after-resume")
}

const reply_cases_path = join(scratch, "reply-cases.json")
writeFileSync(reply_cases_path, JSON.stringify(reply_cases))
const reply_verification = spawnSync(verifierPath, [packagePath, reply_cases_path], { encoding: "utf8" })
assert.equal(reply_verification.status, 0,
  `packaged reply evidence failed: ${reply_verification.stdout}${reply_verification.stderr}`)
process.stdout.write(reply_verification.stdout)
console.log("native-agent-plugin-passed")
