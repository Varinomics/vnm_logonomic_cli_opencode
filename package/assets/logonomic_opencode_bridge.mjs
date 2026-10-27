import { randomBytes, timingSafeEqual } from "node:crypto"
import { writeFile, unlink } from "node:fs/promises"
import { createServer } from "node:net"

const max_request_bytes = 32768
const max_sessions = 128
const max_receipts = 256
const max_retired_correlations = 256
const operation_timeout_ms = 4000

export const wake_identity = (value) => typeof value === "string" &&
  value.length > 0 && value.length <= 256 && !/[\x00-\x20]/.test(value)

export const wake_unavailable = (reason) => ({ available: false, reason })

// Both native plugin contracts share one launch-owned transport and receipt
// table. Only the native implementation decides when submission is possible.
export async function create_wake_bridge(native, registration_file) {
  if (!registration_file) return
  const token = randomBytes(32).toString("hex")
  const instance_id = randomBytes(16).toString("hex")
  const correlations = new Map()
  const retired_correlations = new Set()
  const receipts = new Map()
  const submitting = new Set()
  const sockets = new Set()
  const operations = new Map()
  let disposed = false
  let registered = false

  function readiness(session_id) {
    if (disposed) return wake_unavailable("provider_disposed")
    if (submitting.has(session_id)) return wake_unavailable("provider_busy")
    return native.inspect(session_id)
  }

  async function dispatch(request) {
    if (!request || request.instance_id !== instance_id ||
        typeof request.token !== "string" || request.token.length !== token.length ||
        !timingSafeEqual(Buffer.from(request.token), Buffer.from(token)) ||
        !wake_identity(request.provider_session_id) ||
        !/^[a-f0-9]{32}$/.test(request.transition_nonce ?? "")) return { error: "invalid_binding" }
    const session_id = request.provider_session_id
    const correlation = correlations.get(session_id)
    if ((correlation && correlation !== request.transition_nonce) ||
        retired_correlations.has(`${session_id}\0${request.transition_nonce}`)) {
      return request.operation === "inspect" ? wake_unavailable("provider_correlation_changed") : {
        disposition: "unavailable", reason: "provider_correlation_changed",
      }
    }
    const ready = readiness(session_id)
    if (!correlation && request.operation === "inspect" && ready.reason !== "provider_session_unavailable") {
      if (correlations.size >= max_sessions || retired_correlations.size >= max_retired_correlations) {
        return request.operation === "inspect" ? wake_unavailable("provider_session_capacity") : {
          disposition: "unavailable", reason: "provider_session_capacity",
        }
      }
      correlations.set(session_id, request.transition_nonce)
    }
    if (request.operation === "inspect") return ready
    if (request.operation !== "notify" || !wake_identity(request.notification_id) ||
        typeof request.instruction !== "string" || !request.instruction.trim() ||
        Buffer.byteLength(request.instruction, "utf8") > 16384) return { error: "invalid_request" }

    const key = `${session_id}\0${request.notification_id}`
    const previous = receipts.get(key)
    if (previous) return previous
    if (!ready.available) return {
      disposition: ready.reason === "provider_busy" || ready.reason === "provider_awaiting_input"
        ? "deferred" : "unavailable", reason: ready.reason,
    }
    if (!correlations.has(session_id)) return {
      disposition: "unavailable", reason: "provider_inspection_required",
    }
    // Eviction would allow a retried notification to start a second turn.
    if (receipts.size >= max_receipts) return {
      disposition: "unavailable", reason: "provider_receipt_capacity",
    }
    submitting.add(session_id)
    const controller = new AbortController()
    operations.set(controller, session_id)
    const timer = setTimeout(() => controller.abort(), operation_timeout_ms)
    let submitted = false
    try {
      const result = await native.notify(session_id, request.instruction, controller.signal, () => {
        if (disposed || controller.signal.aborted || correlations.get(session_id) !== request.transition_nonce) {
          throw new Error("Wake was cancelled before submission")
        }
        submitted = true
        receipts.set(key, { disposition: "uncertain", reason: "provider_acceptance_unknown" })
      })
      if (submitted && result?.disposition === "accepted") {
        const accepted = { disposition: "accepted", receipt: `${instance_id}:${request.notification_id}` }
        receipts.set(key, accepted)
        return accepted
      }
      return submitted ? receipts.get(key) : result
    } catch {
      return submitted ? receipts.get(key) : {
        disposition: "unavailable", reason: "provider_status_unavailable",
      }
    } finally {
      clearTimeout(timer)
      operations.delete(controller)
      submitting.delete(session_id)
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
    revoke(session_id) {
      const correlation = correlations.get(session_id)
      if (correlation) {
        retired_correlations.add(`${session_id}\0${correlation}`)
        correlations.delete(session_id)
      }
      for (const [controller, session] of operations) {
        if (session === session_id) controller.abort()
      }
    },
    async dispose() {
      disposed = true
      for (const controller of operations.keys()) controller.abort()
      for (const socket of sockets) socket.destroy()
      await new Promise((resolve) => server.close(resolve))
      if (registered) await unlink(registration_file).catch(() => {})
      correlations.clear()
      retired_correlations.clear()
      receipts.clear()
    },
  }
}
