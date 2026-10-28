// Native event fixtures preserve the adopted root/child correlation and bounded
// projection contract while exercising the packaged SDK helper subprocess.
import assert from "node:assert/strict"
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { pathToFileURL } from "node:url"

const [pluginPath, helperPath, wrapperPath] = process.argv.slice(2)
assert(pluginPath && helperPath && wrapperPath,
  "Expected plugin, packaged helper and test vnmctl executable paths")
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
const captured = () => {
  const text = readFileSync(capturePath, "utf8").trim()
  return text ? text.split("\n").map(JSON.parse) : []
}
const waitForCapturedLength = async (length) => {
  const deadline = Date.now() + 10_000
  while (Date.now() < deadline) {
    if (captured().length >= length) return
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
  await publishRoot(plugin, "root-capacity")
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
  if (["normal", "unicode", "surrogate-pair", "replacement-character"].includes(disposition)) {
    assert.equal(completion.assistant_text, text)
    assert.equal(completion.normal_completion, true)
    assert.equal(observed.filter((item) => item.event === "session.status").length, 1)
    assert.equal(observed.at(-1).event, "chat.message")
  } else {
    assert(observed.every((item) => item.assistant_text === undefined))
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
  assert.deepEqual(captured(), [
    { event: "logonomic.session.resumed", session_id: "ses_resumed" },
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
  while (captured().length === 0 && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50))
  }
  await settle(plugin)
  assert.deepEqual(captured(), [
    { event: "logonomic.session.resumed", session_id: "ses_resumed" },
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

console.log("native-agent-plugin-passed")
