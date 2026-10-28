import { create_wake_bridge, wake_identity, wake_unavailable } from "./logonomic_opencode_bridge.mjs"
import { create_event_delivery } from "./logonomic_opencode_relay.mjs"

const max_pending_events = 32
const delivery_timeout_ms = 47000
const drain_timeout_ms = 96000

function create_lifecycle_relay() {
  const pending = []
  let accepting = true
  let cutoff = false
  let worker
  const delivery = create_event_delivery(delivery_timeout_ms, () => !cutoff)

  function start() {
    if (worker || cutoff) return
    worker = (async () => {
      while (!cutoff && pending.length) await delivery.deliver(pending.shift())
    })().finally(() => {
      worker = undefined
      if (!cutoff && pending.length) start()
    })
  }

  return {
    available: () => accepting,
    enqueue(event) {
      if (!accepting) return
      if (pending.length >= max_pending_events) {
        accepting = false
        pending.length = 0
        pending.push({ event: "logonomic.delivery_overflow" })
      } else {
        pending.push(event)
      }
      start()
    },
    async dispose() {
      accepting = false
      const timer = setTimeout(() => {
        cutoff = true
        pending.length = 0
        delivery.cancel(null)
      }, drain_timeout_ms)
      try {
        while (worker) await worker
      } finally {
        clearTimeout(timer)
      }
    },
  }
}

const disposition = (state) => ({
  disposition: state.reason === "provider_busy" || state.reason === "provider_awaiting_input"
    ? "deferred" : "unavailable",
  reason: state.reason,
})

const root_session = (session, session_id) => session?.id === session_id && !session.parentID

// OpenCode 2's promise client returns decoded values, while the HTTP transport
// wraps those values in `data`. Using the native client keeps auth and routing
// owned by the TUI that the user is actually running.
export async function create_tui_integration(context) {
  const { client, data, ui } = context
  if (process.env.OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED !== "1" ||
      typeof client?.session?.synthetic !== "function" ||
      typeof client?.session?.get !== "function" || typeof client?.session?.active !== "function" ||
      typeof client?.session?.inbox?.list !== "function" || typeof client?.session?.form?.list !== "function" ||
      typeof client?.permission?.list !== "function" || typeof data?.listen !== "function" ||
      typeof data?.session?.get !== "function" || typeof data?.session?.status !== "function" ||
      typeof data?.session?.pending?.list !== "function" ||
      typeof data?.session?.permission?.list !== "function" || typeof data?.session?.form?.list !== "function" ||
      typeof ui?.router?.current !== "function") return

  const relay = create_lifecycle_relay()
  const attention = new Map()
  const settled = new Map()
  let selected
  let visible = false
  let turn_id
  let latest_turn_id
  let generation = 0
  let disposed = false
  let bridge
  let pending_snapshot

  function attention_event(event, request, correlated = true) {
    return {
      event, session_id: selected, api_generation: 2,
      ...(correlated && request.turn_id ? { turn_id: request.turn_id } : {}),
      [request.kind === "permission" ? "permission_id" : "question_id"]: request.id,
    }
  }

  function remember_settled(key, request, replied) {
    attention.delete(key)
    settled.delete(key)
    settled.set(key, { request, replied })
    if (settled.size > max_pending_events) settled.delete(settled.keys().next().value)
  }

  function request_attention(kind, id, correlated = false) {
    const key = `${kind}:${id}`
    if (attention.has(key) || settled.has(key)) return
    if (attention.size >= max_pending_events) {
      relay.enqueue({ event: "logonomic.delivery_overflow" })
      return
    }
    const request = { kind, id, turn_id: correlated ? turn_id : undefined }
    attention.set(key, request)
    ++generation
    relay.enqueue(attention_event(kind === "permission" ? "permission.asked" : "form.created", request))
  }

  function reconcile_attention(kind, ids) {
    // Undefined means this native collection has not hydrated, not that its
    // previously observed requests have been answered.
    if (!ids) return
    for (const [key, request] of attention) {
      if (request.kind !== kind || ids.has(request.id)) continue
      remember_settled(key, request, false)
      ++generation
      // Absence proves the request is no longer pending, but neither its
      // answer nor cancellation. Keep that distinct from native reply events.
      relay.enqueue(attention_event(`logonomic.${kind}.resolved`, request, false))
    }
    for (const id of ids) request_attention(kind, id)
  }

  function snapshot_ids(requests) {
    if (!Array.isArray(requests)) return
    const ids = new Set()
    for (const request of requests) {
      if (request?.sessionID !== selected || !wake_identity(request.id)) continue
      ids.add(request.id)
      if (ids.size > max_pending_events) {
        relay.enqueue({ event: "logonomic.delivery_overflow" })
        return
      }
    }
    return ids
  }

  function current_session() {
    const route = ui.router.current()
    if (route.type !== "session" || !wake_identity(route.sessionID)) return
    const session = data.session.get(route.sessionID)
    return root_session(session, route.sessionID) ? session : undefined
  }

  function select() {
    if (disposed) return
    const session = current_session()
    // Home, child and plugin pages hide the conversation without changing the
    // host's admitted native session. Readiness still requires its visible root.
    if (!session) { visible = false; return }
    const changed = session.id !== selected || !visible
    visible = true
    if (session.id !== selected) {
      if (selected) bridge?.revoke(selected)
      selected = session.id
      turn_id = undefined
      latest_turn_id = undefined
      ++generation
      attention.clear()
      settled.clear()
    }
    // Returning from a hidden route is new readiness evidence. Its helper
    // commit lets the host retry unread messages without changing the nonce.
    if (changed) relay.enqueue({
      event: "logonomic.session.selected", session_id: selected,
      status_type: data.session.status(selected), api_generation: 2,
    })
    // Read every pending identity inside the TUI's reactive selection effect,
    // including when the same selected session finishes hydrating or refreshes.
    const snapshot = {
      session_id: selected,
      permissions: snapshot_ids(data.session.permission.list(selected)),
      forms: snapshot_ids(data.session.form.list(selected)),
    }
    const queued = pending_snapshot !== undefined
    pending_snapshot = snapshot
    if (queued) return
    queueMicrotask(() => {
      const snapshot = pending_snapshot
      pending_snapshot = undefined
      if (disposed || !visible || snapshot.session_id !== selected) return
      // The native store updates before its event listeners. Give the native
      // ask/reply its original turn and answer evidence before reconciling.
      reconcile_attention("permission", snapshot.permissions)
      reconcile_attention("form", snapshot.forms)
    })
  }

  function readiness(session_id) {
    if (disposed || !relay.available()) return wake_unavailable("provider_disposed")
    const session = current_session()
    if (session?.id !== session_id || selected !== session_id) {
      return wake_unavailable("provider_session_unavailable")
    }
    const permissions = data.session.permission.list(session_id)
    const forms = data.session.form.list(session_id)
    if (!Array.isArray(permissions) || !Array.isArray(forms)) {
      return wake_unavailable("provider_status_unavailable")
    }
    if (permissions.length || forms.length || attention.size) return wake_unavailable("provider_awaiting_input")
    if (data.session.status(session_id) !== "idle" || data.session.pending.list(session_id).length) {
      return wake_unavailable("provider_busy")
    }
    if (session.outcome !== "succeeded") return wake_unavailable("provider_interrupted")
    return { available: true }
  }

  const stop_events = data.listen(({ details: event }) => {
    if (disposed) return
    select()
    const value = event.data
    const session_id = value?.sessionID ?? value?.form?.sessionID
    if (session_id !== selected) return
    const type = event.type
    if (type.startsWith("session.execution.") || type.startsWith("session.inbox.") ||
        type.startsWith("permission.") || type.startsWith("form.") ||
        type === "session.agent.selected" || type === "session.model.selected" || type === "session.deleted") {
      ++generation
    }
    if (type === "session.execution.started") {
      turn_id = wake_identity(event.id) ? event.id : undefined
      latest_turn_id = turn_id
    }
    const projected = { event: type, session_id, api_generation: 2, ...(turn_id ? { turn_id } : {}) }
    if (type === "session.execution.started" || type === "session.execution.succeeded" ||
        type === "session.execution.failed" || type === "session.execution.interrupted") {
      relay.enqueue(projected)
      if (type !== "session.execution.started") {
        turn_id = undefined
      }
      return
    }
    if (type === "session.deleted") {
      relay.enqueue(projected)
      bridge?.revoke(session_id)
      selected = undefined
      visible = false
      turn_id = undefined
      latest_turn_id = undefined
      attention.clear()
      settled.clear()
      return
    }
    const asked = type === "permission.asked" || type === "form.created"
    const replied = type === "permission.replied" || type === "form.replied" || type === "form.cancelled"
    if (!asked && !replied) return
    const request_id = type === "form.created" ? value.form.id : value.requestID ?? value.id
    if (!wake_identity(request_id)) return
    const kind = type.startsWith("permission.") ? "permission" : "form"
    const key = `${kind}:${request_id}`
    if (asked) {
      request_attention(kind, request_id, true)
      return
    }
    // A refreshed snapshot or a local reply response can remove the request
    // before its native reply arrives. Retain bounded correlation for that reply
    // and prevent an older snapshot from reopening a settled identity.
    const prior = settled.get(key)
    const request = attention.get(key) ?? prior?.request
    if (!request || prior?.replied) return
    remember_settled(key, request, true)
    // A request may outlive its execution. Its native identity still pairs the
    // reply, but a superseded turn would prevent the host from resolving it.
    const response = attention_event(type, request, request.turn_id === latest_turn_id)
    if (type === "form.replied") {
      // Form answer prose stays in OpenCode. False and zero are meaningful
      // answers, while an empty text field is not a confirmed reply.
      response.answer_nonempty = Object.values(value.answer).some((answer) =>
        typeof answer === "string" ? answer.trim().length > 0 :
          Array.isArray(answer) ? answer.some((text) => text.trim().length > 0) : true)
    }
    relay.enqueue(response)
  })

  bridge = await create_wake_bridge({
    inspect: readiness,
    async notify(session_id, instruction, signal, mark_submitted) {
      const observed_generation = generation
      const [session, active, permissions, forms, pending] = await Promise.all([
        client.session.get({ sessionID: session_id }, { signal }),
        client.session.active({ signal }),
        client.permission.list({ sessionID: session_id }, { signal }),
        client.session.form.list({ sessionID: session_id }, { signal }),
        client.session.inbox.list({ sessionID: session_id }, { signal }),
      ])
      if (!root_session(session, session_id)) return {
        disposition: "unavailable", reason: "provider_session_unavailable",
      }
      if (!active || typeof active !== "object" || Array.isArray(active) ||
          !Array.isArray(permissions) || !Array.isArray(forms) || !Array.isArray(pending)) return {
        disposition: "unavailable", reason: "provider_status_unavailable",
      }
      if (generation !== observed_generation) return {
        disposition: "deferred", reason: "provider_state_changed",
      }
      const state = readiness(session_id)
      if (!state.available) return disposition(state)
      if (permissions.length || forms.length) return {
        disposition: "deferred", reason: "provider_awaiting_input",
      }
      if (active[session_id] || pending.length) return {
        disposition: "deferred", reason: "provider_busy",
      }
      if (session.outcome !== "succeeded") return {
        disposition: "unavailable", reason: "provider_interrupted",
      }
      // Synthetic input uses the session's persisted agent/model/variant.
      // Queue delivery also leaves a concurrently submitted user turn intact.
      mark_submitted()
      const accepted = await client.session.synthetic({
        sessionID: session_id, text: instruction, description: "Logonomic message",
        delivery: "queue", resume: true,
      }, { signal })
      return {
        disposition: accepted?.sessionID === session_id && accepted.type === "synthetic" &&
          wake_identity(accepted.id) ? "accepted" : "uncertain",
      }
    },
  }, process.env.OPENCODE_LOGONOMIC_WAKE_REGISTRATION)

  return {
    select,
    async dispose() {
      disposed = true
      stop_events()
      await bridge?.dispose()
      await relay.dispose()
      attention.clear()
      settled.clear()
    },
  }
}
