// Oracle: OpenCode's plugin event envelope, successful SessionCompaction event,
// and Context sidebar's disjoint assistant-token components. The native event
// identity belongs to the observation, not the helper delivery attempt.
import assert from "node:assert/strict"
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { pathToFileURL } from "node:url"

const [plugin_path, helper_path, wrapper_path] = process.argv.slice(2)
assert(plugin_path && helper_path && wrapper_path)
const scratch = mkdtempSync(join(tmpdir(), "opencode-context-"))
const capture_path = join(scratch, "events.jsonl")
process.once("exit", () => rmSync(scratch, { recursive: true, force: true }))
Object.assign(process.env, {
  LOGONOMIC_VNMCTL_PATH: resolve(wrapper_path),
  LOGONOMIC_SESSION_CAPABILITY: "a".repeat(64),
  LOGONOMIC_PROVIDER_PACKAGE_ID: "com.varinomics.cli.opencode",
  LOGONOMIC_PROVIDER_GENERATION_DIGEST: "b".repeat(64),
  LOGONOMIC_PROVIDER_GENERATION_NONCE: "c".repeat(32),
  LOGONOMIC_PROVIDER_HELPERS_DIR: dirname(resolve(helper_path)),
  OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED: "1",
  OPENCODE_TEST_EVENT_CAPTURE: capture_path,
})
const { Logonomic } = await import(pathToFileURL(resolve(plugin_path)))
const captured = () => {
  const text = readFileSync(capture_path, "utf8").trim()
  return text ? text.split("\n").map(JSON.parse) : []
}
const event = (plugin, id, type, properties) => plugin.event({ event: { id, type, properties } })
const assistant = (session_id, tokens, model_id = "model-context") => ({
  id: `msg-${session_id}`, sessionID: session_id, parentID: "turn-user", role: "assistant",
  providerID: "provider-context", modelID: model_id, tokens,
  cost: 100_000, text: "PRIVATE_NATIVE_CONTENT",
})
const usage = (input = 1000) => ({
  input, output: 80, reasoning: 20, cache: { read: 200, write: 30 }, total: 99,
})
const client = (provider_lookup = async () => ({ data: { all: [{
  id: "provider-context", models: { "model-context": { limit: { context: 4000 } } },
}] } })) => ({
  session: {
    get: async ({ path }) => ({ data: { id: path.id } }),
    messages: () => { throw new Error("Context events must not hydrate conversation text") },
  },
  provider: { list: provider_lookup },
})
const root = (plugin, session_id = "session-root") =>
  event(plugin, `evt-create-${session_id}`, "session.created", { info: { id: session_id } })
const run = async (emit, native_client = client()) => {
  writeFileSync(capture_path, "")
  const plugin = await Logonomic({ client: native_client })
  await emit(plugin)
  await plugin.dispose()
  const bodies = captured()
  assert(!JSON.stringify(bodies).includes("PRIVATE_NATIVE_CONTENT"))
  return bodies
}

const measured = await run((plugin) => {
  root(plugin)
  const info = assistant("session-root", usage())
  event(plugin, "evt-usage", "message.updated", { info })
  info.tokens.input = 9000
  event(plugin, "evt-summary", "message.updated", {
    info: { ...assistant("session-root", usage(900)), summary: true },
  })
})
assert.deepEqual(measured, [{ event: "message.updated", session_id: "session-root", context_updates: [{
  provider_session_id: "session-root", event_id: "evt-usage", used_tokens: 1330, context_window: 4000,
}] }], "capture native step usage before mutable message updates; summary billing is not occupancy")

const model_facts = await run(async (plugin) => {
  root(plugin)
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  const root_model = assistant("session-root", usage(), "root-model")
  event(plugin, "evt-root-model", "message.updated", { info: root_model })
  root_model.modelID = "mutated-after-observation"
  event(plugin, "evt-root-start", "session.status", { sessionID: "session-root", status: { type: "busy" } })
  event(plugin, "evt-child-created", "session.created", { info: { id: "session-child", parentID: "session-root" } })
  event(plugin, "evt-child-start", "session.status", { sessionID: "session-child", status: { type: "busy" } })
  event(plugin, "evt-child-model", "message.updated", { info: assistant("session-child", usage(), "child-model") })
  event(plugin, "evt-child-stop", "session.status", { sessionID: "session-child", status: { type: "idle" } })
  event(plugin, "evt-child-late-model", "message.updated", {
    info: assistant("session-child", usage(), "late-stopped-model"),
  })
  event(plugin, "evt-child-restart", "session.status", { sessionID: "session-child", status: { type: "busy" } })
  event(plugin, "evt-child-restop", "session.status", { sessionID: "session-child", status: { type: "idle" } })
  event(plugin, "evt-summary-model", "message.updated", {
    info: { ...assistant("session-root", usage(), "summary-model"), summary: true },
  })
  root(plugin, "session-other")
  await plugin["chat.message"]({ sessionID: "session-other" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  event(plugin, "evt-other-model", "message.updated", { info: assistant("session-other", usage(), "other-model") })
  event(plugin, "evt-other-start", "session.status", { sessionID: "session-other", status: { type: "busy" } })
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-returned" }, parts: [{ synthetic: false }],
  })
  event(plugin, "evt-return-model", "message.updated", {
    info: { ...assistant("session-root", usage(), "returned-model"), parentID: "turn-returned" },
  })
  event(plugin, "evt-return-start", "session.status", { sessionID: "session-root", status: { type: "busy" } })
})
const model_updates = model_facts.flatMap((entry) => entry.model_state?.updates ?? [])
assert.deepEqual([...new Set(model_updates.map((update) => update.model))], [
  "provider-context/root-model", "provider-context/child-model", "provider-context/other-model",
  "provider-context/returned-model",
], "native immutable model identity excludes mutable later values, summary calls and parent inheritance")
const child_models = model_updates.filter((update) => update.agent_id)
assert(child_models.every((update) => update.agent_id === "session-child" &&
  update.provider_session_id === "session-root" && update.model === "provider-context/child-model"))
const starts = model_facts.filter((entry) => entry.event === "session.status" && entry.fresh_user_turn)
assert.deepEqual(starts.map((entry) => entry.session_id), ["session-root", "session-other", "session-root"])
assert.notEqual(starts[0].activation_nonce, starts[2].activation_nonce)
assert(starts.every((entry) => entry.activation_nonce === entry.model_state.updates[0].transition_nonce),
  "initial model evidence uses the same captured nonce as its root transition")
const child_starts = model_facts.filter((entry) => entry.event === "logonomic.subagent.started")
assert.equal(child_starts.length, 2)
assert.equal(child_starts[1].model_state, undefined, "a repeated child run starts without inherited model evidence")

const sequential_children = await run(async (plugin) => {
  root(plugin)
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  event(plugin, "evt-root-confirmed", "message.updated", { info: assistant("session-root", usage()) })
  event(plugin, "evt-root-busy", "session.status", { sessionID: "session-root", status: { type: "busy" } })
  // Drain each real helper delivery before the next native child. This tests
  // inactive source eviction rather than the independent event-queue bound.
  for (let index = 0; index < 130; ++index) {
    const id = `sequential-child-${index}`
    event(plugin, `evt-create-${id}`, "session.created", { info: { id, parentID: "session-root" } })
    event(plugin, `evt-start-${id}`, "session.status", { sessionID: id, status: { type: "busy" } })
    event(plugin, `evt-model-${id}`, "message.updated", { info: assistant(id, usage(), `model-${index}`) })
    event(plugin, `evt-stop-${id}`, "session.status", { sessionID: id, status: { type: "idle" } })
    const deadline = Date.now() + 5000
    while (!captured().some((entry) => entry.event === "logonomic.subagent.stopped" && entry.agent_id === id)) {
      assert(Date.now() < deadline, `native child ${id} did not drain`)
      await new Promise((resolve) => setTimeout(resolve, 10))
    }
  }
})
const sequential_models = sequential_children.flatMap((entry) => entry.model_state?.updates ?? [])
assert.equal(new Set(sequential_models.filter((entry) => entry.agent_id).map((entry) => entry.agent_id)).size, 130,
  "stopped child mappings must not exhaust a lifetime-only 128-session model cache")

const compactions = await run((plugin) => {
  root(plugin)
  event(plugin, "evt-summary-attempt", "message.updated", {
    info: { ...assistant("session-root", usage()), summary: true, error: { name: "Failed" } },
  })
  event(plugin, "evt-compaction", "session.compacted", { sessionID: "session-root" })
  event(plugin, "evt-after", "message.updated", { info: assistant("session-root", usage(400)) })
  event(plugin, "evt-compaction", "session.compacted", { sessionID: "session-root" })
  event(plugin, "evt-compaction-next", "session.compacted", { sessionID: "session-root" })
})
assert.deepEqual(compactions.flatMap((body) => body.context_updates).map((update) =>
  [update.event_id, update.compacted ?? false]), [
  ["evt-compaction", true], ["evt-after", false], ["evt-compaction", true], ["evt-compaction-next", true],
], "native successful identity remains stable across usage and redelivery for host-owned deduplication")
assert(compactions.every((body) => body.context_updates.every((update) =>
  update.compaction_count === undefined && update.compaction_count_revision === undefined &&
  update.compaction_count_complete === undefined)), "observed events cannot prove unobserved history")

const invalid_usage = await run((plugin) => {
  root(plugin)
  event(plugin, "evt-negative", "message.updated", { info: assistant("session-root", usage(-1)) })
  event(plugin, "evt-overflow", "message.updated", {
    info: assistant("session-root", usage(Number.MAX_SAFE_INTEGER), "other-model"),
  })
})
assert.deepEqual(invalid_usage.map((body) => body.context_updates[0]), [
  { provider_session_id: "session-root", event_id: "evt-negative", usage_unavailable: true },
  { provider_session_id: "session-root", event_id: "evt-overflow", usage_unavailable: true },
])

const missing_capacity = await run((plugin) => {
  root(plugin)
  event(plugin, "evt-capacity-failed", "message.updated", { info: assistant("session-root", usage()) })
  event(plugin, "evt-after-failure", "session.compacted", { sessionID: "session-root" })
}, client(async () => { throw new Error("provider catalogue unavailable") }))
assert.deepEqual(missing_capacity[0].context_updates[0], {
  provider_session_id: "session-root", event_id: "evt-capacity-failed", used_tokens: 1330,
})
assert.equal(missing_capacity[1].context_updates[0].compacted, true,
  "optional model lookup failure cannot stop later native event delivery")

let catalogue_calls = 0
const recovered_capacity = await run((plugin) => {
  root(plugin)
  event(plugin, "evt-before-catalogue", "message.updated", { info: assistant("session-root", usage()) })
  event(plugin, "evt-before-model", "message.updated", { info: assistant("session-root", usage()) })
  event(plugin, "evt-after-catalogue", "message.updated", { info: assistant("session-root", usage()) })
}, client(async () => {
  if (++catalogue_calls === 1) throw new Error("provider catalogue unavailable")
  if (catalogue_calls === 2) return { data: { all: [] } }
  return { data: { all: [{
    id: "provider-context", models: { "model-context": { limit: { context: 4000 } } },
  }] } }
}))
assert.equal(recovered_capacity.length, 2, "unchanged native usage can recover an unavailable model capacity")
assert.equal(catalogue_calls, 3, "lookup failure and a missing model both remain retryable")
assert.equal(recovered_capacity[1].context_updates[0].context_window, 4000)

let zero_window_calls = 0
const unknown_window = await run((plugin) => {
  root(plugin)
  event(plugin, "evt-zero-window", "message.updated", { info: assistant("session-root", usage()) })
  event(plugin, "evt-more-usage", "message.updated", { info: assistant("session-root", usage(2000)) })
}, client(async () => {
  ++zero_window_calls
  return { data: { all: [{
    id: "provider-context", models: { "model-context": { limit: { context: 0 } } },
  }] } }
}))
assert.deepEqual(unknown_window.map((body) => body.context_updates[0]), [
  { provider_session_id: "session-root", event_id: "evt-zero-window", used_tokens: 1330 },
  { provider_session_id: "session-root", event_id: "evt-more-usage", used_tokens: 2330 },
])
assert.equal(zero_window_calls, 1, "a catalogue-confirmed zero window is stable for the current model")

const isolated = await run((plugin) => {
  root(plugin)
  event(plugin, "evt-child-created", "session.created", { info: { id: "session-child", parentID: "session-root" } })
  event(plugin, "evt-child-usage", "message.updated", { info: assistant("session-child", usage()) })
  event(plugin, "evt-child-compaction", "session.compacted", { sessionID: "session-child" })
  event(plugin, undefined, "session.compacted", { sessionID: "session-root" })
  event(plugin, "evt-bad\nidentity", "message.updated", { info: assistant("session-root", usage()) })
  event(plugin, "evt-first", "message.updated", { info: assistant("session-root", usage()) })
  event(plugin, "evt-deleted", "session.deleted", { info: { id: "session-root" } })
  root(plugin, "session-next")
  event(plugin, "evt-new-session", "message.updated", { info: assistant("session-next", usage()) })
})
assert.deepEqual(isolated.flatMap((body) => body.context_updates ?? []).map((update) =>
  [update.provider_session_id, update.event_id]), [
  ["session-root", "evt-first"], ["session-next", "evt-new-session"],
], "child, unidentified, and invalid observations cannot enter the root context stream")

console.log("OpenCode native context producer fixtures passed")
