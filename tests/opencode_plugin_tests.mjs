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

console.log("native-agent-plugin-passed")
