import { spawn } from "node:child_process"
import { join } from "node:path"

const maxRelayDiagnosticCharacters = 1024
const reportDeliveryFailure = (reason) => {
  console.error(`Logonomic OpenCode event delivery failed: ${reason}.`)
}

// The SDK helper owns its wrapper descendant; cancellation closes stdin and
// waits for the helper to reap it instead of killing just the relay process.
export function create_event_delivery(deliveryTimeoutMilliseconds, allowed) {
  let cancelActiveDelivery
  let lastRelayFailure
  const deliver = (projected) => new Promise((resolve) => {
    if (!allowed()) {
      resolve()
      return
    }
    const helpersDirectory = process.env.LOGONOMIC_PROVIDER_HELPERS_DIR
    const relayPath = helpersDirectory && join(helpersDirectory,
      process.platform === "win32" ? "opencode_helper.exe" : "opencode_helper")
    const capability = process.env.LOGONOMIC_SESSION_CAPABILITY
    if (
      !relayPath ||
      !capability ||
      !process.env.LOGONOMIC_PROVIDER_PACKAGE_ID
    ) {
      resolve()
      return
    }
    let child
    let deliveryTimer
    let settled = false
    let terminating = false
    let childFailure
    let terminationFailure
    let relayDiagnostic = ""
    const finish = (failure) => {
      if (settled) return
      settled = true
      clearTimeout(deliveryTimer)
      child?.stdin.destroy()
      if (cancelActiveDelivery === terminate) {
        cancelActiveDelivery = undefined
      }
      const detail = relayDiagnostic.trim().replace(/\s+/g, " ").replace(/\.$/, "")
      const message = failure && (detail ? `${failure}: ${detail}` : failure)
      if (message && message !== lastRelayFailure) reportDeliveryFailure(message)
      lastRelayFailure = message
      resolve()
    }
    const terminate = (
      failure = "the relay exceeded its owned delivery deadline",
    ) => {
      if (settled || terminating) return
      terminating = true
      terminationFailure = failure
      clearTimeout(deliveryTimer)
      // EOF asks the SDK helper to kill and reap its wrapper before it exits.
      // Killing only the helper PID would abandon a live wrapper descendant.
      child.stdin.destroy()
    }

    if (!deliveryTimeoutMilliseconds) {
      finish("the owned relay timeout is unavailable")
      return
    }
    if (!allowed()) {
      finish()
      return
    }
    try {
      child = spawn(relayPath, ["event", "--watch-parent"], {
        stdio: ["pipe", "ignore", "pipe"],
        windowsHide: true,
      })
      cancelActiveDelivery = terminate
      child.stderr.setEncoding("utf8")
      child.stderr.on("data", (chunk) => {
        relayDiagnostic += chunk.slice(
          0, maxRelayDiagnosticCharacters - relayDiagnostic.length,
        )
      })
      child.once("error", () => {
        childFailure ??= "the relay could not be started"
      })
      child.once("close", (code, signal) => {
        if (terminating) {
          finish(terminationFailure)
          return
        }
        if (childFailure) {
          finish(childFailure)
          return
        }
        if (code !== 0) {
          finish(signal || code === null
            ? "the relay was terminated before delivery"
            : `the relay exited with status ${code}`)
          return
        }
        finish()
      })
      child.stdin.once("error", () => {
        terminate("the relay input could not be sent")
      })
      child.stdin.write(JSON.stringify(projected) + "\n")
      deliveryTimer = setTimeout(
        terminate,
        deliveryTimeoutMilliseconds,
      )
    } catch {
      finish("the relay could not be started")
    }
  })

  return { deliver, cancel: (reason) => cancelActiveDelivery?.(reason) }
}
