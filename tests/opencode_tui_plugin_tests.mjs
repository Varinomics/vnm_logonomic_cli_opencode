import assert from "node:assert/strict"
import { spawnSync } from "node:child_process"
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
process.env.OPENCODE_LOGONOMIC_WAKE_REGISTRATION = JSON.stringify(registration)
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
const private_canary = "PRIVATE_NATIVE_PROMPT_AND_ANSWER"
const permission = (id, sessionID = "ses_root") => ({ id, sessionID, action: private_canary, resources: [private_canary] })
const form = (id, sessionID = "ses_root") => ({
  id, sessionID, title: private_canary, fields: [{ key: "value", type: "string", title: private_canary }],
})

async function attention_events(scenario) {
  await writeFile(capture, "")
  let route = { type: "session", sessionID: "ses_root" }
  let listener
  const sessions = new Map([["ses_root", { id: "ses_root" }], ["ses_other", { id: "ses_other" }]])
  const permissions = new Map()
  const forms = new Map()
  const families = new Map()
  integration = await create_tui_integration({
    ui: { router: { current: () => route } },
    data: {
      listen(callback) { listener = callback; return () => { listener = undefined } },
      session: {
        get: (id) => sessions.get(id), status: () => "running", pending: { list: () => [] },
        family: (id) => families.get(id) ?? [id],
        permission: { list: (id) => permissions.get(id) }, form: { list: (id) => forms.get(id) },
      },
    },
    client: {
      session: {
        synthetic: async () => {}, get: async () => {}, active: async () => ({}),
        inbox: { list: async () => [] }, form: { list: async () => [] },
      },
      permission: { list: async () => [] },
    },
  })
  await scenario({
    sessions, permissions, forms, families,
    route(value) { route = value },
    async select(sessionID) {
      if (sessionID) route = { type: "session", sessionID }
      integration.select()
      await Promise.resolve()
    },
    async event(type, data, id) {
      // Native reactive data updates can rerun the selection effect before the
      // same native event reaches plugin listeners.
      integration.select()
      listener({ details: { type, data, id } })
      await Promise.resolve()
    },
  })
  await integration.dispose()
  integration = undefined
  assert.equal(listener, undefined)
  const text = await readFile(capture, "utf8")
  assert.equal(text.includes(private_canary), false, "native prompt and answer prose must stay in OpenCode")
  return text.trim().split("\n").filter(Boolean).map(JSON.parse)
}

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
  // An invalid optional wake field must leave native selection and attention operational.
  process.env.OPENCODE_LOGONOMIC_WAKE_REGISTRATION = "{invalid"
  if (process.argv[3]) {
    const selected_events = await attention_events(async ({ sessions, permissions, forms, select, event, route }) => {
      permissions.set("ses_root", [permission("per_first"), permission("per_remaining")])
      forms.set("ses_root", [form("frm_existing")])
      await select()
      await select()
      await select("ses_other")
      permissions.set("ses_root", [permission("per_remaining")])
      await event("permission.replied", { sessionID: "ses_root", requestID: "per_first", reply: "once" })
      await select("ses_root")
      route({ type: "home" })
      await select()
      await select("ses_root")
      sessions.set("ses_child", { id: "ses_child", parentID: "ses_root" })
      await select("ses_child")
      await select("ses_root")
      await select("ses_hydrating")
      sessions.set("ses_hydrating", { id: "ses_hydrating" })
      await select()
      permissions.set("ses_hydrating", [permission("per_hydrated", "ses_hydrating")])
      await select()
      forms.set("ses_hydrating", [form("frm_hydrated", "ses_hydrating"), form("frm_global", "global")])
      await select()
      await select()
    })
    assert.deepEqual(selected_events.filter((event) => event.event === "logonomic.session.selected")
      .map((event) => event.session_id), ["ses_root", "ses_other", "ses_root", "ses_root", "ses_root", "ses_hydrating"],
    "only visible roots are admitted; returning from a hidden route keeps the same native session")
    assert.deepEqual(selected_events.filter((event) => event.permission_id || event.question_id)
      .map((event) => [event.event, event.session_id, event.permission_id ?? event.question_id, event.turn_id]), [
      ["permission.asked", "ses_root", "per_first", undefined],
      ["permission.asked", "ses_root", "per_remaining", undefined],
      ["form.created", "ses_root", "frm_existing", undefined],
      ["permission.asked", "ses_root", "per_remaining", undefined],
      ["form.created", "ses_root", "frm_existing", undefined],
      ["permission.asked", "ses_hydrating", "per_hydrated", undefined],
      ["form.created", "ses_hydrating", "frm_hydrated", undefined],
    ], "selection and delayed hydration restore each unanswered request, without borrowing a turn or repeating settled requests")

    const reply_events = await attention_events(async ({ permissions, forms, select, event }) => {
      permissions.set("ses_root", [])
      forms.set("ses_root", [form("frm_attached")])
      await select()
      await event("session.execution.started", { sessionID: "ses_root" }, "evt_first")
      permissions.set("ses_root", [permission("per_live")])
      await event("permission.asked", permission("per_live"))
      forms.set("ses_root", [form("frm_attached"), form("frm_live"), form("frm_remaining")])
      await event("form.created", { form: form("frm_live") })
      await event("form.created", { form: form("frm_remaining") })
      permissions.set("ses_root", [])
      await event("permission.replied", { sessionID: "ses_root", requestID: "per_live", reply: "once" })
      forms.set("ses_root", [form("frm_attached"), form("frm_remaining")])
      await event("form.replied", { sessionID: "ses_root", id: "frm_live", answer: { value: false } })
      await event("session.execution.succeeded", { sessionID: "ses_root" })
      forms.set("ses_root", [form("frm_attached")])
      await event("form.cancelled", { sessionID: "ses_root", id: "frm_remaining" })
      await event("session.execution.started", { sessionID: "ses_root" }, "evt_next")
      forms.set("ses_root", [])
      await event("form.replied", { sessionID: "ses_root", id: "frm_attached", answer: { value: private_canary } })
      forms.set("ses_root", [form("frm_empty")])
      await event("form.created", { form: form("frm_empty") })
      forms.set("ses_root", [])
      await event("form.replied", { sessionID: "ses_root", id: "frm_empty", answer: { value: "  " } })
    })
    assert.deepEqual(reply_events.filter((event) => event.permission_id || event.question_id)
      .map((event) => [event.event, event.permission_id ?? event.question_id, event.turn_id, event.answer_nonempty]), [
      ["form.created", "frm_attached", undefined, undefined],
      ["permission.asked", "per_live", "evt_first", undefined],
      ["form.created", "frm_live", "evt_first", undefined],
      ["form.created", "frm_remaining", undefined, undefined],
      ["permission.replied", "per_live", "evt_first", undefined],
      ["form.replied", "frm_live", "evt_first", true],
      ["form.cancelled", "frm_remaining", undefined, undefined],
      ["form.replied", "frm_attached", undefined, true],
      ["form.created", "frm_empty", "evt_next", undefined],
      ["form.replied", "frm_empty", "evt_next", false],
    ], "pending requests resolve independently across completion; hydrated replies never acquire a later execution's turn")

    for (const later_execution of ["running", "completed"]) {
      const cross_turn_events = await attention_events(async ({ permissions, forms, select, event }) => {
        permissions.set("ses_root", [])
        forms.set("ses_root", [])
        await select()
        await event("session.execution.started", { sessionID: "ses_root" }, "evt_first")
        permissions.set("ses_root", [permission("per_retained")])
        await event("permission.asked", permission("per_retained"))
        forms.set("ses_root", [form("frm_retained")])
        await event("form.created", { form: form("frm_retained") })
        await event("session.execution.succeeded", { sessionID: "ses_root" })
        await event("session.execution.started", { sessionID: "ses_root" }, "evt_next")
        if (later_execution === "completed") {
          await event("session.execution.succeeded", { sessionID: "ses_root" })
        }
        permissions.set("ses_root", [])
        await event("permission.replied", { sessionID: "ses_root", requestID: "per_retained", reply: "once" })
        forms.set("ses_root", [])
        await event("form.replied", { sessionID: "ses_root", id: "frm_retained", answer: { value: private_canary } })
        await select()
      })
      const prompts = cross_turn_events.filter((event) => event.permission_id || event.question_id)
      assert.deepEqual(prompts.filter((event) => event.event === "permission.asked" || event.event === "form.created")
        .map((event) => [event.permission_id ?? event.question_id, event.turn_id]),
      [["per_retained", "evt_first"], ["frm_retained", "evt_first"]],
      "live requests retain their observed originating execution")
      assert.equal(prompts.some((event) => event.turn_id === "evt_next"), false,
        "a retained request must never be attributed to a later execution")
      // The host rejects a nonempty superseded turn before attention reduction,
      // while confirmed typed requests remain pending across execution starts.
      const resolutions = prompts.filter((event) =>
        ["permission.replied", "form.replied", "logonomic.permission.resolved", "logonomic.form.resolved"].includes(event.event) &&
        (!event.turn_id || event.turn_id === "evt_next"))
      assert.deepEqual(resolutions.map((event) => event.permission_id ?? event.question_id).sort(),
        ["frm_retained", "per_retained"],
        `both retained requests require admissible resolution after the next execution is ${later_execution}`)
      assert.equal(prompts.find((event) => event.event === "form.replied")?.answer_nonempty, true,
        "cross-turn settlement preserves the actual native form reply evidence")
    }

    const refreshed_events = await attention_events(async ({ permissions, forms, select, event, route }) => {
      permissions.set("ses_root", [permission("per_existing")])
      forms.set("ses_root", [form("frm_existing"), form("frm_remaining")])
      await select()
      await event("session.execution.started", { sessionID: "ses_root" }, "evt_live")
      forms.set("ses_root", [form("frm_existing"), form("frm_remaining"), form("frm_live")])
      await event("form.created", { form: form("frm_live") })
      permissions.set("ses_root", [])
      forms.set("ses_root", [form("frm_remaining")])
      await select()
      await event("form.replied", { sessionID: "ses_root", id: "frm_live", answer: { value: 0 } })
      await event("form.replied", { sessionID: "ses_root", id: "frm_live", answer: { value: private_canary } })
      permissions.set("ses_root", undefined)
      forms.set("ses_root", undefined)
      await select()
      permissions.set("ses_root", [permission("per_existing"), permission("per_other", "ses_other")])
      forms.set("ses_root", [form("frm_existing"), form("frm_live"), form("frm_remaining")])
      await select()
      await event("form.cancelled", { sessionID: "ses_other", id: "frm_remaining" })
      await event("session.execution.failed", { sessionID: "ses_other" })
      permissions.set("ses_other", [permission("per_next", "ses_other")])
      forms.set("ses_other", [form("frm_next", "ses_other")])
      route({ type: "session", sessionID: "ses_other" })
      await event("session.deleted", { sessionID: "ses_root" })
      await select()
      permissions.set("ses_other", [])
      forms.set("ses_other", [])
      await select()
    })
    assert.deepEqual(refreshed_events.filter((event) => event.permission_id || event.question_id)
      .map((event) => [event.event, event.session_id, event.permission_id ?? event.question_id, event.turn_id, event.answer_nonempty]), [
      ["permission.asked", "ses_root", "per_existing", undefined, undefined],
      ["form.created", "ses_root", "frm_existing", undefined, undefined],
      ["form.created", "ses_root", "frm_remaining", undefined, undefined],
      ["form.created", "ses_root", "frm_live", "evt_live", undefined],
      ["logonomic.permission.resolved", "ses_root", "per_existing", undefined, undefined],
      ["logonomic.form.resolved", "ses_root", "frm_existing", undefined, undefined],
      ["logonomic.form.resolved", "ses_root", "frm_live", undefined, undefined],
      ["form.replied", "ses_root", "frm_live", "evt_live", true],
      ["permission.asked", "ses_other", "per_next", undefined, undefined],
      ["form.created", "ses_other", "frm_next", undefined, undefined],
      ["logonomic.permission.resolved", "ses_other", "per_next", undefined, undefined],
      ["logonomic.form.resolved", "ses_other", "frm_next", undefined, undefined],
    ], "snapshot removal resolves only absent identities, preserves late reply evidence, and ignores stale or foreign requests")
    assert.equal(refreshed_events.some((event) => event.event === "session.deleted" || event.event === "session.execution.failed"),
      false, "background terminal events cannot end the newly selected session or clear its pending prompts")

    const child_events = await attention_events(async ({ sessions, permissions, forms, families, select, event }) => {
      permissions.set("ses_root", [])
      forms.set("ses_root", [])
      for (const id of ["ses_child", "ses_sibling", "ses_grandchild"]) {
        sessions.set(id, { id, parentID: id === "ses_grandchild" ? "ses_child" : "ses_root" })
      }
      families.set("ses_root", ["ses_root", "ses_child", "ses_sibling", "ses_grandchild"])
      permissions.set("ses_child", [permission("per_restored", "ses_child")])
      forms.set("ses_child", [])
      await select()
      // A descendant's collection can hydrate after the root is already visible.
      forms.set("ses_grandchild", [form("frm_restored", "ses_grandchild")])
      await select()
      await event("session.execution.started", { sessionID: "ses_root" }, "evt_parent")
      await event("session.execution.succeeded", { sessionID: "ses_root" })
      permissions.set("ses_sibling", [permission("per_live", "ses_sibling")])
      await event("permission.asked", permission("per_live", "ses_sibling"))
      forms.set("ses_sibling", [form("frm_live", "ses_sibling")])
      await event("form.created", { form: form("frm_live", "ses_sibling") })
      await event("session.execution.started", { sessionID: "ses_root" }, "evt_parent_next")
      await event("session.execution.failed", { sessionID: "ses_root" })
      await event("form.replied", { sessionID: "ses_child", id: "frm_live", answer: { value: private_canary } })
      permissions.set("ses_child", undefined)
      await select()
      permissions.set("ses_child", [])
      await select()
      permissions.set("ses_sibling", [])
      await event("permission.replied", { sessionID: "ses_sibling", requestID: "per_live", reply: "once" })
      forms.set("ses_sibling", [])
      // A paired reply remains attributable if the family view refreshed first.
      families.set("ses_root", ["ses_root", "ses_child", "ses_grandchild"])
      await event("form.replied", { sessionID: "ses_sibling", id: "frm_live", answer: { value: private_canary } })
      families.set("ses_root", ["ses_root", "ses_child"])
      await event("session.deleted", { sessionID: "ses_grandchild" })
      await event("permission.asked", permission("per_foreign", "ses_other"))
      await event("form.created", { form: form("frm_global", "global") })
      await select("ses_other")
      await event("form.created", { form: form("frm_old_root", "ses_child") })
    })
    assert.deepEqual(child_events.filter((value) => value.permission_id || value.question_id)
      .map((value) => [value.event, value.session_id, value.agent_id, value.permission_id ?? value.question_id, value.turn_id]), [
      ["permission.asked", "ses_root", "ses_child", "per_restored", undefined],
      ["form.created", "ses_root", "ses_grandchild", "frm_restored", undefined],
      ["permission.asked", "ses_root", "ses_sibling", "per_live", undefined],
      ["form.created", "ses_root", "ses_sibling", "frm_live", undefined],
      ["logonomic.permission.resolved", "ses_root", "ses_child", "per_restored", undefined],
      ["permission.replied", "ses_root", "ses_sibling", "per_live", undefined],
      ["form.replied", "ses_root", "ses_sibling", "frm_live", undefined],
      ["logonomic.form.resolved", "ses_root", "ses_grandchild", "frm_restored", undefined],
    ], "root-visible descendant requests preserve exact ownership and independent settlement across parent executions")
    assert.equal(child_events.some((value) => value.event === "session.deleted"), false,
      "deleting a descendant cannot end its root session")

    const completed_cases = []
    for (const [parts, hint] of [
      [["A detailed explanation. ".repeat(250), "\n\nWhich target should I use?"], true],
      [["Which ", "é𝛼 ".repeat(600), "target should I use?"], true],
      [["Completed."], false],
      [["```\n", "Which target?\n", "```"], false],
      [["> Which ", "target?"], false],
      [["```\nexample\n```\n\nWhich target?"], true],
    ]) {
      const events = await attention_events(async ({ permissions, forms, event, select }) => {
        permissions.set("ses_root", [])
        forms.set("ses_root", [])
        await select()
        await event("session.execution.started", { sessionID: "ses_root" }, "evt_prose")
        const owner = { sessionID: "ses_root", assistantMessageID: "msg_reply" }
        await event("session.step.started", owner)
        await event("session.reasoning.ended", { ...owner, ordinal: 0, text: "Which private reasoning?" })
        for (const [ordinal, text] of parts.entries()) {
          await event("session.text.started", { ...owner, ordinal })
          await event("session.text.delta", { ...owner, ordinal, delta: "not the complete native text" })
          await event("session.text.ended", { ...owner, ordinal, text })
        }
        await event("session.step.ended", { ...owner, finish: "stop" })
        await event("session.execution.succeeded", { sessionID: "ses_root" })
      })
      const body = events.find((value) => value.event === "session.execution.succeeded")
      assert(body, "normal root completion must survive reply processing")
      if (hint) {
        assert.equal(body.normal_completion, true, "complete attributed native reply must reach the classifier")
        assert(Buffer.byteLength(JSON.stringify(body)) < 1024, "long complete native prose must remain bounded at ingress")
      }
      assert.equal(JSON.stringify(events).includes("private reasoning"), false)
      completed_cases.push({ text: parts.join(""), body, hint })
    }
    if (process.argv[5] && process.argv[6]) {
      const cases_path = join(root, "native-replies.json")
      await writeFile(cases_path, JSON.stringify(completed_cases))
      const checked = spawnSync(resolve(process.argv[5]), [resolve(process.argv[6]), cases_path], { encoding: "utf8" })
      assert.equal(checked.status, 0, checked.stdout + checked.stderr)
      process.stdout.write(checked.stdout)
    }

    for (const missing of ["start", "text_start", "text_end", "ordinal", "step_end", "tool_finish", "later_step",
      "reconnect", "new_input", "pending_form", "failed", "interrupted"]) {
      const events = await attention_events(async ({ permissions, forms, event, select }) => {
        permissions.set("ses_root", [])
        forms.set("ses_root", [])
        await select()
        if (missing !== "start") await event("session.execution.started", { sessionID: "ses_root" }, "evt_incomplete")
        const owner = { sessionID: "ses_root", assistantMessageID: "msg_reply" }
        await event("session.step.started", owner)
        const part = { ...owner, ordinal: missing === "ordinal" ? 1 : 0 }
        if (missing !== "text_start") await event("session.text.started", part)
        if (missing !== "text_end") await event("session.text.ended", { ...part, text: "Which target?" })
        if (missing !== "step_end") await event("session.step.ended", { ...owner, finish: missing === "tool_finish" ? "tool-calls" : "stop" })
        if (missing === "later_step") await event("session.step.started", { ...owner, assistantMessageID: "msg_tool_result" })
        if (missing === "reconnect") await event("server.connected", {})
        if (missing === "new_input") await event("session.inbox.enqueued", { sessionID: "ses_root", item: { type: "user" } })
        if (missing === "pending_form") {
          forms.set("ses_root", [form("frm_pending")])
          await event("form.created", { form: form("frm_pending") })
        }
        await event(`session.execution.${["failed", "interrupted"].includes(missing) ? missing : "succeeded"}`,
          { sessionID: "ses_root" })
      })
      assert.equal(events.some((value) => value.assistant_reply_evidence), false,
        `${missing} cannot establish a complete normal attributed reply`)
    }
    const input_events = await attention_events(async ({ event, select }) => {
      await select()
      await event("session.inbox.enqueued", { sessionID: "ses_root", item: { type: "synthetic", text: private_canary } })
      await event("session.inbox.enqueued", { sessionID: "ses_other", item: { type: "user", text: private_canary } })
      await event("session.inbox.enqueued", { sessionID: "ses_root", item: { type: "user", text: private_canary } })
    })
    assert.deepEqual(input_events.filter((value) => value.event === "session.inbox.enqueued"), [{
      event: "session.inbox.enqueued", session_id: "ses_root", api_generation: 2, input_type: "user",
    }], "only genuine selected-root user input can resolve its conversational hint, without forwarding input prose")

    const overflow_events = await attention_events(async ({ permissions, select }) => {
      permissions.set("ses_root", Array.from({ length: 33 }, (_, index) => permission(`per_${index}`)))
      await select()
    })
    assert.deepEqual(overflow_events.map((event) => event.event),
      ["logonomic.session.selected", "logonomic.delivery_overflow"],
      "an oversized native snapshot degrades lifecycle instead of claiming that a truncated list is complete")
  }
  console.log("OpenCode TUI contract: selection, descendant attention, complete replies, pending reconciliation, wake policy and cleanup passed")
} finally {
  await integration?.dispose()
  await rm(root, { recursive: true, force: true })
}
