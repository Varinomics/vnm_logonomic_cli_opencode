// Logonomic managed OpenCode event plugin

import { Buffer } from "node:buffer"
import { spawn } from "node:child_process"
import { join } from "node:path"

const maxMetadataCharacters = 256
const maxPendingItems = 32
const maxPendingTurnCandidates = 8
const maxChildSessions = 128
const maxChildParentDepth = 16
const childObservationCapacityLost = Symbol("child observation capacity lost")
const maxSessionDiffManifestBytes = Number(
  "201326592",
)
const maxSessionDiffResultBytes = 1024 * 1024
const sessionDiffCancellationHelperMilliseconds = 1000
// The wrapper may wait five seconds for its private core, force it, then wait
// five more seconds before its artifact guard runs.
const sessionDiffCooperativeCancellationMilliseconds = 11000
const sessionDiffTreeReapMilliseconds = 5000
// session.deleted may emit degradation followed by deletion; no item emits more.
const maxProjectedDeliveriesPerItem = 2
const clientLookupTimeoutMilliseconds = 250
const deliveryReapTimeoutMilliseconds = 500
const disposeItemSettlementMarginMilliseconds =
  deliveryReapTimeoutMilliseconds
const finalCleanupReservationMilliseconds =
  deliveryReapTimeoutMilliseconds +
  disposeItemSettlementMarginMilliseconds
const maxTimerMilliseconds = 2147483647
const maxDeliveryTimeoutMilliseconds =
  Math.floor(
    (maxTimerMilliseconds -
      clientLookupTimeoutMilliseconds -
      finalCleanupReservationMilliseconds) /
      maxProjectedDeliveriesPerItem,
  ) - deliveryReapTimeoutMilliseconds

const positiveMilliseconds = (value) => {
  if (!/^[1-9][0-9]*$/.test(value ?? "")) return
  const milliseconds = Number(value)
  return Number.isSafeInteger(milliseconds) &&
    milliseconds <= maxDeliveryTimeoutMilliseconds
    ? milliseconds
    : undefined
}

const reportDeliveryFailure = (reason) => {
  console.error(`Logonomic OpenCode event delivery failed: ${reason}.`)
}

const metadata = (value) =>
  typeof value === "string" &&
  value.length > 0 &&
  value.length <= maxMetadataCharacters &&
  !/[\0\r\n]/.test(value)
    ? value
    : undefined

const millisecondsUntil = (deadline) =>
  Math.max(0, deadline - Date.now())

const settlesBeforeDeadline = async (promise, deadline) => {
  const milliseconds = millisecondsUntil(deadline)
  if (milliseconds === 0) return false
  let deadlineTimer
  try {
    return await Promise.race([
      promise.then(() => true),
      new Promise((resolve) => {
        deadlineTimer = setTimeout(() => resolve(false), milliseconds)
        deadlineTimer.unref?.()
      }),
    ])
  } finally {
    clearTimeout(deadlineTimer)
  }
}

const sessionDiffFailure = (message, details = {}) => {
  const error = new Error(message)
  Object.assign(error, details)
  return error
}

const runCancellationProcess = (
  program,
  arguments_,
  timeoutMilliseconds,
) => new Promise(
  (resolve) => {
    let child
    let settled = false
    let timedOut = false
    let timer
    const settle = (result) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      resolve({ ...result, timedOut })
    }
    try {
      child = spawn(program, arguments_, {
        env: process.env,
        shell: false,
        stdio: "ignore",
        windowsHide: true,
      })
      timer = setTimeout(() => {
        timedOut = true
        try {
          child.kill("SIGKILL")
        } catch {
          // The close observer still owns settlement for a started helper.
        }
      }, timeoutMilliseconds)
      child.once("error", (error) => settle({ error }))
      child.once("close", (code, signal) => settle({ code, signal }))
    } catch (error) {
      settle({ error })
    }
  },
)

const posixProcessGroupExists = (processID) => {
  try {
    process.kill(-processID, 0)
    return true
  } catch (error) {
    return error?.code !== "ESRCH"
  }
}

const waitForPosixProcessGroupExit = async (processID, deadline) => {
  while (posixProcessGroupExists(processID) && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 10))
  }
  return !posixProcessGroupExists(processID)
}

const requestCooperativeCancellation = async (wrapperPath, processID) => {
  if (process.platform !== "win32") {
    try {
      process.kill(processID, "SIGTERM")
      return true
    } catch (error) {
      return error?.code === "ESRCH"
    }
  }
  const result = await runCancellationProcess(
    wrapperPath,
    ["--cancel-process", String(processID)],
    sessionDiffCancellationHelperMilliseconds,
  )
  return !result.timedOut && result.code === 0
}

const hardTerminateTree = async (processID, childClosed) => {
  const deadline = Date.now() + sessionDiffTreeReapMilliseconds
  if (process.platform !== "win32") {
    try {
      process.kill(-processID, "SIGKILL")
    } catch (error) {
      if (error?.code !== "ESRCH") return false
    }
    const [groupExited, wrapperClosed] = await Promise.all([
      waitForPosixProcessGroupExit(processID, deadline),
      settlesBeforeDeadline(childClosed, deadline),
    ])
    return groupExited && wrapperClosed
  }

  const windowsRoot = process.env.SystemRoot || process.env.WINDIR
  if (!windowsRoot) return false
  const taskkill = await runCancellationProcess(
    join(windowsRoot, "System32", "taskkill.exe"),
    ["/PID", String(processID), "/T", "/F"],
    sessionDiffTreeReapMilliseconds,
  )
  const wrapperClosed = await settlesBeforeDeadline(childClosed, deadline)
  return wrapperClosed && (taskkill.code === 0 || taskkill.code === 128)
}

const createChangesInvocation = (
  manifestJson,
  abortSignal,
  changedFilePaths,
) => {
  const changedFilesRequest = Array.isArray(changedFilePaths)
  const requestDescription = changedFilesRequest
    ? "selected changed-file"
    : "Session-diff"
  let cancelInvocation = async () => {}
  const result = new Promise((resolve, reject) => {
    if (!changedFilesRequest &&
        (typeof manifestJson !== "string" || manifestJson.length === 0)) {
      reject(sessionDiffFailure(
        "The Logonomic Session-diff manifest must be a non-empty string.",
      ))
      return
    }
    if (!changedFilesRequest && Buffer.byteLength(manifestJson, "utf8") >
        maxSessionDiffManifestBytes) {
      reject(sessionDiffFailure(
        "The Logonomic Session-diff manifest exceeds the 192 MiB limit.",
      ))
      return
    }
    if (changedFilesRequest && (changedFilePaths.length === 0 ||
        changedFilePaths.some((path) =>
          typeof path !== "string" || path.length === 0))) {
      reject(sessionDiffFailure(
        "The Logonomic changed-file selection requires non-empty paths.",
      ))
      return
    }

    const wrapperPath = process.env.LOGONOMIC_VNMCTL_PATH
    if (!wrapperPath) {
      reject(sessionDiffFailure(
        "LOGONOMIC_VNMCTL_PATH is unavailable in this Logonomic session.",
      ))
      return
    }
    if (abortSignal?.aborted) {
      reject(sessionDiffFailure(
        `The Logonomic ${requestDescription} request was cancelled.`,
      ))
      return
    }

    let child
    let settled = false
    let closeResult
    let closeResultResolve
    let cancellationFailure
    let cancellationPromise
    let processTreeSettlementPromise
    let startFailure
    let outputBytes = 0
    const stdout = []
    const stderr = []
    const childClosed = new Promise((closedResolve) => {
      closeResultResolve = closedResolve
    })
    const settle = (failure, result) => {
      if (settled) return
      settled = true
      abortSignal?.removeEventListener("abort", abort)
      if (failure) reject(failure)
      else resolve(result)
    }
    const ensureProcessTreeSettled = () => {
      if (!processTreeSettlementPromise) {
        // Defer the owner body so the shared promise is installed before any
        // cleanup callback can synchronously request cancellation.
        processTreeSettlementPromise = Promise.resolve().then(async () => {
          if (!child?.pid) return true
          if (cancellationFailure && !closeResult) {
            const cooperative = await requestCooperativeCancellation(
              wrapperPath,
              child.pid,
            )
            const closedCooperatively = cooperative &&
              await settlesBeforeDeadline(
                childClosed,
                Date.now() +
                  sessionDiffCooperativeCancellationMilliseconds,
              )
            if (process.platform !== "win32" &&
                posixProcessGroupExists(child.pid)) {
              return hardTerminateTree(child.pid, childClosed)
            }
            if (!closedCooperatively) {
              return hardTerminateTree(child.pid, childClosed)
            }
            return true
          }
          if (process.platform !== "win32" &&
              posixProcessGroupExists(child.pid)) {
            return hardTerminateTree(child.pid, childClosed)
          }
          return true
        })
      }
      return processTreeSettlementPromise
    }
    const settleFromClose = async () => {
      if (settled || cancellationFailure || !closeResult) return
      const reaped = await ensureProcessTreeSettled()
      if (cancellationFailure || settled) return
      if (!reaped) {
        settle(sessionDiffFailure(
          `The Logonomic ${requestDescription} wrapper exited but its ` +
            "process tree " +
            "could not be reaped.",
        ))
        return
      }
      if (startFailure) {
        settle(startFailure)
        return
      }
      const standardOutput = Buffer.concat(stdout).toString("utf8")
      const standardError = Buffer.concat(stderr).toString("utf8")
      if (closeResult.code !== 0) {
        const reason = standardError.trim() || standardOutput.trim() ||
          (closeResult.signal
            ? `The wrapper was terminated by ${closeResult.signal}.`
            : `The wrapper exited with status ${closeResult.code}.`)
        settle(sessionDiffFailure(reason, {
          exitCode: closeResult.code,
          signal: closeResult.signal,
          stderr: standardError,
          stdout: standardOutput,
        }))
        return
      }
      settle(undefined, standardOutput || "Changes sent to Logonomic.")
    }
    const requestCancellation = (failure) => {
      if (settled) return Promise.resolve()
      if (cancellationPromise) return cancellationPromise
      cancellationFailure = failure
      cancellationPromise = (async () => {
        if (!child?.pid) {
          settle(failure)
          return
        }
        const reaped = await ensureProcessTreeSettled()
        if (!reaped) {
          settle(sessionDiffFailure(
            `${failure.message} Logonomic could not confirm that the wrapper ` +
              "process tree was terminated.",
            { cause: failure },
          ))
          return
        }
        settle(failure)
      })()
      return cancellationPromise
    }
    cancelInvocation = (message) => requestCancellation(sessionDiffFailure(
      message,
    ))
    const abort = () => {
      void cancelInvocation(
        `The Logonomic ${requestDescription} request was cancelled.`,
      )
    }
    const capture = (destination, chunk) => {
      if (cancellationFailure) return
      outputBytes += chunk.length
      if (outputBytes > maxSessionDiffResultBytes) {
        void requestCancellation(sessionDiffFailure(
          `The Logonomic ${requestDescription} wrapper returned too much output.`,
        ))
        return
      }
      destination.push(chunk)
    }

    try {
      child = spawn(
        wrapperPath,
        changedFilesRequest
          ? [
              "call",
              "logonomic.diff",
              "show_changed_files",
              "--",
              ...changedFilePaths,
            ]
          : ["call", "logonomic.diff", "show", "--manifest-stdin"],
        {
          env: process.env,
          detached: process.platform !== "win32",
          shell: false,
          stdio: [changedFilesRequest ? "ignore" : "pipe", "pipe", "pipe"],
          windowsHide: true,
        },
      )
      child.stdout.on("data", (chunk) => capture(stdout, chunk))
      child.stderr.on("data", (chunk) => capture(stderr, chunk))
      child.once("error", (error) => {
        startFailure = sessionDiffFailure(
          `The Logonomic ${requestDescription} wrapper could not be started: ` +
            error.message,
          { cause: error },
        )
      })
      child.once("close", (code, signal) => {
        closeResult = { code, signal }
        closeResultResolve(closeResult)
        void settleFromClose()
      })
      if (!changedFilesRequest) {
        child.stdin.once("error", (error) => {
          void requestCancellation(sessionDiffFailure(
            `The Logonomic Session-diff manifest could not be sent: ${error.message}`,
            { cause: error },
          ))
        })
      }
      abortSignal?.addEventListener("abort", abort, { once: true })
      if (abortSignal?.aborted) {
        abort()
        return
      }
      if (!changedFilesRequest) child.stdin.end(manifestJson, "utf8")
    } catch (error) {
      const failure = sessionDiffFailure(
        `The Logonomic ${requestDescription} wrapper could not be started: ` +
          error.message,
        { cause: error },
      )
      if (child?.pid) void requestCancellation(failure)
      else settle(failure)
    }
  })
  return {
    result,
    cancel: (message) => cancelInvocation(message),
    disposalMessage: changedFilesRequest
      ? "The Logonomic changed-file request was cancelled because the " +
        "OpenCode plugin was disposed."
      : "The Logonomic Session-diff request was cancelled because the " +
        "OpenCode plugin was disposed.",
  }
}

export const Logonomic = async ({ client }) => {
  const managedDeliveryTimeoutMilliseconds =
    "47000"
  const deliveryTimeoutMilliseconds = positiveMilliseconds(
    managedDeliveryTimeoutMilliseconds,
  )
  const disposeTimeoutMilliseconds =
    clientLookupTimeoutMilliseconds +
    maxProjectedDeliveriesPerItem *
      ((deliveryTimeoutMilliseconds ?? 0) +
        deliveryReapTimeoutMilliseconds) +
    finalCleanupReservationMilliseconds
  const rootTurnIDs = new Map()
  const pendingRootTurns = new Map()
  const degradedRootSessions = new Set()
  const sessionRecords = new Map()
  const activeChildRuns = new Map()
  const stoppedChildRuns = new Map()
  const pendingItems = []
  const lookupControllers = new Set()
  const lookupPromises = new Set()
  const activeSessionDiffInvocations = new Set()
  let accepting = true
  let lookupsAllowed = true
  let deliveryCutoffReached = false
  let workerPromise
  let cancelActiveDelivery
  let stopLookupsPromise
  let disposePromise
  let sessionDiffDisposalStarted = false
  let incompleteDrainReported = false

  const evictInactiveChildSession = () => {
    for (const [childSessionID, record] of sessionRecords) {
      if (record.kind !== "child") continue
      if (activeChildRuns.has(childSessionID)) continue
      sessionRecords.delete(childSessionID)
      stoppedChildRuns.delete(childSessionID)
      return true
    }
    return false
  }

  const childSessionCount = () => {
    let count = 0
    for (const record of sessionRecords.values()) {
      if (record.kind === "child") ++count
    }
    return count
  }

  const rememberStoppedChildRun = (childSessionID) => {
    stoppedChildRuns.delete(childSessionID)
    stoppedChildRuns.set(childSessionID, true)
    if (stoppedChildRuns.size <= maxChildSessions) return
    stoppedChildRuns.delete(stoppedChildRuns.keys().next().value)
  }

  const childObservationLoss = () => {
    accepting = false
    pendingItems.length = 0
    return { event: "logonomic.delivery_overflow" }
  }

  const clearSessionProjectionState = (sessionID) => {
    rootTurnIDs.delete(sessionID)
    pendingRootTurns.delete(sessionID)
    degradedRootSessions.delete(sessionID)
    activeChildRuns.delete(sessionID)
  }

  const clearRootCorrelationState = (sessionID) => {
    rootTurnIDs.delete(sessionID)
    pendingRootTurns.delete(sessionID)
    degradedRootSessions.delete(sessionID)
  }

  const classifiedSessionKind = (sessionID) =>
    sessionRecords.get(sessionID)?.kind

  const reduceSessionFact = (info, source) => {
    const sessionID = metadata(info?.id)
    if (!sessionID) return
    const retained = sessionRecords.get(sessionID)
    const suppliedParentID = info?.parentID
    const isRoot = suppliedParentID === undefined ||
      suppliedParentID === null || suppliedParentID === ""
    if (isRoot) {
      if (retained?.kind === "child" && source !== "created") {
        return "child"
      }
      if (retained?.kind === "unresolved" && source === "lookup") return
      const evidence = source === "lookup" ? "inferred" : "observed"
      const published = retained?.kind === "root" && retained.published === true
      if (source === "created") stoppedChildRuns.delete(sessionID)
      if (retained?.kind === "child") clearSessionProjectionState(sessionID)
      sessionRecords.set(sessionID, {
        kind: "root",
        evidence: retained?.kind === "root" &&
            retained.evidence === "observed"
          ? "observed"
          : evidence,
        published,
      })
      return "root"
    }

    const parentID = metadata(suppliedParentID)
    if (!parentID) {
      if (source === "lookup") return
      if (retained?.kind === "root" && retained.published) return "root"
      if (retained?.kind === "root" || !retained ||
          retained.kind === "unresolved") {
        clearSessionProjectionState(sessionID)
        sessionRecords.set(sessionID, { kind: "unresolved" })
        return "unresolved"
      }
      return retained.kind
    }

    if (retained?.kind === "root") {
      if (retained.published ||
          (source === "created" && retained.evidence === "observed")) {
        return "root"
      }
    } else if (retained?.kind === "child" &&
        retained.parentID !== parentID) {
      return "child"
    } else if (retained?.kind === "unresolved" && source === "lookup") {
      return
    }
    if (retained?.kind !== "child" &&
        childSessionCount() >= maxChildSessions &&
        !evictInactiveChildSession()) return childObservationCapacityLost
    const agentLabel = metadata(info?.agent)
    const record = { kind: "child", parentID }
    if (retained?.kind === "child" && retained.rootSessionID) {
      record.rootSessionID = retained.rootSessionID
    }
    if (agentLabel) record.agentLabel = agentLabel
    else if (retained?.kind === "child" && retained.agentLabel) {
      record.agentLabel = retained.agentLabel
    }
    clearRootCorrelationState(sessionID)
    sessionRecords.set(sessionID, record)
    return "child"
  }

  const sessionInfo = async (sessionID) => {
    if (!lookupsAllowed || deliveryCutoffReached) return
    const controller = new AbortController()
    const timeoutToken = Symbol("lookup timeout")
    let timeout
    let abort
    const settled = Promise.resolve()
      .then(() => client.session.get({
        path: { id: sessionID },
        signal: controller.signal,
      }))
      .then(
        (response) => ({ response }),
        () => ({}),
      )
    const interrupted = new Promise((resolve) => {
      abort = () => resolve(timeoutToken)
      controller.signal.addEventListener("abort", abort, { once: true })
    })
    const completion = Promise.race([
      settled,
      interrupted,
      new Promise((resolve) => {
        timeout = setTimeout(() => {
          controller.abort()
          resolve(timeoutToken)
        }, clientLookupTimeoutMilliseconds)
      }),
    ])
    lookupControllers.add(controller)
    lookupPromises.add(completion)
    try {
      const outcome = await completion
      if (outcome === timeoutToken) return
      return outcome.response?.data
    } finally {
      clearTimeout(timeout)
      controller.signal.removeEventListener("abort", abort)
      lookupControllers.delete(controller)
      lookupPromises.delete(completion)
    }
  }

  const stopLookups = async () => {
    lookupsAllowed = false
    for (const controller of lookupControllers) controller.abort()
    while (lookupPromises.size > 0) {
      await Promise.all(lookupPromises)
    }
  }

  const stopLookupsOnce = () => {
    stopLookupsPromise ??= stopLookups()
    return stopLookupsPromise
  }

  const reportIncompleteDrain = () => {
    if (incompleteDrainReported) return
    incompleteDrainReported = true
    reportDeliveryFailure(
      "plugin disposal cancelled an incomplete delivery drain",
    )
  }

  const resolveSessionKind = async (sessionID) => {
    const retained = sessionRecords.get(sessionID)
    if (retained?.kind === "child") return "child"
    if (retained?.kind === "unresolved") return
    if (retained?.kind === "root" &&
        (retained.evidence === "observed" || retained.published)) {
      return "root"
    }

    const lookedUp = await sessionInfo(sessionID)
    if (metadata(lookedUp?.id) !== sessionID) return
    return reduceSessionFact(lookedUp, "lookup") ??
      classifiedSessionKind(sessionID)
  }

  const managedRootForChild = async (
    childSessionID,
    allowUnpublishedRoot = false,
  ) => {
    const initialKind = await resolveSessionKind(childSessionID)
    if (initialKind !== "child") return
    const traversed = []
    const visited = new Set()
    let sessionID = childSessionID
    let depth = 0
    while (true) {
      if (visited.has(sessionID)) return
      visited.add(sessionID)
      const child = sessionRecords.get(sessionID)
      if (child?.kind !== "child") {
        if (child?.kind !== "root" ||
            (!child.published && !allowUnpublishedRoot)) return
        for (const record of traversed) record.rootSessionID = sessionID
        return sessionID
      }
      const retainedRoot = sessionRecords.get(child.rootSessionID)
      if (child.rootSessionID && retainedRoot?.kind === "root" &&
          (retainedRoot.published || allowUnpublishedRoot)) {
        for (const record of traversed) {
          record.rootSessionID = child.rootSessionID
        }
        return child.rootSessionID
      }
      if (depth >= maxChildParentDepth) return
      ++depth
      traversed.push(child)
      sessionID = child.parentID
      const kind = await resolveSessionKind(sessionID)
      if (kind === childObservationCapacityLost) {
        return childObservationCapacityLost
      }
      if (!kind) return
    }
  }

  const childProjection = (eventName, rootSessionID, childSessionID, child) => {
    const projected = {
      event: eventName,
      session_id: rootSessionID,
      agent_id: childSessionID,
    }
    if (child.agentLabel) projected.agent_label = child.agentLabel
    return projected
  }

  const startChildRun = async (childSessionID) => {
    if (activeChildRuns.has(childSessionID)) return
    const rootSessionID = await managedRootForChild(childSessionID)
    if (rootSessionID === childObservationCapacityLost) {
      return childObservationCapacityLost
    }
    const child = sessionRecords.get(childSessionID)
    if (!rootSessionID || !child ||
        child.kind !== "child" ||
        sessionRecords.get(rootSessionID)?.published !== true ||
        activeChildRuns.size >= maxChildSessions) return
    const active = {
      rootSessionID,
      agentLabel: child.agentLabel,
    }
    activeChildRuns.set(childSessionID, active)
    stoppedChildRuns.delete(childSessionID)
    return childProjection(
      "logonomic.subagent.started",
      rootSessionID,
      childSessionID,
      active,
    )
  }

  const stopChildRun = async (childSessionID, authoritative = false) => {
    const active = activeChildRuns.get(childSessionID)
    if (active) {
      activeChildRuns.delete(childSessionID)
      rememberStoppedChildRun(childSessionID)
      return childProjection(
        "logonomic.subagent.stopped",
        active.rootSessionID,
        childSessionID,
        active,
      )
    }
    if (!authoritative || stoppedChildRuns.has(childSessionID)) return
    const rootSessionID = await managedRootForChild(childSessionID, true)
    if (rootSessionID === childObservationCapacityLost) {
      return childObservationCapacityLost
    }
    const child = sessionRecords.get(childSessionID)
    if (!rootSessionID || child?.kind !== "child") return
    rememberStoppedChildRun(childSessionID)
    return childProjection(
      "logonomic.subagent.stopped",
      rootSessionID,
      childSessionID,
      child,
    )
  }

  const forgetChildrenOfRoot = (rootSessionID) => {
    for (const [childSessionID, active] of activeChildRuns) {
      if (active.rootSessionID === rootSessionID) {
        activeChildRuns.delete(childSessionID)
        rememberStoppedChildRun(childSessionID)
      }
    }
    const forgotten = new Set()
    for (let pass = 0; pass < maxChildSessions; ++pass) {
      let changed = false
      for (const [childSessionID, child] of sessionRecords) {
        if (child.kind === "child" && !forgotten.has(childSessionID) &&
            (child.rootSessionID === rootSessionID ||
             child.parentID === rootSessionID ||
             forgotten.has(child.parentID))) {
          forgotten.add(childSessionID)
          changed = true
        }
      }
      if (!changed) break
    }
    for (const childSessionID of forgotten) {
      sessionRecords.delete(childSessionID)
      clearSessionProjectionState(childSessionID)
    }
  }

  const degradeTurnCorrelation = (sessionID, degradationType) => {
    if (degradedRootSessions.has(sessionID)) return
    degradedRootSessions.add(sessionID)
    rootTurnIDs.delete(sessionID)
    pendingRootTurns.delete(sessionID)
    return {
      event: "logonomic.turn_correlation_degraded",
      session_id: sessionID,
      degradation_type: degradationType,
    }
  }

  const markTurnCorrelationAmbiguous = (sessionID, degradationType) => {
    const kind = classifiedSessionKind(sessionID)
    if (kind === "child" || kind === "unresolved") return
    rootTurnIDs.delete(sessionID)
    const pending = pendingRootTurns.get(sessionID) ?? { turnIDs: [] }
    pending.turnIDs.length = 0
    pending.confirmedTurnID = undefined
    pending.degradationType ??= degradationType
    pendingRootTurns.set(sessionID, pending)
    if (classifiedSessionKind(sessionID) === "root") {
      return degradeTurnCorrelation(sessionID, pending.degradationType)
    }
  }

  const projectEvent = async (event) => {
    const properties = event?.properties
    if (!properties || typeof properties !== "object") return

    if (event.type === "session.created" ||
        event.type === "session.updated") {
      const sessionID = metadata(properties.info?.id)
      if (!sessionID) return
      const kind = reduceSessionFact(
        properties.info,
        event.type === "session.created" ? "created" : "updated",
      )
      if (kind === childObservationCapacityLost) {
        return childObservationLoss()
      }
      return
    }

    if (event.type === "session.deleted") {
      const sessionID = metadata(properties.info?.id)
      if (!sessionID) return
      const reduced = reduceSessionFact(properties.info, "deleted")
      const kind = reduced ?? classifiedSessionKind(sessionID)
      if (kind === childObservationCapacityLost) {
        return childObservationLoss()
      }
      const publishedRoot = sessionRecords.get(sessionID)?.published === true
      const degradationType = pendingRootTurns.get(sessionID)
        ?.degradationType
      const projected = degradationType && kind === "root" && publishedRoot
        ? degradeTurnCorrelation(sessionID, degradationType)
        : undefined
      if (kind === "child") {
        const stopped = await stopChildRun(
          sessionID,
          true,
        )
        sessionRecords.delete(sessionID)
        clearSessionProjectionState(sessionID)
        if (stopped === childObservationCapacityLost) {
          return childObservationLoss()
        }
        return stopped
      }
      forgetChildrenOfRoot(sessionID)
      sessionRecords.delete(sessionID)
      clearSessionProjectionState(sessionID)
      if (kind !== "root" || !publishedRoot) return
      const deletion = { event: event.type, session_id: sessionID }
      return projected ? [projected, deletion] : deletion
    }

    if (event.type === "message.updated") {
      const info = properties.info
      const sessionID = metadata(info?.sessionID)
      const messageID = metadata(info?.id)
      const parentID = metadata(info?.parentID)
      if (
        info?.role !== "assistant" ||
        !sessionID ||
        !messageID ||
        !parentID ||
        ["child", "unresolved"].includes(
          classifiedSessionKind(sessionID),
        )
      ) return
      if (degradedRootSessions.has(sessionID)) return
      const pending = pendingRootTurns.get(sessionID)
      if (
        !pending ||
        pending.degradationType ||
        !pending.turnIDs.includes(parentID)
      ) return
      if (
        pending.confirmedTurnID &&
        pending.confirmedTurnID !== parentID
      ) {
        return markTurnCorrelationAmbiguous(
          sessionID,
          "conflicting_confirmation",
        )
      }
      pending.confirmedTurnID = parentID
      return
    }

    const sessionID = metadata(properties.sessionID)
    if (!sessionID) return
    const kind = await resolveSessionKind(sessionID)
    if (kind === childObservationCapacityLost) {
      return childObservationLoss()
    }
    if (kind === "child") {
      if (event.type === "session.status") {
        const statusType = properties.status?.type
        if (statusType === "busy" || statusType === "retry") {
          const started = await startChildRun(sessionID)
          return started === childObservationCapacityLost
            ? childObservationLoss()
            : started
        }
        if (statusType === "idle") {
          const stopped = await stopChildRun(
            sessionID,
            true,
          )
          return stopped === childObservationCapacityLost
            ? childObservationLoss()
            : stopped
        }
        return
      }
      if (event.type === "session.idle" ||
          event.type === "session.error") {
        const stopped = await stopChildRun(
          sessionID,
          true,
        )
        return stopped === childObservationCapacityLost
          ? childObservationLoss()
          : stopped
      }
      return
    }
    if (kind !== "root") return

    const pendingDegradationType = pendingRootTurns.get(sessionID)
      ?.degradationType
    if (pendingDegradationType) {
      return degradeTurnCorrelation(
        sessionID,
        pendingDegradationType,
      )
    }

    if (degradedRootSessions.has(sessionID)) return

    if (event.type === "session.status") {
      const statusType = properties.status?.type
      if (!["busy", "retry", "idle"].includes(statusType)) return
      let turnID = rootTurnIDs.get(sessionID)
      if (statusType === "busy" || statusType === "retry") {
        const pending = pendingRootTurns.get(sessionID)
        if (pending?.confirmedTurnID) {
          turnID = pending.confirmedTurnID
          pendingRootTurns.delete(sessionID)
          rootTurnIDs.set(sessionID, turnID)
        }
        if (!turnID) return
        const root = sessionRecords.get(sessionID)
        if (root?.kind !== "root") return
        sessionRecords.set(sessionID, {
          ...root,
          published: true,
        })
        return {
          event: event.type,
          session_id: sessionID,
          turn_id: turnID,
          status_type: statusType,
        }
      } else {
        rootTurnIDs.delete(sessionID)
        pendingRootTurns.delete(sessionID)
        if (!turnID) return
      }
      return {
        event: event.type,
        session_id: sessionID,
        turn_id: turnID,
        status_type: statusType,
      }
    }

    if (event.type === "session.idle") {
      const turnID = rootTurnIDs.get(sessionID)
      rootTurnIDs.delete(sessionID)
      pendingRootTurns.delete(sessionID)
      if (!turnID) return
      return {
        event: event.type,
        session_id: sessionID,
        turn_id: turnID,
      }
    }

    if (event.type === "session.error") {
      const turnID = rootTurnIDs.get(sessionID)
      rootTurnIDs.delete(sessionID)
      pendingRootTurns.delete(sessionID)
      if (!turnID) return
      return {
        event: event.type,
        session_id: sessionID,
        turn_id: turnID,
        error_type: properties.error?.name === "MessageAbortedError"
          ? "aborted"
          : "failed",
      }
    }

    const turnID = rootTurnIDs.get(sessionID)
    if (!turnID) return

    if (event.type === "permission.asked") {
      const permissionID = metadata(properties.id)
      if (!permissionID) return
      return {
        event: event.type,
        session_id: sessionID,
        turn_id: turnID,
        permission_id: permissionID,
      }
    }

    if (event.type === "permission.replied") {
      const permissionID = metadata(properties.requestID)
      if (!permissionID) return
      return {
        event: event.type,
        session_id: sessionID,
        turn_id: turnID,
        permission_id: permissionID,
      }
    }
  }

  const projectChatMessage = async ({ input, output }) => {
    const sessionID = metadata(input?.sessionID)
    const turnID = metadata(output?.message?.id)
    if (!sessionID || !turnID || !Array.isArray(output?.parts)) return
    if (!output.parts.some((part) =>
      part && typeof part === "object" && part.synthetic !== true)) return
    const kind = classifiedSessionKind(sessionID)
    if (kind === "child" || kind === "unresolved") return
    if (degradedRootSessions.has(sessionID)) return
    const pending = pendingRootTurns.get(sessionID)
    if (!pending) {
      pendingRootTurns.set(sessionID, {
        turnIDs: [turnID],
      })
      return
    }
    if (
      pending.degradationType ||
      pending.turnIDs.includes(turnID)
    ) return
    if (pending.turnIDs.length >= maxPendingTurnCandidates) {
      return markTurnCorrelationAmbiguous(sessionID, "candidate_capacity")
    }
    pending.turnIDs.push(turnID)
  }

  const deliver = (projected) => new Promise((resolve) => {
    if (deliveryCutoffReached) {
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
    if (!deliveryTimeoutMilliseconds) {
      reportDeliveryFailure("the owned relay timeout is unavailable")
      resolve()
      return
    }

    let child
    let deliveryTimer
    let settled = false
    let terminating = false
    let childFailure
    let terminationFailure
    const finish = (failure) => {
      if (settled) return
      settled = true
      clearTimeout(deliveryTimer)
      child?.stdin.destroy()
      if (cancelActiveDelivery === terminate) {
        cancelActiveDelivery = undefined
      }
      if (failure) reportDeliveryFailure(failure)
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

    if (deliveryCutoffReached) {
      finish()
      return
    }
    try {
      child = spawn(relayPath, ["event", "--watch-parent"], {
        stdio: ["pipe", "ignore", "inherit"],
        windowsHide: true,
      })
      cancelActiveDelivery = terminate
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

  const project = async (item) => {
    if (item.kind === "overflow") {
      return { event: "logonomic.delivery_overflow" }
    }
    if (item.kind === "chat.message") return projectChatMessage(item)
    return projectEvent(item.event)
  }

  const startWorker = () => {
    if (workerPromise || deliveryCutoffReached) return
    workerPromise = (async () => {
      while (!deliveryCutoffReached && pendingItems.length > 0) {
        const projected = await project(pendingItems.shift())
        if (!projected || deliveryCutoffReached) continue
        const deliveries = Array.isArray(projected)
          ? projected
          : [projected]
        for (const delivery of deliveries) {
          if (deliveryCutoffReached) break
          await deliver(delivery)
        }
      }
    })().finally(() => {
      workerPromise = undefined
      if (!deliveryCutoffReached && pendingItems.length > 0) startWorker()
    })
  }

  const enqueue = (item) => {
    if (!accepting) return false
    if (pendingItems.length >= maxPendingItems) {
      accepting = false
      pendingItems.length = 0
      pendingItems.push({ kind: "overflow" })
      startWorker()
      return false
    }
    pendingItems.push(item)
    startWorker()
    return true
  }

  const awaitIdle = async () => {
    while (workerPromise) await workerPromise
  }

  const reachDeliveryCutoff = () => {
    let cutoffStarted = false
    if (!deliveryCutoffReached) {
      deliveryCutoffReached = true
      accepting = false
      pendingItems.length = 0
      cutoffStarted = true
    }
    const lookupsStopped = stopLookupsOnce()
    if (cutoffStarted) reportIncompleteDrain()
    cancelActiveDelivery?.(null)
    return lookupsStopped
  }

  const sessionDiffTool = {
    description:
      "Present an exact Session-change manifest in Logonomic's diff panel.",
    args: {
      manifest_json: {
        type: "string",
        description:
          "Exact UTF-8 JSON logonomic.session_diff version 1 manifest.",
      },
    },
    async execute({ manifest_json: manifestJson }, context) {
      if (sessionDiffDisposalStarted) {
        throw sessionDiffFailure(
          "The Logonomic Session-diff tool is unavailable because the " +
            "OpenCode plugin was disposed.",
        )
      }
      const invocation = createChangesInvocation(
        manifestJson,
        context?.abort,
      )
      activeSessionDiffInvocations.add(invocation)
      try {
        return {
          title: "Session changes presented",
          output: await invocation.result,
        }
      } finally {
        activeSessionDiffInvocations.delete(invocation)
      }
    },
  }

  const changedFilesTool = {
    description:
      "Open a native Logonomic 'Session-selected changed files' panel for " +
      "repository-relative paths already known from this Session's conversation " +
      "context. Use for requests to bring up, open, show, or inspect modified or " +
      "changed files when context already supplies their paths; this tool presents " +
      "context-selected paths and does not discover repository status. Shared-file " +
      "rows can include other Sessions' edits.",
    args: {
      paths: {
        type: "array",
        items: { type: "string" },
        description:
          "The complete ordered list of repository-relative paths already " +
          "known from this Session's conversation context. Preserve that " +
          "order and include only those paths.",
      },
    },
    async execute({ paths }, context) {
      if (sessionDiffDisposalStarted) {
        throw sessionDiffFailure(
          "The Logonomic changed-file tool is unavailable because the " +
            "OpenCode plugin was disposed.",
        )
      }
      const invocation = createChangesInvocation(
        undefined,
        context?.abort,
        paths,
      )
      activeSessionDiffInvocations.add(invocation)
      try {
        return {
          title: "Session-selected changed files shown",
          output: await invocation.result,
        }
      } finally {
        activeSessionDiffInvocations.delete(invocation)
      }
    },
  }

  const settleActiveSessionDiffInvocations = async () => {
    const activeInvocations = [...activeSessionDiffInvocations]
    await Promise.all(activeInvocations.map(async (invocation) => {
      await invocation.cancel(invocation.disposalMessage)
      await Promise.allSettled([invocation.result])
    }))
  }

  const dispose = () => {
    if (disposePromise) return disposePromise
    sessionDiffDisposalStarted = true
    disposePromise = (async () => {
      try {
        accepting = false
        const sessionDiffSettlement = settleActiveSessionDiffInvocations()
        const deadline = Date.now() + disposeTimeoutMilliseconds
        const drainDeadline =
          deadline - finalCleanupReservationMilliseconds
        let drained = !workerPromise || await settlesBeforeDeadline(
          awaitIdle(),
          drainDeadline,
        )
        if (drained) {
          drained = await settlesBeforeDeadline(stopLookupsOnce(), deadline)
          if (drained) {
            await sessionDiffSettlement
            return
          }
        }

        const lookupsStopped = reachDeliveryCutoff()
        // The helper owns wrapper termination and reaping. Disposal waits
        // for that owner to settle; a timer cannot establish process cleanup.
        await Promise.allSettled([lookupsStopped, awaitIdle()])
        await sessionDiffSettlement
      } finally {
        rootTurnIDs.clear()
        pendingRootTurns.clear()
        degradedRootSessions.clear()
        activeChildRuns.clear()
        stoppedChildRuns.clear()
        sessionRecords.clear()
      }
    })()
    return disposePromise
  }

  const relevantEvents = new Set([
    "session.created",
    "session.updated",
    "session.deleted",
    "message.updated",
    "session.status",
    "session.idle",
    "session.error",
    "permission.asked",
    "permission.replied",
  ])

  const hooks = {
    tool: {
      "logonomic_show_changes": sessionDiffTool,
      "logonomic_show_changed_files": changedFilesTool,
    },
    dispose,
  }
  if (process.env.OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED !== "1") {
    return hooks
  }
  hooks.event = ({ event }) => {
    try {
      if (relevantEvents.has(event?.type)) {
        enqueue({ kind: "event", event })
      }
    } catch {
      // The editor lifecycle must not depend on Logonomic availability.
    }
  }
  hooks["chat.message"] = async (input, output) => {
    try {
      enqueue({ kind: "chat.message", input, output })
    } catch {
      // The editor lifecycle must not depend on Logonomic availability.
    }
  }
  return hooks
}
