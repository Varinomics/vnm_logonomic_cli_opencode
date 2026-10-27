import { create_wake_bridge, wake_identity as identity, wake_unavailable as unavailable } from "./logonomic_opencode_bridge.mjs"

const max_sessions = 128

export async function create_session_wake(client, registration_file) {
  if (!registration_file || typeof client?.session?.promptAsync !== "function" ||
      typeof client?.session?.status !== "function" || typeof client?.session?.get !== "function") return
  const sessions = new Map()
  function session_record(session_id) {
    let record = sessions.get(session_id)
    if (!record && identity(session_id) && sessions.size < max_sessions) {
      record = { status: "unknown", completed: false, blocked: new Set(), generation: 0 }
      sessions.set(session_id, record)
    }
    return record
  }

  function readiness(session_id) {
    const record = sessions.get(session_id)
    if (!record || record.child) return unavailable("provider_session_unavailable")
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
    chat_message(input, output) {
      const record = session_record(input?.sessionID)
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
      if (event.type === "session.deleted") { sessions.delete(session_id); return }
      const record = session_record(session_id)
      if (!record) return
      if (event.type === "session.created" || event.type === "session.updated") {
        record.child = !!properties.info.parentID
      }
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
      if (event.type === "permission.asked" || event.type === "question.asked") {
        record.blocked.add(properties.id)
        ++record.generation
      }
      if (event.type === "permission.replied" || event.type === "question.replied" ||
          event.type === "question.rejected") {
        record.blocked.delete(properties.requestID ?? properties.id)
        ++record.generation
      }
    },
    async dispose() {
      await bridge.dispose()
      sessions.clear()
    },
  }
}
