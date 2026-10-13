// This opt-in gate runs an isolated native OpenCode with a loopback-only fake
// model. It never borrows provider credentials or sends a prompt to a paid API.
import assert from "node:assert/strict"
import { spawn } from "node:child_process"
import { randomBytes } from "node:crypto"
import { cp, mkdtemp, mkdir, readFile, rm } from "node:fs/promises"
import { createServer } from "node:http"
import { connect, createServer as create_tcp_server } from "node:net"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { pathToFileURL } from "node:url"

const [executable, plugin_path, dependency_seed] = process.argv.slice(2)
assert(executable && plugin_path, "expected native OpenCode executable and managed plugin")
const root = await mkdtemp(join(tmpdir(), "opencode-native-wake-"))
const registration = join(root, "wake.json")
const server_password = randomBytes(24).toString("hex")
let child
let child_exit
let output = ""
let model_calls = 0
const model = createServer(async (request, response) => {
  let body = ""
  for await (const chunk of request) body += chunk
  assert(request.url.endsWith("/chat/completions"), "native model request must use fixture endpoint")
  const prompt = JSON.parse(body)
  ++model_calls
  if (prompt.stream) {
    response.writeHead(200, { "Content-Type": "text/event-stream" })
    for (const chunk of [
      { delta: { role: "assistant", content: "The isolated mailbox notification was received." }, finish_reason: null },
      { delta: {}, finish_reason: "stop" },
    ]) response.write(`data: ${JSON.stringify({
      id: `fixture-${model_calls}`, object: "chat.completion.chunk", created: 1700000000,
      model: "wake", choices: [{ index: 0, ...chunk }],
    })}\n\n`)
    response.end("data: [DONE]\n\n")
  } else {
    response.writeHead(200, { "Content-Type": "application/json" })
    response.end(JSON.stringify({
      id: `fixture-${model_calls}`, object: "chat.completion", created: 1700000000, model: "wake",
      choices: [{ index: 0, message: { role: "assistant", content: "Isolated mailbox" }, finish_reason: "stop" }],
      usage: { prompt_tokens: 1, completion_tokens: 1, total_tokens: 2 },
    }))
  }
})
const until = async (check, description) => {
  const deadline = Date.now() + 30000
  while (Date.now() < deadline) {
    const result = await check()
    if (result) return result
    if (child?.exitCode !== null && child?.exitCode !== undefined) throw new Error(`native process exited: ${output}`)
    await new Promise((resolve) => setTimeout(resolve, 50))
  }
  throw new Error(`timed out waiting for ${description}: ${output}`)
}
try {
  await new Promise((resolve) => model.listen(0, "127.0.0.1", resolve))
  const port_reservation = create_tcp_server()
  await new Promise((resolve) => port_reservation.listen(0, "127.0.0.1", resolve))
  const native_port = port_reservation.address().port
  await new Promise((resolve) => port_reservation.close(resolve))
  await mkdir(join(root, "config"))
  // Optional installed public dependencies avoid a first-run npm fetch. Never
  // copy configuration, authentication, history, or any other profile material.
  if (dependency_seed) {
    const destination = join(root, "config", "opencode")
    await mkdir(destination)
    for (const name of ["node_modules", "package.json", "package-lock.json"]) {
      await cp(join(dependency_seed, name), join(destination, name), { recursive: true })
    }
  }
  const environment = Object.fromEntries(["PATH", "SystemRoot", "WINDIR", "TEMP", "TMP", "COMSPEC", "PATHEXT"]
    .filter((key) => process.env[key]).map((key) => [key, process.env[key]]))
  Object.assign(environment, {
    HOME: root, USERPROFILE: root, APPDATA: root, LOCALAPPDATA: root,
    XDG_CONFIG_HOME: join(root, "config"), XDG_DATA_HOME: root, XDG_CACHE_HOME: root,
    OPENCODE_CONFIG_CONTENT: JSON.stringify({
      model: "fixture/wake", small_model: "fixture/wake", permission: "deny",
      provider: { fixture: {
        npm: "@ai-sdk/openai-compatible", name: "Isolated fixture",
        options: { baseURL: `http://127.0.0.1:${model.address().port}/v1`, apiKey: "local-fixture" },
        models: { wake: {
          name: "Wake fixture", limit: { context: 32000, output: 1024 }, variants: { "deliberate reasoning": {} },
        } },
      } },
      plugin: [[pathToFileURL(resolve(plugin_path)).href, { session_wake: { registration_file: registration } }]],
    }),
    OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED: "1",
    OPENCODE_DISABLE_MODELS_FETCH: "1", OPENCODE_DISABLE_AUTOUPDATE: "1",
    OPENCODE_DISABLE_DEFAULT_PLUGINS: "1",
    OPENCODE_DISABLE_PROJECT_CONFIG: "1",
    OPENCODE_SERVER_PASSWORD: server_password,
  })
  child = spawn(executable, ["serve", "--hostname", "127.0.0.1", "--port", String(native_port), "--print-logs"], {
    cwd: root, env: environment, windowsHide: true, stdio: ["ignore", "pipe", "pipe"],
  })
  child_exit = new Promise((resolve, reject) => { child.once("exit", resolve); child.once("error", reject) })
  for (const stream of [child.stdout, child.stderr]) stream.on("data", (chunk) => { output = (output + chunk).slice(-12000) })
  const base = await until(() => /http:\/\/127\.0\.0\.1:\d+/.exec(output)?.[0], "native server")
  const api = async (path, body) => {
    const response = await fetch(`${base}${path}`, {
      method: body === undefined ? "GET" : "POST",
      headers: {
        "Content-Type": "application/json", "x-opencode-directory": root,
        Authorization: `Basic ${Buffer.from(`opencode:${server_password}`).toString("base64")}`,
      },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
      signal: AbortSignal.timeout(30000),
    })
    assert(response.ok, `native API ${path}: ${response.status} ${await response.clone().text()}`)
    return response.status === 204 ? undefined : response.json()
  }
  const session = await api("/session", {})
  await api(`/session/${session.id}/message`, {
    agent: "plan", model: { providerID: "fixture", modelID: "wake" }, variant: "deliberate reasoning",
    parts: [{ type: "text", text: "Reply with a short sentence; do not use tools." }],
  })
  const binding = await until(async () => {
    try { return JSON.parse(await readFile(registration, "utf8")) } catch { return undefined }
  }, "native plugin bridge registration")
  const bridge = (operation) => new Promise((resolve, reject) => {
    const socket = connect(binding.port, "127.0.0.1")
    let bytes = ""
    socket.once("error", reject)
    socket.once("connect", () => socket.write(`${JSON.stringify({
      ...binding, operation, provider_session_id: session.id, transition_nonce: "d".repeat(32),
      ...(operation === "notify" ? { notification_id: "native_mail", instruction: "Read the new mailbox notification." } : {}),
    })}\n`))
    socket.on("data", (chunk) => { bytes += chunk })
    socket.once("end", () => { try { resolve(JSON.parse(bytes)) } catch (error) { reject(error) } })
  })
  await until(async () => (await bridge("inspect")).available, "native turn completion")
  const accepted = await bridge("notify")
  assert.equal(accepted.disposition, "accepted")
  assert.deepEqual(await bridge("notify"), accepted)
  const messages = await until(async () => {
    const items = await api(`/session/${session.id}/message`)
    return items.filter((item) => item.info.role === "user").length === 2 &&
      items.filter((item) => item.info.role === "assistant" && item.info.finish === "stop").length === 2 && items
  }, "second native turn in original session")
  const users = messages.filter((item) => item.info.role === "user")
  assert(users.every((item) => item.info.sessionID === session.id && item.info.agent === "plan" &&
    item.info.model.providerID === "fixture" && item.info.model.modelID === "wake" &&
    item.info.model.variant === "deliberate reasoning"))
  assert.equal(users[1].parts.find((part) => part.type === "text").text, "Read the new mailbox notification.")
  assert(model_calls >= 2)
  console.log("Native OpenCode: same live session woke once, Plan/model/variant retained, second response observed through native API")
} catch (error) {
  console.error(output)
  throw error
} finally {
  if (child?.exitCode === null) {
    child.kill()
    await child_exit
  }
  model.closeAllConnections()
  await new Promise((resolve) => model.close(resolve))
  await rm(root, { recursive: true, force: true })
}
