# OpenCode for Logonomic

This standalone package supplies OpenCode launcher recognition, interactive
configuration and tools, delegated ACP implementation, lifecycle delivery, and
interactive resume through the public Logonomic CLI add-on SDK. Its package identity is `logonomic.cli.opencode`.

OpenCode has no singular global account, authentication, quota, or usage model in
this integration. Those capabilities are not advertised. Read-only structured
review is unavailable; structured inspection belongs to delegated implementation.

A delegated task keeps the launcher's permission posture: OpenCode's own
`permission` configuration, which the package passes through unchanged, decides
which tools run and which ask. When OpenCode asks, the package answers with the
request's own `once` option only for the task's own Task tools
(`logonomic-task-<task>-<server>_<action>`, announced by OpenCode's `tool_call`
with that title and requested with kind `other`), and with `reject` for anything else, which
nobody can approve in a headless run; it never chooses `always`. OpenCode fails
a rejected tool call and continues the turn. A task that needs other tools
therefore needs a configuration that allows them.

The `opencode-ai` lifecycle contract requires a verified version at least 1.18.11;
its delegated ACP contract requires at least 1.18.25. The `@opencode/cli` 2.x
integration uses the native TUI plugin and synthetic inbox contracts, qualified
against 2.1.4. Newer versions remain eligible and their actual native methods and
protocol messages are validated. The selected stock launcher chain is
verified before delegation. An interactive launch reports its lifecycle only when
the host's complete invocation, a direct launcher or `powershell -NoLogo -Command`
running the recognized script, starts a verified OpenCode; otherwise the launch is
prepared without lifecycle. User configuration is merged, and package writes use
the exact host-acknowledged profile storage allocation.

For a verified OpenCode 2 TUI invocation, preparation adds the native
`--standalone` option when absent. Its private server keeps model tools in the
recipient launch's Logonomic environment, including its mailbox authority.
Direct launches and the verified PowerShell invocation retain their original
arguments and permission settings. An explicit remote server, management command,
unknown interface or unverifiable package-runner launch remains an ordinary launch
without wake authority. The combined native plugin directory is added to server
configuration; OpenCode discovers its TUI entrypoint alongside the user's existing
`cli.json` plugins, without replacing that list or modifying the configuration file.

| Native contract | Integrated behavior |
| --- | --- |
| `opencode-ai` hooks and SDK | Lifecycle, native questions and permissions, context telemetry, changed-file tools, resume, delegated ACP and idle wake |
| `@opencode/cli` 2.x TUI and inbox | Selected root session, execution outcomes, native forms and permissions, resume and idle wake; delegated ACP, context telemetry and changed-file tools are not advertised through this contract |

A stored session resumes through the same verified launcher with
`--session <id>` added. The hooks plugin confirms the native store identity;
the OpenCode 2 plugin observes the TUI's selected root session through its public
reactive API. This lets Logonomic confirm the resume before the first turn. A launcher
that cannot report lifecycle, or whose options could select another session,
directory or interface or leave out plugins, cannot resume.

The optional `session-wake@1` capability wakes an idle existing interactive
session through its managed plugin's own native SDK client. A private, launch-owned
loopback bridge binds delivery to that live plugin instance, native session, and
launch correlation. The plugin requires a completed normal turn and preserves
its actual agent, model and variant. The hooks SDK submits the observed selection
explicitly; OpenCode 2 synthetic input retains the session's persisted selection.
Busy sessions and pending native permissions
or questions defer delivery; interrupted sessions remain unavailable. It never
submits terminal input or edits the TUI draft. A retained notification receipt
prevents a repeated accepted or uncertain delivery from creating another turn;
acceptance does not mean the mailbox has been read or acknowledged.

Receipts are retained for the lifetime of the managed plugin instance and shared
across its native sessions. Once 256 receipts have accumulated, a new otherwise
eligible notification returns `unavailable` with `provider_receipt_capacity`;
the mailbox message remains stored. Repeated notification IDs for the same session
still return their retained result. Deleting a session does not free receipts;
restarting OpenCode creates a fresh plugin instance and receipt table.

Switching to another root conversation revokes the previous native wake binding.
Returning requires inspection under the new Logonomic transition; retired
transition nonces cannot submit notifications. Temporarily showing the home,
child-session or plugin page suppresses wake readiness without inventing a new
root conversation. The bridge retains at most 256 retired correlations before
refusing fresh bindings with `provider_session_capacity`.

OpenCode 2 uses `session.synthetic` with queue delivery and execution enabled.
A concurrent user turn can therefore retain the notification in the native inbox
without steering or interrupting that turn. Readiness is checked again against
native session, execution, permission, form and pending-input APIs before admission.
No version ceiling is imposed on compatible launchers.

## Building and local qualification

Interactive native question events retain `question.asked`, `question.replied`,
and `question.rejected` and the native question identity. Root questions retain
their original root turn; child questions retain their native child and request
identities without borrowing a root turn. Replies carry a boolean
`answer_nonempty` in the projected provider event:
true means at least one returned answer string contains non-whitespace text.
Question and answer prose are omitted. A reply is distinct from a rejection;
disposal or session termination clears pending correlation without fabricating
either event. A native qualification gate must require a correlated reply with
`answer_nonempty: true`, not merely disappearance of attention.

The OpenCode 2 TUI restores pending permissions and forms from the selected
session's native data, including after hydration and reselection. Restored
requests have no invented execution turn. A request removed from a refreshed
pending list resolves attention without claiming a native reply or cancellation;
only an observed native form reply supplies `answer_nonempty`. A native reply
keeps its request identity and answer evidence when the request outlives its
execution; after a newer execution starts, it omits the superseded turn.

The optional `attention-kind@1` companion distinguishes confirmed native
questions from permissions and inferred conversational hints. The OpenCode 1
plugin scans complete native assistant text parts while retaining bounded
Markdown context and the final prose paragraph's first and last 128 UTF-8 bytes.
It supplies those explicitly scoped edges only on normal root completion
(`finish: stop`, completed timestamp, no error), before the first idle event
clears turn identity. Long explanations retain their closing question; invalid,
quoted or code evidence is omitted. Part boundaries preserve fence, quote and
paragraph context. These edges are private classifier input, never a claimed
complete message or copied into lifecycle observation fields.
The existing SDK local heuristic classifies hints; genuine subsequent user input
can resolve only the host-selected conversational hint. No transcript hydration
or external classification is used. Pending question, reply, and hint tracking
are bounded; overflow uses the existing lifecycle-degradation path.

Delegated ACP question disabling and base helper-ingress contracts are unchanged.
Provider fixtures establish protocol behavior, not paid native or UI qualification.
The wake fixture exercises the real authenticated bridge and packaged C++ adapter,
including repeat delivery, busy/attention deferral and uncertain acceptance.
`node tests/opencode_wake_native_tests.mjs <opencode-executable> <managed-plugin> [public-dependency-seed]`
is an opt-in native gate: it creates an isolated profile and loopback fake model,
then observes two turns in the same native session. It does not prove TUI rendering
or draft preservation visually and never uses the user's provider credentials.
The optional seed directory supplies only `node_modules`, `package.json` and
`package-lock.json` to avoid a first-run dependency download; no native profile
configuration, authentication or history is copied.

`node tests/opencode2_wake_native_tests.mjs <opencode-executable> <package-directory> <opencode2_native_prepare>`
qualifies the OpenCode 2 route using the packaged adapter's actual launch
preparation. It runs an isolated native TUI and fake model, verifies native queue
admission, duplicate suppression, Plan/model/variant preservation and the
recipient's test mailbox environment, then submits a pre-existing test draft
through normal terminal input to check that wake preserved it exactly. A separate
observer loaded from the existing `cli.json` verifies that user plugins remain
active. This fixture creates no real mailbox messages and uses no user credentials.

The project is C++20 and uses only the standalone SDK, the standard library and
platform APIs. CMake tracks the SDK source on Logonomic `master`; a local SDK
checkout can be supplied with `LOGONOMIC_CLI_ADDON_SDK_SOURCE`. Provider sources
and build outputs remain separate from the desktop app.

Run every compiler-invoking configure or build through `queued-build` with an
explicit numeric slot count matching native parallelism. Windows uses Visual
Studio 2026 x64 with MSVC v145 (`vcvarsall.bat x64 -vcvars_ver=14.51`) and FASTBuild
when available. Set `CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV=OFF`; cache-enabled MSVC
uses Embedded `/Z7` debug information. Use a fresh out-of-source build directory.
The `opencode_package` target assembles and indexes the platform package under
`<build>/package/logonomic.cli.opencode` for a single-configuration generator, or
`<build>/package/<config>/logonomic.cli.opencode` for a multi-configuration generator.

The package recipe proves the Windows/Linux x86-64 or macOS ARM64 compiler target
during configuration. `cmake --install` exports a fresh artifact beneath its install-time
prefix and preserves executable permissions. An existing per-package destination,
including a dangling symlink, is refused. POSIX `DESTDIR` staging is supported;
nonempty `DESTDIR` with a drive-letter prefix is refused. Export is not atomic:
a failure may leave a partial artifact directory. Choose a fresh destination or
manually remove only that identified disposable export before retrying; never
remove an installed generation this way. The application's package ingestion
owns atomic installation and updates.

After the sources are ready, build once for the selected platform and run its
registered CTest entries. `opencode_package_validation` checks indexed bytes and
declarations; `opencode_package_conformance` exercises the SDK protocol contract.
Provider behavior and real helper tests complement those generic checks. Windows
and Linux require separate native qualification; evidence from one does not prove
the other's process paths. Local staging uses `logonomic_cli_addon_stage` and the
desktop's generic package ingestion in isolated profiles.

Local qualification does not activate this package in a user's live profile.
Publication, signed availability, hosted rehearsal, migration/rollback gates and
the maintained observation window are separate release checkpoints. The native
OpenCode implementation remains installed until its authorized deletion landing.

## Attribution

The OpenCode icons derive from Lobe Icons and retain the packaged MIT notice in
`package/notices/lobe_icons_mit.txt`; per-resource source and normalization details
are in `package/provenance/lobe_icons.json`. OpenCode names and marks identify the
independent upstream software; they do not imply endorsement.
