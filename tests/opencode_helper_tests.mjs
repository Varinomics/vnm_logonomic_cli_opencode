// The SDK provider-event contract owns these oracles: committed acceptance is
// distinct from transport exit status, retries retain key/nonce/evidence, and
// the helper returns a committed response without logging its body.
import assert from "node:assert/strict"
import { spawnSync } from "node:child_process"
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"

const [helperArgument, wrapperArgument] = process.argv.slice(2)
assert(helperArgument && wrapperArgument, "Expected helper and test vnmctl paths")
const helper = resolve(helperArgument)
const scratch = mkdtempSync(join(tmpdir(), "opencode-helper-"))
const capture = join(scratch, "events.jsonl")
const requests = join(scratch, "requests.jsonl")
const environment = {
  ...process.env,
  LOGONOMIC_VNMCTL_PATH: resolve(wrapperArgument),
  LOGONOMIC_SESSION_CAPABILITY: "a".repeat(64),
  LOGONOMIC_PROVIDER_PACKAGE_ID: "com.varinomics.cli.opencode",
  LOGONOMIC_PROVIDER_GENERATION_DIGEST: "b".repeat(64),
  LOGONOMIC_PROVIDER_GENERATION_NONCE: "c".repeat(32),
  LOGONOMIC_PROVIDER_HELPERS_DIR: dirname(helper),
  OPENCODE_TEST_EVENT_CAPTURE: capture,
  OPENCODE_TEST_REQUEST_CAPTURE: requests,
}
const event = {
  event: "session.status",
  session_id: "session-primary",
  turn_id: "turn-user",
  status_type: "busy",
}
const run = (mode = "", input = event, env = environment) => {
  writeFileSync(capture, "")
  writeFileSync(requests, "")
  return spawnSync(helper, ["event"], {
    input: JSON.stringify(input),
    encoding: "utf8",
    timeout: 60_000,
    windowsHide: true,
    env: { ...env, OPENCODE_TEST_DELIVERY_MODE: mode },
  })
}
try {
  const accepted = run()
  assert.equal(accepted.error, undefined)
  assert.equal(accepted.status, 0, accepted.stderr)
  assert.deepEqual(JSON.parse(accepted.stdout), {
    accepted: true,
    helper_response: { delivered: true },
  })
  assert.equal(accepted.stderr, "")

  const retried = run("retry")
  assert.equal(retried.status, 0, retried.stderr)
  const deliveries = readFileSync(requests, "utf8").trim().split("\n").map(JSON.parse)
  assert.equal(deliveries.length, 2)
  assert.deepEqual(deliveries[0], deliveries[1])
  assert.match(deliveries[0].idempotency_key, /^[a-f0-9]{32}$/)
  assert.match(deliveries[0].transition_nonce, /^[a-f0-9]{32}$/)
  assert.deepEqual(deliveries[0].event, { name: event.event, body: event })

  const selected = { event: "logonomic.session.selected", session_id: "ses_A",
    api_generation: 2, status_type: "idle", activation_nonce: "d".repeat(32), execution_sequence: 0 }
  const selection = run("retry", selected)
  assert.equal(selection.status, 0, selection.stderr)
  const selected_deliveries = readFileSync(requests, "utf8").trim().split("\n").map(JSON.parse)
  assert.deepEqual(selected_deliveries[0], selected_deliveries[1])
  assert.equal(selected_deliveries[0].transition_nonce, selected.activation_nonce,
    "the native source binds the transition before asynchronous delivery")
  assert.equal(run("", { ...selected, execution_sequence: 1 }).status, 0)
  assert.notEqual(JSON.parse(readFileSync(requests, "utf8").trim()).transition_nonce, selected.activation_nonce,
    "later cwd facts retain source attribution without reusing a transition nonce for different evidence")
  assert.equal(run("", { ...selected, activation_nonce: "invalid" }).status, 2)

  const refused = run("refuse")
  assert.equal(refused.status, 1)
  assert.deepEqual(JSON.parse(refused.stdout), { accepted: false, reason: "retired_session" })
  assert.equal(readFileSync(requests, "utf8").trim().split("\n").length, 1)

  const invalid = run("", { event: 7 })
  assert.equal(invalid.status, 2)
  assert.equal(readFileSync(requests, "utf8"), "")
  const unbound = run("", event, { ...environment, LOGONOMIC_SESSION_CAPABILITY: "" })
  assert.equal(unbound.status, 2)
  assert.equal(readFileSync(requests, "utf8"), "")
} finally {
  rmSync(scratch, { recursive: true, force: true })
}
