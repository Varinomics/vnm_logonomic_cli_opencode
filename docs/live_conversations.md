# Native conversation changes

The managed OpenCode 2 TUI integration observes the selected root through the
native reactive `ui.router.current()` and `data.session.get()` APIs. Selecting a
saved conversation is visible before another prompt. `/new` first shows the home
page; the new conversation is reported when OpenCode creates its native identity.
Opening or cancelling a picker, viewing a child, and returning from a hidden page
do not create another conversation. Child activity stays attributed to its root.

Each actual root selection captures an activation nonce before helper delivery.
Returning A → B → A creates a new activation of A. A helper retry retains its
original evidence, delivery key and transition nonce. Later readiness and default
directory snapshots keep that activation and advance a source sequence; they
cannot reopen a departed root. The host remains responsible for epochs and the
departed conversation's history row.

The `opencode-ai` generation 1 hooks do not expose the selected idle TUI root.
They confirm an in-process switch after fresh user input is acknowledged by the
native SDK. A host-prepared `--session` launch can confirm its resumed root before
the first turn. Neither path guesses identity from slash-command text, terminal
output or a child session. Generation 1 does not claim immediate idle-picker
selection or a confirmed native default directory.

## Saved conversation inspection

New lifecycle starts retain the exact native session ID and prepared database
source in the opaque resume record. The source records the native generation and
absolute database path resolved from the verified launcher and its effective
storage environment. Inspection requires that retained source to match the
currently resolved launcher/account/profile context. It never searches other
profiles for the same ID. Older ID-only records retain `interactive-resume@2`
launch compatibility, while inspection reports their unverified storage scope as
unknown.

`interactive-resume@3` reports availability; `@4` also reports saved-content state.
Both bind the full inspection request digest and read a SQLite transaction in
read-only mode. The @3 result omits the @4 extension, and all three capability
declarations keep the same environment requirements.

A missing exact root in a readable supported store is absent. A missing,
unreadable or unsupported store is unknown. Empty content requires complete
understood main-conversation evidence with no queued input, child-only history,
fork/revert dependency or detached parts. Known native user text or files, and
understood legacy user/assistant text, establish nonempty content. Partial,
unsupported or ambiguous evidence stays unknown. Inspection never deletes native
rows or files; a fresh inspection observes later committed writes. Explicit
preparation repeats the exact availability check when original storage is known.

## Native default directory

The optional `execution-context@1` observation comes from the selected OpenCode 2
root's current `Session.Info.location`. Accepted native `session_move` changes
can update that default without changing the session ID. Paths retain their exact
bytes and subdirectory; launch cwd, transcript metadata, Git roots, child locations
and per-command overrides do not supply this observation. Missing native evidence
is unknown; a native remote workspace is unavailable as a local directory. The
activation nonce and source sequence fence delayed observations, including an
earlier activation of the same ID.

This reports live execution context separately from immutable launch provenance.
It does not change the shared host's cross-worktree ownership rules, implement a
resume-location chooser, or prevent a native picker from selecting a conversation
already live in another terminal. The public TUI plugin contract observes an
accepted selection; it has no pre-selection ownership veto.

## Qualification

The maintained contract tests cover SQLite snapshots and late writes, exact
storage scope, old-host negotiation, helper retries, idle selection, A → B → A,
child filtering and source-attributed directory changes. The native gate is:

```sh
node tests/opencode2_wake_native_tests.mjs <opencode-executable> <package-directory> <opencode2_native_prepare> <opencode_fake_vnmctl>
```

It uses an isolated profile, a loopback scripted model, actual packaged preparation
and helper delivery. Normal terminal input performs `/new`, `/sessions` selection
and picker cancellation; the public native client performs a same-ID directory
move. It uses no user credentials or external model endpoint. Qualification on one
platform does not establish qualification on Windows or macOS.

Native schema and API references:

- [OpenCode TUI plugin context](https://github.com/anomalyco/opencode/blob/da8c0211f7c00afbf1dd96e629801fe84d205541/packages/plugin/src/tui/context.ts)
- [Native session schema](https://github.com/anomalyco/opencode/blob/da8c0211f7c00afbf1dd96e629801fe84d205541/packages/schema/src/session.ts)
- [Native session movement](https://github.com/anomalyco/opencode/blob/da8c0211f7c00afbf1dd96e629801fe84d205541/packages/core/src/session/move.ts)
- [Native SQLite layout](https://github.com/anomalyco/opencode/blob/da8c0211f7c00afbf1dd96e629801fe84d205541/packages/core/src/session/sql.ts)
- [Native message schemas](https://github.com/anomalyco/opencode/blob/da8c0211f7c00afbf1dd96e629801fe84d205541/packages/schema/src/session-message.ts)
