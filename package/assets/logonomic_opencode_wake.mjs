import { create_wake_bridge, wake_identity as identity, wake_unavailable as unavailable } from "./logonomic_opencode_bridge.mjs"

const max_sessions = 128

export async function create_session_wake(client, registration_file) {
  if (!registration_file || typeof client?.session?.promptAsync !== "function" ||
      typeof client?.session?.status !== "function" || typeof client?.session?.get !== "function") return
  const sessions = new Map()
  const unresolved = new Map()
  let selected
  let unknown_blocker_lost = false

  function record() {
    return { status: "unknown", completed: false, blocked: new Set(), generation: 0, incomplete: unknown_blocker_lost }
  }

  function admit_root(id) {
    if (sessions.has(id)) return
    if (sessions.size >= max_sessions) {
      // An observed root that never supplied turn context can be re-admitted
      // later. Live bindings and outstanding requests keep their records.
      const unused = [...sessions].find(([other, value]) => other !== selected &&
        !value.context && !value.blocked.size && !bridge.bound(other))
      if (!unused) return
      sessions.delete(unused[0])
    }
    sessions.set(id, unresolved.get(id) ?? record())
    unresolved.delete(id)
  }

  // Keep native facts in arrival order until the lifecycle plugin classifies
  // an already-existing session. Only user input and open requests need state.
  function input_record(id, asked = false) {
    if (!identity(id)) return
    const retained = sessions.get(id) ?? unresolved.get(id)
    if (retained) return retained
    if (unresolved.size >= max_sessions) {
      // Never discard an unanswered request. Displaced turn context makes
      // its session unavailable until the next observed user turn.
      const unused = [...unresolved].find(([, candidate]) => !candidate.blocked.size)
      if (!unused) {
        // All slots hold open, unclassified requests. Once an ask is lost the
        // plugin cannot prove absence of a blocker for newly encountered IDs.
        if (asked) unknown_blocker_lost = true
        return
      }
      unresolved.delete(unused[0])
    }
    const pending = record()
    unresolved.set(id, pending)
    return pending
  }

  function readiness(session_id) {
    const record = sessions.get(session_id)
    if (!record || (selected && session_id !== selected)) return unavailable("provider_session_unavailable")
    if (record.incomplete) return unavailable("provider_session_capacity")
    if (!record.context) return unavailable("provider_turn_context_unavailable")
    if (record.blocked.size) return unavailable("provider_awaiting_input")
    if (record.status !== "idle") return unavailable("provider_busy")
    if (!record.completed) return unavailable("provider_interrupted")
    return { available: true }
  }

  const bridge = await create_wake_bridge({
    inspect: readiness,
    async notify(session_id, instruction, signal, mark_submitted) {
      const record = sessions.get(session_id)
      const generation = record.generation
      const session = await client.session.get({ path: { id: session_id }, signal, throwOnError: true })
      if (session.error || session.data?.id !== session_id || session.data.parentID) return {
        disposition: "unavailable", reason: "provider_session_unavailable",
      }
      const status = await client.session.status({ signal, throwOnError: true })
      if (status.error || !status.data || typeof status.data !== "object") return {
        disposition: "unavailable", reason: "provider_status_unavailable",
      }
      if (record.generation !== generation || record.blocked.size ||
          (status.data[session_id]?.type ?? "idle") !== "idle") return {
        disposition: "deferred", reason: "provider_state_changed",
      }
      // Plan, model and variant remain explicit in the opencode-ai SDK contract.
      mark_submitted()
      const result = await client.session.promptAsync({
        path: { id: session_id },
        body: { ...record.context, parts: [{ type: "text", text: instruction }] },
        signal,
        throwOnError: true,
      })
      return { disposition: result.error ? "uncertain" : "accepted" }
    },
  }, registration_file)
  if (!bridge) return

  return {
    session_kind(id, kind, observed) {
      if (!observed || unresolved.get(id) !== observed) return
      if (kind === "root") admit_root(id)
      if (kind === "child") unresolved.delete(id)
    },
    select_root(session_id, observed) {
      if (observed !== (sessions.get(session_id) ?? unresolved.get(session_id))) return
      if (selected === session_id) return
      // A resume can be inspected before this launch's first confirmed input.
      if (selected) bridge.revoke(selected)
      else for (const id of sessions.keys()) {
        if (id !== session_id) bridge.revoke(id)
      }
      selected = session_id
    },
    chat_message(input, output) {
      const record = input_record(input?.sessionID)
      if (!record) return
      ++record.generation
      record.completed = false
      record.status = "busy"
      const message = output?.message
      const variant = message?.model?.variant
      if (!identity(message?.agent) || !identity(message?.model?.providerID) ||
          !identity(message?.model?.modelID) ||
          (variant !== undefined && (typeof variant !== "string" ||
            Buffer.byteLength(variant, "utf8") > 1024))) { record.context = undefined; return }
      record.context = {
        agent: message.agent,
        model: { providerID: message.model.providerID, modelID: message.model.modelID },
        ...(variant !== undefined ? { variant } : {}),
      }
      record.turn_id = message.id
    },
    event(event) {
      const properties = event?.properties
      const session_id = properties?.sessionID ?? properties?.info?.sessionID ?? properties?.info?.id
      if (!identity(session_id)) return
      const session_fact = event.type === "session.created" || event.type === "session.updated"
      if (event.type === "session.deleted" || (session_fact && properties.info?.parentID)) {
        bridge.revoke(session_id)
        sessions.delete(session_id)
        unresolved.delete(session_id)
        if (selected === session_id) selected = undefined
        return
      }
      if (session_fact && properties.info?.id === session_id) {
        admit_root(session_id)
        return
      }
      const asked = event.type === "permission.asked" || event.type === "question.asked"
      const record = asked ? input_record(session_id, true) : sessions.get(session_id) ?? unresolved.get(session_id)
      if (!record) return
      if (event.type === "session.status") {
        record.status = properties.status?.type ?? "unknown"
        ++record.generation
      }
      if (event.type === "session.idle") { record.status = "idle"; ++record.generation }
      if (event.type === "session.error") { record.completed = false; ++record.generation }
      if (event.type === "message.updated" && properties.info?.role === "assistant" &&
          properties.info.parentID === record.turn_id) {
        record.completed = properties.info.finish === "stop" &&
          Number.isFinite(properties.info.time?.completed) && !properties.info.error
      }
      if (asked) {
        record.blocked.add(properties.id)
        ++record.generation
      }
      if (event.type === "permission.replied" || event.type === "question.replied" ||
          event.type === "question.rejected") {
        record.blocked.delete(properties.requestID ?? properties.id)
        if (!record.blocked.size && !record.context) unresolved.delete(session_id)
        ++record.generation
      }
      return record
    },
    async dispose() {
      await bridge.dispose()
      sessions.clear()
      unresolved.clear()
    },
  }
}
