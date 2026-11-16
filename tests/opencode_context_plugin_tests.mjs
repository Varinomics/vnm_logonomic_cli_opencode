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
const request_path = join(scratch, "requests.jsonl")
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
  OPENCODE_TEST_REQUEST_CAPTURE: request_path,
})
const { Logonomic } = await import(pathToFileURL(resolve(plugin_path)))
const read_lines = (path) => {
  const text = readFileSync(path, "utf8").trim()
  return text ? text.split("\n").map(JSON.parse) : []
}
const captured = () => read_lines(capture_path)
// The helper's own request carries the transition nonce the host fences.
const requests = () => read_lines(request_path)
const event = (plugin, id, type, properties) => plugin.event({ event: { id, type, properties } })
const assistant = (session_id, tokens, model_id = "model-context") => ({
  id: `msg-${session_id}`, sessionID: session_id, parentID: "turn-user", role: "assistant",
  providerID: "provider-context", modelID: model_id, tokens,
  cost: 100_000, text: "PRIVATE_NATIVE_CONTENT",
})
const usage = (input = 1000) => ({
  input, output: 80, reasoning: 20, cache: { read: 200, write: 30 }, total: 99,
})
// OpenCode 1.18.33 writes a dispatched assistant record (prompt.ts:1186-1201),
// publishes busy status (processor.ts:653), then the step-start part of that
// record (processor.ts:424-432). An input's first record selects its root at
// that status; a running session's later status changes nothing.
const step_start = (plugin, record) => event(plugin, `evt-step-${record.id}`, "message.part.updated", {
  part: { id: `prt-step-${record.id}`, sessionID: record.sessionID, messageID: record.id, type: "step-start" },
})
const dispatched = (plugin, id, info) => {
  event(plugin, id, "message.updated", { info })
  step_start(plugin, info)
}
const first_dispatched = (plugin, id, status_id, info) => {
  event(plugin, id, "message.updated", { info })
  event(plugin, status_id, "session.status", { sessionID: info.sessionID, status: { type: "busy" } })
  step_start(plugin, info)
}
// A retained fact may ride several carriers; its activation and sequence identify it.
const distinct_facts = (bodies) => [...new Map(bodies.flatMap((entry) => entry.model_state?.updates ?? [])
  .map((update) => [`${update.transition_nonce}/${update.sequence}`, update])).values()]
// A supplied native store answers lookups; otherwise every session is a root.
const lookups = []
const client = (provider_lookup = async () => ({ data: { all: [{
  id: "provider-context", models: { "model-context": { limit: { context: 4000 } } },
}] } }), sessions = undefined) => ({
  session: {
    get: async ({ path }) => {
      lookups.push(path.id)
      return { data: sessions ? sessions.get(path.id) : { id: path.id } }
    },
    messages: () => { throw new Error("Context events must not hydrate conversation text") },
  },
  provider: { list: provider_lookup },
})
const root = (plugin, session_id = "session-root") =>
  event(plugin, `evt-create-${session_id}`, "session.created", { info: { id: session_id } })
const run = async (emit, native_client = client()) => {
  writeFileSync(capture_path, "")
  writeFileSync(request_path, "")
  lookups.length = 0
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
  step_start(plugin, root_model)
  event(plugin, "evt-child-created", "session.created", { info: { id: "session-child", parentID: "session-root" } })
  event(plugin, "evt-child-start", "session.status", { sessionID: "session-child", status: { type: "busy" } })
  dispatched(plugin, "evt-child-model", assistant("session-child", usage(), "child-model"))
  event(plugin, "evt-child-stop", "session.status", { sessionID: "session-child", status: { type: "idle" } })
  dispatched(plugin, "evt-child-late-model", assistant("session-child", usage(), "late-stopped-model"))
  event(plugin, "evt-child-restart", "session.status", { sessionID: "session-child", status: { type: "busy" } })
  event(plugin, "evt-child-restop", "session.status", { sessionID: "session-child", status: { type: "idle" } })
  dispatched(plugin, "evt-summary-model", { ...assistant("session-root", usage(), "summary-model"), summary: true })
  root(plugin, "session-other")
  await plugin["chat.message"]({ sessionID: "session-other" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-other-model", "evt-other-start", assistant("session-other", usage(), "other-model"))
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-returned" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-return-model", "evt-return-start",
    { ...assistant("session-root", usage(), "returned-model"), parentID: "turn-returned" })
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
assert.deepEqual(distinct_facts(model_facts).filter((update) => !update.agent_id).map((update) =>
  [update.provider_session_id, update.model, update.transition_nonce]), [
  ["session-root", "provider-context/root-model", starts[0].activation_nonce],
  ["session-other", "provider-context/other-model", starts[1].activation_nonce],
  ["session-root", "provider-context/returned-model", starts[2].activation_nonce],
], "initial model evidence uses the same captured nonce as its root transition")
const child_starts = model_facts.filter((entry) => entry.event === "logonomic.subagent.started")
assert.equal(child_starts.length, 2)
assert.equal(child_starts[1].model_state, undefined, "a repeated child run starts without inherited model evidence")

const sequential_children = await run(async (plugin) => {
  root(plugin)
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-root-confirmed", "evt-root-busy", assistant("session-root", usage()))
  // Drain each real helper delivery before the next native child. This tests
  // inactive source eviction rather than the independent event-queue bound.
  for (let index = 0; index < 130; ++index) {
    const id = `sequential-child-${index}`
    event(plugin, `evt-create-${id}`, "session.created", { info: { id, parentID: "session-root" } })
    event(plugin, `evt-start-${id}`, "session.status", { sessionID: id, status: { type: "busy" } })
    dispatched(plugin, `evt-model-${id}`, assistant(id, usage(), `model-${index}`))
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

// OpenCode 1.18.33 emits chat.message before session.updated for a pre-existing
// session. The lifecycle still admits it as a root through the native lookup,
// so the host retires the previous root transition while the plugin has no
// source evidence about the prompted session.
const unobserved_switch = await run(async (plugin) => {
  root(plugin)
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-root-model", "evt-root-start", assistant("session-root", usage(), "root-model"))
  await plugin["chat.message"]({ sessionID: "session-existing" }, {
    message: { id: "turn-existing" }, parts: [{ synthetic: false }],
  })
  event(plugin, "evt-existing-touched", "session.updated", { info: { id: "session-existing" } })
  first_dispatched(plugin, "evt-existing-model", "evt-existing-start",
    { ...assistant("session-existing", usage(), "existing-model"), parentID: "turn-existing" })
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-returned" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-return-model", "evt-return-start",
    { ...assistant("session-root", usage(), "returned-model"), parentID: "turn-returned" })
})
const switch_starts = unobserved_switch.filter((entry) => entry.event === "session.status" && entry.fresh_user_turn)
assert.deepEqual(switch_starts.map((entry) => entry.session_id), ["session-root", "session-existing", "session-root"])
const switch_nonces = requests().filter((request) => request.event.body.fresh_user_turn)
  .map((request) => request.transition_nonce)
assert.equal(new Set(switch_nonces).size, 3,
  "each host root transition opens under a nonce no earlier transition used")
assert.equal(switch_starts[1].activation_nonce, switch_nonces[1],
  "the session's own native fact admits it as a root; its selected input opens its transition")
assert.equal(switch_starts[2].activation_nonce, switch_nonces[2])
assert.deepEqual(distinct_facts(unobserved_switch).map((update) => [update.model, update.transition_nonce]), [
  ["provider-context/root-model", switch_nonces[0]],
  ["provider-context/existing-model", switch_nonces[1]],
  ["provider-context/returned-model", switch_nonces[2]],
], "the returned root's model evidence belongs to its new source-captured activation")

// With no session event at all, the native lookup alone decides the prompted
// session. Delegated work continued in an existing child keeps the root's one
// host transition, its model facts and the child's own facts; an existing root
// opens its own transition.
const prompted_existing = (existing) => run(async (plugin) => {
  root(plugin)
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-root-model", "evt-root-start", assistant("session-root", usage(), "root-model"))
  await plugin["chat.message"]({ sessionID: "session-existing" }, {
    message: { id: "turn-existing" }, parts: [{ synthetic: false }],
  })
  event(plugin, "evt-existing-start", "session.status", { sessionID: "session-existing", status: { type: "busy" } })
  first_dispatched(plugin, "evt-existing-model", "evt-existing-confirmed",
    { ...assistant("session-existing", usage(), "existing-model"), parentID: "turn-existing" })
  event(plugin, "evt-existing-stop", "session.status", { sessionID: "session-existing", status: { type: "idle" } })
  dispatched(plugin, "evt-root-later-model", assistant("session-root", usage(), "root-later-model"))
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-again" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-again-model", "evt-again-start",
    { ...assistant("session-root", usage(), "root-again-model"), parentID: "turn-again" })
}, client(undefined, new Map([["session-root", { id: "session-root" }], ["session-existing", existing]])))

const continued_child = await prompted_existing({ id: "session-existing", parentID: "session-root" })
assert(lookups.includes("session-existing"), "the native lookup decides the unobserved session")
assert.deepEqual(continued_child.filter((entry) => entry.event !== "message.updated").map((entry) =>
  [entry.event, entry.session_id, entry.agent_id, entry.fresh_user_turn]), [
  ["session.status", "session-root", undefined, true],
  ["logonomic.subagent.started", "session-root", "session-existing", undefined],
  ["logonomic.subagent.stopped", "session-root", "session-existing", undefined],
  ["session.status", "session-root", undefined, true],
], "the lookup admits the prompted session as the root's child, without a host transition")
const [child_transition] = requests().filter((request) => request.event.body.fresh_user_turn)
  .map((request) => request.transition_nonce)
assert.deepEqual([...new Set(continued_child.flatMap((entry) => entry.activation_nonce ?? []))], [child_transition],
  "a child prompt and the root's next input keep the root's activation")
assert.deepEqual(distinct_facts(continued_child).map((update) =>
  [update.agent_id, update.provider_session_id, update.model, update.transition_nonce]), [
  ["", "session-root", "provider-context/root-model", child_transition],
  ["session-existing", "session-root", "provider-context/existing-model", child_transition],
  ["", "session-root", "provider-context/root-later-model", child_transition],
  ["", "session-root", "provider-context/root-again-model", child_transition],
], "root facts continue and the child reports only its own model")

const existing_root = await prompted_existing({ id: "session-existing" })
assert(lookups.includes("session-existing"))
const root_starts = existing_root.filter((entry) => entry.event === "session.status" && entry.fresh_user_turn)
const root_nonces = requests().filter((request) => request.event.body.fresh_user_turn)
  .map((request) => request.transition_nonce)
assert.deepEqual(root_starts.map((entry) => [entry.session_id, entry.activation_nonce]), [
  ["session-root", root_nonces[0]], ["session-existing", root_nonces[1]], ["session-root", root_nonces[2]],
], "the lookup admits an existing root, and each root transition uses its selected input's nonce")
assert.equal(new Set(root_nonces).size, 3)
assert.deepEqual(distinct_facts(existing_root).map((update) =>
  [update.agent_id, update.model, update.transition_nonce]), [
  ["", "provider-context/root-model", root_nonces[0]],
  ["", "provider-context/existing-model", root_nonces[1]],
  ["", "provider-context/root-again-model", root_nonces[2]],
], "each root's facts belong to its own transition; an inactive root's late reply has none")

// Nested delegation starts each child after its parent, so the lifecycle's
// cached root admits any depth. Model attribution follows that admission; a
// cycle reaches no root.
const nested_depth = 20
const nested = await run(async (plugin) => {
  root(plugin)
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-root-confirmed", "evt-root-busy", assistant("session-root", usage()))
  for (let depth = 1; depth <= nested_depth; ++depth) {
    const id = `nested-${depth}`
    const parentID = depth === 1 ? "session-root" : `nested-${depth - 1}`
    event(plugin, `evt-create-${id}`, "session.created", { info: { id, parentID } })
    event(plugin, `evt-start-${id}`, "session.status", { sessionID: id, status: { type: "busy" } })
    const deadline = Date.now() + 5000
    while (!captured().some((entry) => entry.event === "logonomic.subagent.started" && entry.agent_id === id)) {
      assert(Date.now() < deadline, `nested child ${id} was not admitted`)
      await new Promise((resolve) => setTimeout(resolve, 10))
    }
  }
  dispatched(plugin, "evt-nested-model", assistant(`nested-${nested_depth}`, usage(), "nested-model"))
  event(plugin, "evt-cycle-a", "session.created", { info: { id: "cycle-a", parentID: "cycle-b" } })
  event(plugin, "evt-cycle-b", "session.created", { info: { id: "cycle-b", parentID: "cycle-a" } })
  event(plugin, "evt-cycle-start", "session.status", { sessionID: "cycle-a", status: { type: "busy" } })
  dispatched(plugin, "evt-cycle-model", assistant("cycle-a", usage(), "cycle-model"))
})
const nested_updates = nested.flatMap((entry) => entry.model_state?.updates ?? []).filter((update) => update.agent_id)
assert.deepEqual(nested_updates.map((update) => [update.agent_id, update.provider_session_id, update.model]), [
  [`nested-${nested_depth}`, "session-root", "provider-context/nested-model"],
], "a lifecycle-admitted nested child keeps its own model past any fixed walk depth; a cycle reaches no root")

// Oracle: OpenCode 1.18.33 prompt.ts handleSubtask and shellImpl write
// assistant records that name a model but dispatch no root model step, while
// processor.ts publishes a step-start part for the assistant message it
// dispatches. Every record starts with zero tokens, so only that step's message
// identity shows that a model served it. A subtask command's request still
// selects the root model; it has not served the root until a later step does.
const no_tokens = () => ({ input: 0, output: 0, reasoning: 0, cache: { read: 0, write: 0 } })
const native_record = (session_id, id, parentID, model_id, agent = "build") => ({
  ...assistant(session_id, no_tokens(), model_id), id, parentID, agent, mode: agent, cost: 0,
})
const completed = (record, update = {}) => ({ ...record, ...update, time: { created: 1, completed: 2 } })
const status = (plugin, id, session_id, type) => event(plugin, id, "session.status", {
  sessionID: session_id, status: { type },
})
const subtask_command = (plugin, turn_id) => {
  const selected = { providerID: "provider-context", modelID: "root-model" }
  return plugin["chat.message"]({ sessionID: "session-root", agent: "build", model: selected }, {
    message: { id: turn_id, role: "user", model: selected },
    parts: [{ type: "subtask", agent: "task-agent", description: "", command: "delegate",
      model: { providerID: "provider-context", modelID: "task-model" }, prompt: "PRIVATE_NATIVE_CONTENT" }],
  })
}
const source_authority = await run(async (plugin) => {
  root(plugin)
  // The first command runs its task before any root step serves the root.
  await subtask_command(plugin, "turn-command-first")
  status(plugin, "evt-first-loop", "session-root", "busy")
  const wrapper_first = native_record("session-root", "msg-wrapper-first", "turn-command-first", "task-model",
    "task-agent")
  event(plugin, "evt-wrapper-first", "message.updated", { info: wrapper_first })
  event(plugin, "evt-wrapper-first-done", "message.updated", {
    info: completed(wrapper_first, { finish: "tool-calls" }),
  })
  status(plugin, "evt-first-summary-loop", "session-root", "busy")
  const served_first = native_record("session-root", "msg-served-first", "turn-summary-first", "root-model")
  event(plugin, "evt-served-first", "message.updated", { info: served_first })
  step_start(plugin, served_first)
  event(plugin, "evt-served-first-done", "message.updated", {
    info: completed(served_first, { tokens: usage(), finish: "stop" }),
  })
  status(plugin, "evt-first-exit", "session-root", "busy")
  status(plugin, "evt-first-idle", "session-root", "idle")
  // The admitted root delegates again, while its child serves the task model.
  await subtask_command(plugin, "turn-command-next")
  status(plugin, "evt-next-loop", "session-root", "busy")
  const wrapper_next = native_record("session-root", "msg-wrapper-next", "turn-command-next", "task-model",
    "task-agent")
  event(plugin, "evt-wrapper-next", "message.updated", { info: wrapper_next })
  event(plugin, "evt-task-created", "session.created", { info: { id: "session-task", parentID: "session-root" } })
  status(plugin, "evt-task-busy", "session-task", "busy")
  const task_step = native_record("session-task", "msg-task", "turn-task", "task-model", "task-agent")
  event(plugin, "evt-task-model", "message.updated", { info: task_step })
  step_start(plugin, task_step)
  status(plugin, "evt-task-idle", "session-task", "idle")
  event(plugin, "evt-wrapper-next-done", "message.updated", {
    info: completed(wrapper_next, { finish: "tool-calls" }),
  })
  status(plugin, "evt-next-summary-loop", "session-root", "busy")
  const served_next = native_record("session-root", "msg-served-next", "turn-summary-next", "root-model")
  event(plugin, "evt-served-next", "message.updated", { info: served_next })
  step_start(plugin, served_next)
  event(plugin, "evt-served-next-done", "message.updated", {
    info: completed(served_next, { tokens: usage(), finish: "stop" }),
  })
  status(plugin, "evt-next-exit", "session-root", "busy")
  status(plugin, "evt-next-idle", "session-root", "idle")
  // A user shell action records its model, but no model step runs.
  const shell = native_record("session-root", "msg-shell", "turn-shell", "shell-model")
  event(plugin, "evt-shell", "message.updated", { info: shell })
  event(plugin, "evt-shell-done", "message.updated", { info: completed(shell) })
  // A genuine dispatch serves its model from step start, still with zero tokens.
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-prompt" }, parts: [{ synthetic: false }],
  })
  status(plugin, "evt-prompt-loop", "session-root", "busy")
  const served_prompt = native_record("session-root", "msg-served-prompt", "turn-prompt", "prompt-model")
  event(plugin, "evt-served-prompt", "message.updated", { info: served_prompt })
  served_prompt.modelID = "mutated-after-observation"
  step_start(plugin, served_prompt)
})
const authority_starts = source_authority.filter((entry) => entry.event === "session.status" && entry.fresh_user_turn)
const authority_facts = distinct_facts(source_authority)
const authority_root = authority_facts.filter((update) => !update.agent_id).map((update) => update.model)
assert.deepEqual({
  starts: authority_starts.map((entry) => entry.model_state?.updates.map((update) => update.model) ?? []),
  root: authority_root.filter((model, index) => model !== authority_root[index - 1]),
  children: [...new Set(authority_facts.filter((update) => update.agent_id)
    .map((update) => `${update.agent_id} ${update.model}`))],
  nonces: [...new Set(authority_facts.map((update) => update.transition_nonce))],
}, {
  starts: [[], ["provider-context/root-model"]],
  root: ["provider-context/root-model", "provider-context/prompt-model"],
  children: ["session-task provider-context/task-model"],
  nonces: [authority_starts[0]?.activation_nonce],
}, "only a record's own native step confirms its captured model; task wrappers, shell records and request " +
  "selections cannot, while a child confirms its own model under the source activation")

// Oracle: OpenCode v1 hands every bus event to plugin hooks as
// `void hook["event"](...)` (plugin/index.ts:255-262 at 51ef4be1) without
// awaiting plugin work, and task.ts runs children concurrently. Their step-start
// parts therefore keep arriving while serial projection awaits a native lookup.
// The root usage update holds projection in the catalogue lookup behind an
// explicit started/release handshake. The lookup's 250 ms deadline is a timer,
// and every arrival from the started signal to the release runs before any
// timer can fire. Each loop iteration publishes busy, and the loop then ends
// idle (prompt.ts:1089; the observed completion order in the plugin tests).
// Optional model steps cannot charge core capacity, displace completion or close
// admission, and each child keeps its own confirmed model.
const held_children = 32 // the plugin's core admission bound
let lookup_started
const lookup_called = new Promise((resolve) => { lookup_started = resolve })
let release_lookup
const held_catalogue = () => {
  lookup_started()
  return new Promise((resolve) => {
    release_lookup = () => resolve({ data: { all: [{
      id: "provider-context", models: { "root-model": { limit: { context: 4000 } } },
    }] } })
  })
}
const held_lookup = await run(async (plugin) => {
  root(plugin)
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-user" }, parts: [{ synthetic: false }],
  })
  const root_reply = native_record("session-root", "msg-root-reply", "turn-user", "root-model")
  first_dispatched(plugin, "evt-root-reply", "evt-root-start", root_reply)
  // Each child's run starts and its dispatched record awaits the model response.
  // Each lifecycle delivery drains, so only the held arrivals below are queued.
  const child_records = []
  for (let index = 0; index < held_children; ++index) {
    const id = `held-child-${index}`
    event(plugin, `evt-create-${id}`, "session.created", { info: { id, parentID: "session-root" } })
    status(plugin, `evt-loop-${id}`, id, "busy")
    child_records.push(native_record(id, `msg-${id}`, `turn-${id}`, `child-model-${index}`))
    event(plugin, `evt-record-${id}`, "message.updated", { info: child_records[index] })
    status(plugin, `evt-dispatch-${id}`, id, "busy")
    const deadline = Date.now() + 5000
    while (!captured().some((entry) => entry.event === "logonomic.subagent.started" && entry.agent_id === id)) {
      assert(Date.now() < deadline, `held child ${id} did not start`)
      await new Promise((resolve) => setTimeout(resolve, 10))
    }
  }
  event(plugin, "evt-root-usage", "message.updated", {
    info: completed(root_reply, { tokens: usage(), finish: "stop" }),
  })
  let handshake
  await Promise.race([lookup_called, new Promise((_, reject) => {
    handshake = setTimeout(() => reject(new Error("the root usage projection must start its catalogue lookup")), 5000)
  })]).finally(() => clearTimeout(handshake))
  for (const record of child_records) step_start(plugin, record)
  status(plugin, "evt-root-exit", "session-root", "busy")
  status(plugin, "evt-root-idle", "session-root", "idle")
  event(plugin, "evt-root-idle-session", "session.idle", { sessionID: "session-root" })
  release_lookup()
  // Admission stays open for the root's next input.
  await plugin["chat.message"]({ sessionID: "session-root" }, {
    message: { id: "turn-next" }, parts: [{ synthetic: false }],
  })
  first_dispatched(plugin, "evt-next-reply", "evt-next-start",
    native_record("session-root", "msg-next-reply", "turn-next", "root-next-model"))
}, client(held_catalogue))
const held_activation = held_lookup.find((entry) => entry.fresh_user_turn)?.activation_nonce
const held_idle = held_lookup.findIndex((entry) => entry.event === "session.status" && entry.status_type === "idle")
const held_facts = distinct_facts(held_lookup)
const held_child_facts = held_facts.filter((update) => update.agent_id)
assert.deepEqual({
  overflow: held_lookup.some((entry) => entry.event === "logonomic.delivery_overflow"),
  core: held_lookup.filter((entry) => entry.event !== "message.updated").map((entry) => [entry.event,
    entry.agent_id ?? "", entry.status_type ?? "", entry.turn_id ?? "", entry.fresh_user_turn === true]),
  context: held_lookup.flatMap((entry) => entry.context_updates ?? []).map((update) =>
    [update.event_id, update.used_tokens, update.context_window]),
  root: held_facts.filter((update) => !update.agent_id).map((update) =>
    [update.model, update.transition_nonce, update.source]),
  children: held_child_facts.map((update) => [update.agent_id, update.provider_session_id, update.model,
    update.transition_nonce, update.source]).sort(),
  children_before_completion: held_child_facts.every((update) => held_lookup.findIndex((entry) =>
    entry.model_state?.updates.some((carried) => carried.agent_id === update.agent_id)) < held_idle),
}, {
  overflow: false,
  core: [
    ["session.status", "", "busy", "turn-user", true],
    ...Array.from({ length: held_children }, (_, index) =>
      ["logonomic.subagent.started", `held-child-${index}`, "", "", false]),
    ["session.status", "", "busy", "turn-user", false],
    ["session.status", "", "idle", "turn-user", false],
    ["session.status", "", "busy", "turn-next", true],
  ],
  context: [["evt-root-usage", 1330, 4000]],
  root: [
    ["provider-context/root-model", held_activation, "opencode.message.updated"],
    ["provider-context/root-next-model", held_activation, "opencode.message.updated"],
  ],
  children: Array.from({ length: held_children }, (_, index) => [`held-child-${index}`, "session-root",
    `provider-context/child-model-${index}`, held_activation, "opencode.message.updated"]).sort(),
  children_before_completion: true,
}, "optional model steps behind a held native lookup cannot charge core capacity, lose completion or close " +
  "admission, and each child keeps its own confirmed model")

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
