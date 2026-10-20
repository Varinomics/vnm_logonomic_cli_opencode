import { randomBytes, timingSafeEqual } from "node:crypto"
import { writeFile, unlink } from "node:fs/promises"
import { createServer } from "node:net"

const max_request_bytes = 32768
const max_sessions = 128
const max_receipts = 256
const operation_timeout_ms = 4000

const unavailable = (reason) => ({ available: false, reason })
const identity = (value) => typeof value === "string" &&
  value.length > 0 && value.length <= 256 && !/[\x00-\x20]/.test(value)

// The bridge belongs to this native OpenCode instance. The adapter learns its
// endpoint through one private, launch-owned file, never a guessed TUI port.
export async function create_session_wake(client, registration_file) {
  if (!registration_file || typeof client?.session?.promptAsync !== "function" ||
      typeof client?.session?.status !== "function" || typeof client?.session?.get !== "function") return

  const token = randomBytes(32).toString("hex")
  const instance_id = randomBytes(16).toString("hex")
  const sessions = new Map()
  const receipts = new Map()
  const sockets = new Set()
  const operations = new Set()
  let disposed = false
  let registered = false

  function session_record(session_id) {
    let record = sessions.get(session_id)
    if (!record && identity(session_id) && sessions.size < max_sessions) {
      record = { status: "unknown", completed: false, blocked: new Set(), generation: 0 }
      sessions.set(session_id, record)
    }
    return record
  }

  function readiness(session_id) {
    if (disposed) return unavailable("provider_disposed")
    const record = sessions.get(session_id)
    if (!record || record.child) return unavailable("provider_session_unavailable")
    if (!record.context) return unavailable("provider_turn_context_unavailable")
    if (record.blocked.size) return unavailable("provider_awaiting_input")
    if (record.status !== "idle" || record.submitting) return unavailable("provider_busy")
    if (!record.completed) return unavailable("provider_interrupted")
    return { available: true }
  }

  async function dispatch(request) {
    if (!request || request.instance_id !== instance_id ||
        typeof request.token !== "string" || request.token.length !== token.length ||
        !timingSafeEqual(Buffer.from(request.token), Buffer.from(token)) ||
        !identity(request.provider_session_id) ||
        !/^[a-f0-9]{32}$/.test(request.transition_nonce ?? "")) return { error: "invalid_binding" }
    const session_id = request.provider_session_id
    const current = sessions.get(session_id)
    if (current?.correlation && current.correlation !== request.transition_nonce) {
      return request.operation === "inspect" ? unavailable("provider_correlation_changed") : {
        disposition: "unavailable", reason: "provider_correlation_changed",
      }
    }
    if (current) current.correlation ??= request.transition_nonce
    if (request.operation === "inspect") return readiness(session_id)
    if (request.operation !== "notify" || !identity(request.notification_id) ||
        typeof request.instruction !== "string" || !request.instruction.trim() ||
        Buffer.byteLength(request.instruction, "utf8") > 16384) return { error: "invalid_request" }

    const key = `${session_id}\0${request.notification_id}`
    const previous = receipts.get(key)
    if (previous) return previous
    const ready = readiness(session_id)
    if (!ready.available) return {
      disposition: ready.reason === "provider_busy" || ready.reason === "provider_awaiting_input"
        ? "deferred" : "unavailable", reason: ready.reason,
    }
    // Never evict a receipt and then risk submitting its notification twice.
    if (receipts.size >= max_receipts) return {
      disposition: "unavailable", reason: "provider_receipt_capacity",
    }
    const record = sessions.get(session_id)
    const generation = record.generation
    record.submitting = true
    const controller = new AbortController()
    operations.add(controller)
    const timer = setTimeout(() => controller.abort(), operation_timeout_ms)
    let submitted = false
    try {
      const session = await client.session.get({
        path: { id: session_id }, signal: controller.signal, throwOnError: true,
      })
      if (session.error || session.data?.id !== session_id || session.data.parentID) return {
        disposition: "unavailable", reason: "provider_session_unavailable",
      }
      const status = await client.session.status({ signal: controller.signal, throwOnError: true })
      if (status.error || !status.data || typeof status.data !== "object") return {
        disposition: "unavailable", reason: "provider_status_unavailable",
      }
      if (disposed || record.generation !== generation || record.blocked.size ||
          (status.data[session_id]?.type ?? "idle") !== "idle") return {
        disposition: "deferred", reason: "provider_state_changed",
      }
      // All provider defaults remain explicit: a Plan conversation must not
      // wake with the default Build agent. This never touches the TUI draft.
      const receipt = `${instance_id}:${request.notification_id}`
      const uncertain = { disposition: "uncertain", reason: "provider_acceptance_unknown" }
      receipts.set(key, uncertain)
      submitted = true
      const result = await client.session.promptAsync({
        path: { id: session_id },
        body: {
          ...record.context,
          parts: [{ type: "text", text: request.instruction }],
        },
        signal: controller.signal,
        throwOnError: true,
      })
      if (result.error) return uncertain
      const accepted = { disposition: "accepted", receipt }
      receipts.set(key, accepted)
      return accepted
    } catch {
      return submitted ? receipts.get(key) : {
        disposition: "unavailable", reason: "provider_status_unavailable",
      }
    } finally {
      clearTimeout(timer)
      operations.delete(controller)
      record.submitting = false
    }
  }

  const server = createServer((socket) => {
    if (disposed || sockets.size >= 8) { socket.destroy(); return }
    sockets.add(socket)
    const deadline = setTimeout(() => socket.destroy(), operation_timeout_ms + 1000)
    socket.on("error", () => {})
    socket.on("close", () => {
      clearTimeout(deadline)
      sockets.delete(socket)
    })
    let bytes = Buffer.alloc(0)
    let received = false
    socket.on("data", (chunk) => {
      if (received) { socket.destroy(); return }
      if (bytes.length + chunk.length > max_request_bytes) { socket.destroy(); return }
      bytes = Buffer.concat([bytes, chunk])
      const newline = bytes.indexOf(10)
      if (newline < 0) return
      received = true
      if (newline !== bytes.length - 1) { socket.destroy(); return }
      let request
      try { request = JSON.parse(bytes.subarray(0, newline).toString("utf8")) }
      catch { socket.destroy(); return }
      void dispatch(request).then(
        (response) => socket.end(`${JSON.stringify(response)}\n`),
        () => socket.destroy(),
      )
    })
  })
  try {
    await new Promise((resolve, reject) => {
      server.once("error", reject)
      server.listen({ host: "127.0.0.1", port: 0, exclusive: true }, resolve)
    })
    await writeFile(registration_file, JSON.stringify({
      version: 1, port: server.address().port, token, instance_id,
    }), { encoding: "utf8", flag: "wx", mode: 0o600 })
    registered = true
    server.unref()
  } catch {
    server.close()
    return
  }

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
      disposed = true
      for (const controller of operations) controller.abort()
      for (const socket of sockets) socket.destroy()
      await new Promise((resolve) => server.close(resolve))
      if (registered) await unlink(registration_file).catch(() => {})
      sessions.clear()
      receipts.clear()
    },
  }
}
