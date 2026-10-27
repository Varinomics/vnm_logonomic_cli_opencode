// The session-wake contract requires native admission independent of the input
// editor. This gate runs the real CLI against a loopback model and observes only
// its public plugin API. Terminal input below represents the fixture's user.
import assert from "node:assert/strict"
import { execFile, spawn } from "node:child_process"
import { randomBytes } from "node:crypto"
import { mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises"
import { createServer } from "node:http"
import { connect } from "node:net"
import { tmpdir } from "node:os"
import { delimiter, dirname, join, resolve } from "node:path"
import { promisify } from "node:util"

const [executable, package_directory, preparation_executable] = process.argv.slice(2)
assert(executable && package_directory && preparation_executable,
  "expected native OpenCode executable, built package directory and native preparation helper")
const root = await mkdtemp(join(tmpdir(), "opencode2-native-wake-"))
let registration
const snapshot_path = join(root, "native.json")
const initial_text = "Complete the initial native fixture turn without using tools."
const wake_text = "The sandbox dependency completed. Read this recipient's mailbox context."
const draft_text = "Preserve this unsent draft: verify the dependency migration."
const mailbox_identity = "native-recipient-mailbox"
const observer_directory = join(root, "observer")
const mailbox_script = join(root, "mailbox_context.mjs")
let child
let child_exit
let output = ""
let model_failure
let release_initial
let initial_held = false
let mailbox_tool_sent = false
let mailbox_observed = false

const read_json = async (path) => {
  try { return JSON.parse(await readFile(path, "utf8")) }
  catch (error) {
    if (error.code === "ENOENT") return undefined
    throw error
  }
}

const until = async (check, description) => {
  const deadline = Date.now() + 30000
  while (Date.now() < deadline) {
    if (model_failure) throw model_failure
    if (child?.exitCode !== null && child?.exitCode !== undefined) {
      throw new Error(`native CLI exited while waiting for ${description}: ${output}`)
    }
    const result = await check()
    if (result) return result
    await new Promise((resolve) => setTimeout(resolve, 30))
  }
  throw new Error(`timed out waiting for ${description}: ${output}`)
}

const shell_quote = (value) => process.platform === "win32"
  ? `"${value.replaceAll('"', '""')}"`
  : `'${value.replaceAll("'", "'\\''")}'`

const model = createServer(async (request, response) => {
  try {
    assert.equal(request.url, "/v1/chat/completions", "native request must use the fixture model endpoint")
    let body = ""
    for await (const chunk of request) body += chunk
    const prompt = JSON.parse(body)
    assert.equal(prompt.stream, true)
    if (!initial_held && prompt.tools?.length) {
      initial_held = true
      await new Promise((resolve) => { release_initial = resolve })
    }
    mailbox_observed ||= prompt.messages.some((message) =>
      message.role === "tool" && JSON.stringify(message.content).includes(mailbox_identity))
    const wake = JSON.stringify(prompt.messages).includes(wake_text)
    const call_mailbox = wake && !mailbox_tool_sent && prompt.tools?.some((tool) => tool.function.name === "shell")
    if (call_mailbox) mailbox_tool_sent = true
    const choice = call_mailbox
      ? { delta: { role: "assistant", tool_calls: [{
          index: 0, id: "call_native_mailbox", type: "function", function: {
            name: "shell", arguments: JSON.stringify({
              command: `${shell_quote(process.execPath)} ${shell_quote(mailbox_script)}`,
            }),
          },
        }] }, finish_reason: null }
      : { delta: { role: "assistant", content: wake
          ? "The recipient mailbox context was read."
          : "Initial native turn completed." }, finish_reason: null }
    response.writeHead(200, { "Content-Type": "text/event-stream" })
    for (const chunk of [choice, { delta: {}, finish_reason: call_mailbox ? "tool_calls" : "stop" }]) {
      response.write(`data: ${JSON.stringify({
        id: "native-fixture", object: "chat.completion.chunk", created: 1700000000,
        model: "wake", choices: [{ index: 0, ...chunk }],
      })}\n\n`)
    }
    response.end("data: [DONE]\n\n")
  }
  catch (error) {
    model_failure = error
    response.writeHead(500)
    response.end("Native fixture model failed")
  }
})

try {
  await mkdir(observer_directory)
  await writeFile(mailbox_script,
    'process.stdout.write(JSON.stringify({ mailbox: process.env.LOGONOMIC_SESSION_CAPABILITY }))\n')
  await writeFile(join(observer_directory, "tui.mjs"), `
import { createEffect, createRoot, createSignal } from "solid-js"
import { renameSync, writeFileSync } from "node:fs"

// Native discovery reruns setup when the combined plugin inventory arrives.
let initial_session

export default {
  id: "org.varinomics.native-wake-observer",
  async setup(context) {
    const events = []
    let stop_events
    const dispose = createRoot((dispose) => {
      const [revision, update_revision] = createSignal(0)
      stop_events = context.data.listen(({ details }) => {
        if (!details.type.startsWith("session.")) return
        events.push(details)
        update_revision((value) => value + 1)
      })
      createEffect(() => {
        revision()
        const route = context.ui.router.current()
        const session_id = route.type === "session" ? route.sessionID : undefined
        const snapshot = {
          version: context.app.version, route, selected_model: context.ui.model.current(), events,
          session: session_id ? context.data.session.get(session_id) : undefined,
          status: session_id ? context.data.session.status(session_id) : undefined,
          messages: session_id ? context.data.session.message.list(session_id) : [],
        }
        writeFileSync(context.options.snapshot + ".tmp", JSON.stringify(snapshot))
        renameSync(context.options.snapshot + ".tmp", context.options.snapshot)
      })
      return dispose
    })
    initial_session ??= (async () => {
      const session = await context.client.session.create({
        title: "Native wake fixture", agent: "plan",
        model: { providerID: "fixture", id: "wake", variant: "deliberate" },
        location: { directory: context.options.directory },
      })
      await context.client.session.prompt({ sessionID: session.id, text: context.options.initial_text })
      return session
    })()
    const session = await initial_session
    context.ui.router.navigate({ type: "session", sessionID: session.id })
    return () => { stop_events(); dispose() }
  },
}
`)
  const config_directory = join(root, "config", "opencode")
  await mkdir(config_directory, { recursive: true })
  await writeFile(join(config_directory, "cli.json"), JSON.stringify({ plugins: [{
    package: observer_directory, options: { snapshot: snapshot_path, directory: root, initial_text },
  }] }))
  await new Promise((resolve, reject) => {
    model.once("error", reject)
    model.listen(0, "127.0.0.1", resolve)
  })
  const environment = Object.fromEntries(["PATH", "SystemRoot", "WINDIR", "TEMP", "TMP", "COMSPEC", "PATHEXT"]
    .filter((name) => process.env[name]).map((name) => [name, process.env[name]]))
  Object.assign(environment, {
    PATH: dirname(resolve(executable)) + delimiter + (environment.PATH ?? ""),
    TERM: "xterm-256color",
    XDG_CONFIG_HOME: join(root, "config"), XDG_DATA_HOME: join(root, "data"),
    XDG_STATE_HOME: join(root, "state"), XDG_CACHE_HOME: join(root, "cache"),
    OPENCODE_DISABLE_MODELS_FETCH: "1", OPENCODE_DISABLE_CLAUDE_CODE: "1",
    OPENCODE_CONFIG_PROJECT_DISABLE: "1",
    OPENCODE_CONFIG_CONTENT: JSON.stringify({
      model: "fixture/wake",
      providers: { fixture: {
        name: "Isolated fixture", package: "@opencode/ai/providers/openai-compatible",
        settings: { baseURL: `http://127.0.0.1:${model.address().port}/v1`, apiKey: "local-fixture" },
        models: { wake: {
          name: "Wake fixture", limit: { context: 32000, output: 1024 },
          variants: [{ id: "deliberate" }],
        } },
      } },
    }),
  })
  const platform = process.platform === "win32" ? "windows-x86_64"
    : process.platform === "darwin" ? "macos-arm64" : "linux-x86_64"
  const request_path = join(root, "prepare.json")
  await writeFile(request_path, JSON.stringify({
    launcher: { script_body: "opencode --auto", runner_key: "direct", environment_text: "" },
    launcher_argv: [resolve(executable), "--auto"],
    launch_environment: { platform, effective_environment: environment },
    working_directory: root, title_instruction: "Keep the sandbox session title tied to its task.",
    declared_assets: [], session_wake: { version: 1 },
  }))
  const prepared = await promisify(execFile)(preparation_executable,
    [resolve(package_directory), root, request_path], { env: environment, timeout: 30000 })
  const patch = JSON.parse(prepared.stdout)
  assert.equal(patch.disposition.prepared, true)
  assert.equal(patch.session_wake.available, true, "actual packaged adapter must prepare native wake")
  assert(patch.argv.includes("--standalone"), "prepared native invocation must own its server")
  assert.equal(patch.working_directory, "inherit")
  registration = patch.session_wake.binding.registration_file
  assert.equal(patch.environment.set.OPENCODE_LOGONOMIC_WAKE_REGISTRATION, registration)
  for (const name of patch.environment.unset) delete environment[name]
  Object.assign(environment, patch.environment.set, { LOGONOMIC_SESSION_CAPABILITY: mailbox_identity })
  child = spawn(patch.argv[0], patch.argv.slice(1), {
    cwd: root, env: environment, windowsHide: true, stdio: ["pipe", "pipe", "pipe"],
  })
  child_exit = new Promise((resolve, reject) => { child.once("exit", resolve); child.once("error", reject) })
  for (const stream of [child.stdout, child.stderr]) {
    stream.on("data", (chunk) => { output = (output + chunk).slice(-50000) })
  }
  const binding = await until(() => read_json(registration), "native plugin registration")
  const initial = await until(async () => {
    const value = await read_json(snapshot_path)
    return value?.session && value.status === "running" && initial_held && value
  }, "initial native execution")
  const session_id = initial.session.id
  const transition_nonce = randomBytes(16).toString("hex")
  const bridge = (operation, target = session_id) => new Promise((resolve, reject) => {
    const socket = connect(binding.port, "127.0.0.1")
    let bytes = ""
    socket.setTimeout(5000, () => socket.destroy(new Error("native wake bridge timed out")))
    socket.once("error", reject)
    socket.once("connect", () => socket.write(`${JSON.stringify({
      ...binding, operation, provider_session_id: target, transition_nonce,
      ...(operation === "notify" ? { notification_id: "native_mail", instruction: wake_text } : {}),
    })}\n`))
    socket.on("data", (chunk) => { bytes += chunk })
    socket.once("end", () => { try { resolve(JSON.parse(bytes)) } catch (error) { reject(error) } })
  })
  assert.equal((await bridge("inspect")).available, false, "running conversation must not be wake-ready")
  assert.equal((await bridge("notify")).disposition, "deferred", "running conversation must defer admission")
  release_initial()
  const before = await until(async () => {
    const value = await read_json(snapshot_path)
    return value?.session?.outcome === "succeeded" && value.status === "idle" &&
      (await bridge("inspect")).available && value
  }, "completed idle native conversation")
  assert.equal((await bridge("inspect", "ses_unrelated_native_session")).available, false)

  child.stdin.write(draft_text)
  await until(() => output.includes(draft_text), "unsent draft in the native input editor")
  const accepted = await bridge("notify")
  assert.equal(accepted.disposition, "accepted")
  assert.deepEqual(await bridge("notify"), accepted, "one notification must retain one receipt")
  const after = await until(async () => {
    const value = await read_json(snapshot_path)
    return value?.session?.time.idle > before.session.time.idle &&
      value.status === "idle" && value.session.outcome === "succeeded" && value
  }, "native continuation after mailbox notification")
  assert.equal(after.session.id, session_id)
  assert.equal(after.session.agent, "plan")
  assert.deepEqual(after.session.model, before.session.model)
  assert.deepEqual(after.selected_model, before.selected_model)
  assert.equal(after.messages.filter((message) => message.type === "user").length, 1,
    "native wake must not submit the user's draft")
  assert.equal(after.messages.filter((message) => message.type === "synthetic" && message.text === wake_text).length, 1)
  assert(mailbox_tool_sent && mailbox_observed, "native server must retain this recipient's mailbox environment")

  child.stdin.write("\r")
  await until(async () => {
    const value = await read_json(snapshot_path)
    return value?.messages.filter((message) => message.type === "user" && message.text === draft_text).length === 1
  }, "original draft submitted unchanged by the fixture user")
  console.log(JSON.stringify({
    native_version: after.version, session_id,
    result: "passed", prepared_by_packaged_adapter: true, global_cli_plugin_preserved: true,
    busy_deferred: true, duplicate_synthetic_messages: 0,
    agent: after.session.agent, model: after.session.model,
    mailbox_environment_preserved: true, draft_preserved: true,
  }))
}
catch (error) {
  console.error(output.slice(-12000))
  throw error
}
finally {
  release_initial?.()
  if (child?.exitCode === null) {
    child.kill()
    await child_exit
  }
  model.closeAllConnections()
  await new Promise((resolve) => model.close(resolve))
  await rm(root, { recursive: true, force: true })
}
