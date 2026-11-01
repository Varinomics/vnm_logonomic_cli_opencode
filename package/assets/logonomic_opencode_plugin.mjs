// Logonomic managed OpenCode event plugin

import { Buffer } from "node:buffer"
import { spawn } from "node:child_process"
import { join } from "node:path"
import { create_session_wake } from "./logonomic_opencode_wake.mjs"
import { create_event_delivery } from "./logonomic_opencode_relay.mjs"
import { Assistant_reply_evidence } from "./logonomic_opencode_reply.mjs"

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
// Deletion and assistant updates can each emit two independent deliveries.
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

const context_identity = (value) => {
  const text = metadata(value)
  return text && !/[\u0000-\u001f]/.test(text) &&
    Buffer.byteLength(text, "utf8") <= maxMetadataCharacters
    ? text : undefined
}

const context_count = (value) => Number.isSafeInteger(value) && value >= 0

const context_sample = (event) => {
  const info = event.properties?.info
  const compacted = event.type === "session.compacted"
  if (!compacted && (info?.role !== "assistant" || info.summary === true)) return
  const provider_session_id = context_identity(compacted
    ? event.properties?.sessionID : info.sessionID)
  const event_id = context_identity(event.id)
  if (!provider_session_id || !event_id) return
  if (compacted) return { provider_session_id, event_id, compacted: true }

  // Native assistant usage is replaced at each step. Its cache and reasoning
  // components are disjoint from input/output; billing totals are unrelated.
  const tokens = info.tokens
  const components = [tokens?.input, tokens?.output, tokens?.reasoning,
    tokens?.cache?.read, tokens?.cache?.write]
  const used_tokens = components.every(context_count)
    ? components.reduce((total, value) => total + value, 0) : undefined
  if (used_tokens === 0) return
  return {
    provider_session_id, event_id,
    provider_id: metadata(info.providerID),
    model_id: metadata(info.modelID),
    ...(context_count(used_tokens) ? { used_tokens } : { usage_unavailable: true }),
  }
}

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

export const Logonomic = async ({ client }, options = {}) => {
  const session_wake = process.env.OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED === "1"
    ? await create_session_wake(client, options.session_wake?.registration_file)
    : undefined
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
  const pending_questions = new Map()
  const pending_permissions = new Map()
  const assistant_replies = new Map()
  const reply_text = new Map()
  const hint_candidates = new Set()
  const pendingRootTurns = new Map()
  const degradedRootSessions = new Set()
  const sessionRecords = new Map()
  const activeChildRuns = new Map()
  const stoppedChildRuns = new Map()
  const context_samples = new Map()
  const pendingItems = []
  const lookupControllers = new Set()
  const lookupPromises = new Set()
  const activeSessionDiffInvocations = new Set()
  let accepting = true
  let lookupsAllowed = true
  let deliveryCutoffReached = false
  let workerPromise
  let stopLookupsPromise
  let disposePromise
  let sessionDiffDisposalStarted = false
  let incompleteDrainReported = false
  let context_model

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

  const clear_attention_state = (session_id, preserve_children = false) => {
    assistant_replies.delete(session_id)
    hint_candidates.delete(session_id)
    for (const pending of [pending_questions, pending_permissions]) {
      for (const [id, request] of pending) {
        if (preserve_children && request.agent_id) continue
        if (request.session_id === session_id || request.agent_id === session_id) pending.delete(id)
      }
    }
  }

  const complete_reply = (session_id, turn_id, projected) => {
    const reply = assistant_replies.get(session_id)
    const pending_question = [...pending_questions.values()]
      .some((question) => question.session_id === session_id)
    clear_attention_state(session_id, true)
    if (reply?.turn_id === turn_id && reply.normal &&
        reply.evidence !== undefined && !pending_question) {
      if (hint_candidates.size >= maxPendingItems) return childObservationLoss()
      projected.normal_completion = true
      projected.assistant_reply_evidence = reply.evidence
      hint_candidates.add(session_id)
    }
    return projected
  }

  const clearSessionProjectionState = (sessionID) => {
    clear_attention_state(sessionID)
    rootTurnIDs.delete(sessionID)
    pendingRootTurns.delete(sessionID)
    degradedRootSessions.delete(sessionID)
    activeChildRuns.delete(sessionID)
    context_samples.delete(sessionID)
  }

  const clearRootCorrelationState = (sessionID) => {
    clear_attention_state(sessionID, true)
    rootTurnIDs.delete(sessionID)
    pendingRootTurns.delete(sessionID)
    degradedRootSessions.delete(sessionID)
    context_samples.delete(sessionID)
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

  // A null timeout waits until the lookup settles or disposal aborts it.
  const lookup_client = async (
    lookup,
    timeoutMilliseconds = clientLookupTimeoutMilliseconds,
  ) => {
    if (!lookupsAllowed || deliveryCutoffReached) return
    const controller = new AbortController()
    const timeoutToken = Symbol("lookup timeout")
    let timeout
    let abort
    const settled = Promise.resolve()
      .then(() => lookup(controller.signal))
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
      ...(timeoutMilliseconds === null ? [] : [new Promise((resolve) => {
        timeout = setTimeout(() => {
          controller.abort()
          resolve(timeoutToken)
        }, timeoutMilliseconds)
      })]),
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

  const sessionInfo = (sessionID, timeoutMilliseconds = clientLookupTimeoutMilliseconds) =>
    lookup_client((signal) => client.session.get({
      path: { id: sessionID }, signal,
    }), timeoutMilliseconds)

  const native_context_window = async ({ provider_id, model_id }) => {
    if (!provider_id || !model_id) return
    if (context_model?.provider_id === provider_id && context_model.model_id === model_id) {
      return context_model.window
    }
    const providers = await lookup_client((signal) => client.provider.list({ signal }))
    const provider = Array.isArray(providers?.all)
      ? providers.all.find((item) => item?.id === provider_id) : undefined
    const window = provider?.models?.[model_id]?.limit?.context
    if (!context_count(window)) return
    // OpenCode reports zero for a configured model with no known capacity.
    context_model = { provider_id, model_id, window: window || undefined }
    return context_model.window
  }

  const project_context = async (name, sample) => {
    if (!sample || classifiedSessionKind(sample.provider_session_id) !== "root") return
    if (sample.compacted) {
      context_samples.delete(sample.provider_session_id)
      return { event: name, session_id: sample.provider_session_id, context_updates: [sample] }
    }
    const window = sample.usage_unavailable ? undefined : await native_context_window(sample)
    const previous = context_samples.get(sample.provider_session_id)
    if (previous && previous.used_tokens === sample.used_tokens &&
        previous.usage_unavailable === sample.usage_unavailable &&
        previous.provider_id === sample.provider_id && previous.model_id === sample.model_id &&
        previous.window === window) return
    context_samples.set(sample.provider_session_id, { ...sample, window })
    return {
      event: name,
      session_id: sample.provider_session_id,
      context_updates: [{
        provider_session_id: sample.provider_session_id,
        event_id: sample.event_id,
        ...(sample.usage_unavailable ? { usage_unavailable: true } : { used_tokens: sample.used_tokens }),
        ...(window !== undefined ? { context_window: window } : {}),
      }],
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
    clear_attention_state(sessionID, true)
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

  const project_attention = (event, session_id, source_session_id) => {
    const properties = event.properties
    const question = event.type.startsWith("question.")
    const asked = event.type.endsWith(".asked")
    const pending = question ? pending_questions : pending_permissions
    const request_id = metadata(asked ? properties.id : properties.requestID)
    if (!request_id) return
    const prior = pending.get(request_id)
    if (!asked) {
      if (!prior || prior.session_id !== session_id ||
          (prior.agent_id ?? prior.session_id) !== source_session_id) return
      if (event.type === "question.replied" && typeof properties.answer_nonempty !== "boolean") return
      pending.delete(request_id)
      return {
        ...prior, event: event.type,
        ...(event.type === "question.replied" ? { answer_nonempty: properties.answer_nonempty } : {}),
      }
    }

    const root_request = source_session_id === session_id
    const turn_id = root_request ? rootTurnIDs.get(session_id) : undefined
    if (root_request && !turn_id) return
    const tool = question ? properties.tool : undefined
    if (tool !== undefined &&
        (!metadata(tool?.messageID) || !metadata(tool?.callID) ||
         (root_request &&
          assistant_replies.get(session_id)?.message_id !== tool.messageID))) return
    if (prior) {
      if (prior.session_id !== session_id || prior.turn_id !== turn_id ||
          (prior.agent_id ?? prior.session_id) !== source_session_id) {
        return degradeTurnCorrelation(session_id, "conflicting_confirmation")
      }
      return
    }
    if (pending.size >= maxPendingItems) return childObservationLoss()
    // A background child can outlive the root turn or overlap its next turn.
    // Its native request identifies the child, not an active root turn.
    const projected = {
      event: event.type, session_id,
      ...(root_request ? { turn_id } : { agent_id: source_session_id }),
      ...(question ? { question_id: request_id } : { permission_id: request_id }),
      ...(tool ? { tool_message_id: tool.messageID, tool_call_id: tool.callID } : {}),
    }
    pending.set(request_id, projected)
    return projected
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
      if (rootTurnIDs.get(sessionID) === parentID ||
          pending?.turnIDs.includes(parentID)) {
        const retained = assistant_replies.get(sessionID)
        if (!retained && assistant_replies.size >= maxPendingItems) {
          return childObservationLoss()
        }
        const reply = retained?.message_id === messageID
          ? retained : { message_id: messageID, turn_id: parentID }
        reply.normal = info.finish === "stop" && !info.error &&
          Number.isFinite(info.time?.completed) && info.time.completed > 0
        assistant_replies.set(sessionID, reply)
      }
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

    const sourceSessionID = metadata(properties.sessionID)
    if (!sourceSessionID) return
    let sessionID = sourceSessionID
    const kind = await resolveSessionKind(sessionID)
    if (kind === childObservationCapacityLost) {
      return childObservationLoss()
    }
    const attentionEvent = [
      "permission.asked", "permission.replied",
      "question.asked", "question.replied", "question.rejected",
    ].includes(event.type)
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
      if (!attentionEvent) return
      // The native root UI presents its children's prompts, including children
      // still running after the root turn has completed.
      sessionID = await managedRootForChild(sourceSessionID)
      if (sessionID === childObservationCapacityLost) return childObservationLoss()
      if (!sessionID) return
    }
    else if (kind !== "root") return

    const pendingDegradationType = pendingRootTurns.get(sessionID)
      ?.degradationType
    if (pendingDegradationType) {
      return degradeTurnCorrelation(
        sessionID,
        pendingDegradationType,
      )
    }

    if (degradedRootSessions.has(sessionID)) return

    if (attentionEvent) return project_attention(event, sessionID, sourceSessionID)

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
      const projected = {
        event: event.type,
        session_id: sessionID,
        turn_id: turnID,
        status_type: statusType,
      }
      return complete_reply(sessionID, turnID, projected)
    }

    if (event.type === "session.idle") {
      const turnID = rootTurnIDs.get(sessionID)
      rootTurnIDs.delete(sessionID)
      pendingRootTurns.delete(sessionID)
      if (!turnID) return
      return complete_reply(sessionID, turnID, {
        event: event.type,
        session_id: sessionID,
        turn_id: turnID,
      })
    }

    if (event.type === "session.error") {
      clear_attention_state(sessionID, true)
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
    const submitted = hint_candidates.delete(sessionID)
      ? { event: "chat.message", session_id: sessionID, turn_id: turnID }
      : undefined
    const pending = pendingRootTurns.get(sessionID)
    if (!pending) {
      pendingRootTurns.set(sessionID, {
        turnIDs: [turnID],
      })
      return submitted
    }
    if (
      pending.degradationType ||
      pending.turnIDs.includes(turnID)
    ) return
    if (pending.turnIDs.length >= maxPendingTurnCandidates) {
      return markTurnCorrelationAmbiguous(sessionID, "candidate_capacity")
    }
    pending.turnIDs.push(turnID)
    return submitted
  }

  const project_completed_text = ({ session_id, message_id, evidence }) => {
    const reply = assistant_replies.get(session_id)
    if (reply?.message_id === message_id) reply.evidence = evidence
  }

  // Logonomic resumes a session by relaunching OpenCode with --session, which
  // opens it without emitting any event. The resumed session is reported once
  // this OpenCode's own store has it, so Logonomic can confirm the resume
  // before the user's first turn. The lookup waits for the instance to finish
  // bootstrapping, however long that takes: Logonomic's own resume deadline
  // decides failure, and disposal ends the wait.
  const confirmResumedSession = async (sessionID) => {
    const info = await sessionInfo(sessionID, null)
    if (metadata(info?.id) !== sessionID) return
    enqueue({ kind: "resumed", info: { id: info.id, parentID: info.parentID } })
  }

  const projectResumedSession = ({ info }) => {
    if (reduceSessionFact(info, "lookup") !== "root") return
    return { event: "logonomic.session.resumed", session_id: info.id }
  }

  const event_delivery = create_event_delivery(
    deliveryTimeoutMilliseconds, () => !deliveryCutoffReached,
  )
  const deliver = event_delivery.deliver

  const project = async (item) => {
    if (item.kind === "overflow") {
      return { event: "logonomic.delivery_overflow" }
    }
    if (item.kind === "chat.message") return projectChatMessage(item)
    if (item.kind === "completed_text") return project_completed_text(item)
    if (item.kind === "resumed") return projectResumedSession(item)
    if (item.event.type === "session.compacted") {
      return project_context(item.event.type, item.context_sample)
    }
    if (item.event.type === "message.updated") {
      const lifecycle = await projectEvent(item.event)
      const context = await project_context(item.event.type, item.context_sample)
      return lifecycle && context ? [lifecycle, context] : lifecycle ?? context
    }
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
    event_delivery.cancel(null)
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
        await session_wake?.dispose()
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
        pending_questions.clear()
        assistant_replies.clear()
        reply_text.clear()
        hint_candidates.clear()
        pendingRootTurns.clear()
        degradedRootSessions.clear()
        activeChildRuns.clear()
        stoppedChildRuns.clear()
        sessionRecords.clear()
        context_samples.clear()
      }
    })()
    return disposePromise
  }

  const relevantEvents = new Set([
    "session.created",
    "session.updated",
    "session.deleted",
    "session.compacted",
    "message.updated",
    "session.status",
    "session.idle",
    "session.error",
    "permission.asked",
    "permission.replied",
    "question.asked",
    "question.replied",
    "question.rejected",
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
      session_wake?.event(event)
      // Release native text state at native event order, before queued helper
      // delivery. An older queued completion must not erase a newer reply.
      const info = event?.properties?.info
      if (event?.type === "message.updated" && Number.isFinite(info?.time?.completed) &&
          info.time.completed > 0 && reply_text.get(info.sessionID)?.message_id === info.id) {
        reply_text.delete(info.sessionID)
      } else if (event?.type === "session.deleted") {
        reply_text.delete(info?.id)
      } else if (["session.error", "session.idle"].includes(event?.type) ||
          (event?.type === "session.status" && event.properties?.status?.type === "idle")) {
        reply_text.delete(event.properties?.sessionID)
      }
      if (relevantEvents.has(event?.type)) {
        if (event.type.startsWith("question.")) {
          const properties = event.properties
          if (!properties || typeof properties !== "object") return
          // Only an indication crosses the helper boundary; native answer and
          // question prose must never enter lifecycle metadata or diagnostics.
          const answers = properties.answers
          const valid_answers = Array.isArray(answers) && answers.every((answer) =>
            Array.isArray(answer) && answer.every((value) => typeof value === "string"))
          event = { type: event.type, properties: {
            id: properties.id, sessionID: properties.sessionID, requestID: properties.requestID,
            ...(properties.tool !== undefined ? { tool: {
              messageID: properties.tool?.messageID, callID: properties.tool?.callID,
            } } : {}),
            ...(event.type === "question.replied" && valid_answers ? {
              answer_nonempty: answers.some((answer) => answer.some((value) => value.trim().length > 0)),
            } : {}),
          } }
        }
        enqueue({ kind: "event", event,
          ...(["message.updated", "session.compacted"].includes(event.type)
            ? { context_sample: context_sample(event) } : {}),
        })
      }
    } catch {
      // The editor lifecycle must not depend on Logonomic availability.
    }
  }
  hooks["chat.message"] = async (input, output) => {
    try {
      session_wake?.chat_message(input, output)
      enqueue({ kind: "chat.message", input, output })
    } catch {
      // The editor lifecycle must not depend on Logonomic availability.
    }
  }
  hooks["experimental.text.complete"] = async (input, output) => {
    const session_id = metadata(input?.sessionID)
    const message_id = metadata(input?.messageID)
    const part_id = metadata(input?.partID)
    if (!accepting || !session_id || !message_id || !part_id) return
    let retained = reply_text.get(session_id)
    if (retained?.message_id !== message_id) {
      if (!retained && reply_text.size >= maxPendingItems) {
        enqueue({ kind: "overflow" })
        return
      }
      retained = { message_id, scan: new Assistant_reply_evidence() }
      reply_text.set(session_id, retained)
    }
    if (retained.part_id === part_id) return
    retained.part_id = part_id
    retained.scan.append(output?.text)
    enqueue({ kind: "completed_text", session_id, message_id,
      evidence: retained.scan.snapshot() })
  }
  const resumedSessionID = metadata(
    process.env.OPENCODE_LOGONOMIC_RESUME_SESSION,
  )
  if (resumedSessionID) void confirmResumedSession(resumedSessionID)
  return hooks
}
