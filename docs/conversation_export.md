# Conversation extraction

The optional `conversation-export@2` capability writes one retained OpenCode
conversation as UTF-8 Markdown, plain text or the common conversation format.
The SDK owns the file descriptor, renderer and output lifetime. This provider
owns source selection, native ordering and content interpretation.

Preparation records the verified native generation and absolute database path
in `VNM_OPENCODE_EXPORT_SOURCE`. It resolves the source from the environment the
native launch receives, including supported script assignments. An inherited
locator is always cleared before this launch's locator is considered. The
version 2 declaration requires this name to survive in the finalized source
environment; an absent locator leaves source extraction unavailable without
preventing ordinary launch. Export
uses that frozen source and the exact native session ID; it does not consult
today's launcher, current account, home directory or installed CLI. Remote,
in-memory and unresolved sources do not have a retained local source.

For the verified stock installation, the default is `opencode.db` below
`XDG_DATA_HOME/opencode`, or the native home directory's `.local/share/opencode`
when `XDG_DATA_HOME` is empty. An explicit absolute `OPENCODE_DB` is preserved;
a relative override is resolved under that data directory. Empty `OPENCODE_DB`
selects the default for legacy OpenCode and leaves OpenCode 2 without an
exportable database. Windows drive-relative and root-relative overrides cannot
be frozen as absolute source facts. No native configuration or storage selector
is changed to make extraction available.

Hosts adopting only `conversation-export@1` leave this optional capability
unavailable. Other providers' version 1 source-capture behavior is unchanged.

The reader opens SQLite read-only and uses one deferred transaction for the
session and all message/part queries. Committed WAL content belongs to that
snapshot. Cancellation interrupts reads and rolls back the transaction.
Extraction does not run OpenCode, migrate the database, checkpoint the WAL or
modify the source records. SQLite's ordinary read marks in shared memory are
part of a live WAL reader.

Legacy OpenCode uses `session`, `message` and `part`. Messages follow
`(time_created, id)` and parts follow `id`. A legacy source is unavailable when
`session_v2` contains the same native ID, because that identity has also been
imported into another generation. OpenCode 2 reads `session_v2` and projected
`session_message` rows in `seq` order. Generation selection comes from the
verified native launch, rather than the presence of a table.

Both generations copy a fork's prefix into the child. Export never follows a
parent or adds subagent histories. Staged reverts use the native cleanup
prefix: legacy supports a message and optional part boundary; OpenCode 2 keeps
rows before the message boundary. Missing or unsupported native content fails
explicitly instead of publishing a partial document.

Full extraction preserves retained user and assistant text, reasoning, tool
calls and results, failures, compaction records and attachment references.
Unfinished assistant, tool, shell and compaction content remains present with
explicit state markers. Tool result text remains as retained by OpenCode;
partial or truncated native shell output stays marked. Referenced attachment files are not opened, and
embedded binary data is represented by an attachment marker. Compact extraction
uses provider-declared block visibility without changing the native source.

Older Logonomic executions that never captured an export source remain
unavailable. A real Logonomic resume confirms the same native conversation
identity and captures its new launch context under a fresh durable execution
ID; it does not backfill the older execution's record.

## Native contracts

The legacy reader follows OpenCode `1.18.33` at
`51ef4be1d3c122f18fefb510dca8d778571f4f18`:

- [Storage schema](https://github.com/anomalyco/opencode/blob/51ef4be1d3c122f18fefb510dca8d778571f4f18/packages/core/src/session/sql.ts)
- [Message order and hydration](https://github.com/anomalyco/opencode/blob/51ef4be1d3c122f18fefb510dca8d778571f4f18/packages/opencode/src/session/message-v2.ts)
- [Content types](https://github.com/anomalyco/opencode/blob/51ef4be1d3c122f18fefb510dca8d778571f4f18/packages/schema/src/v1/session.ts)
- [Revert cleanup](https://github.com/anomalyco/opencode/blob/51ef4be1d3c122f18fefb510dca8d778571f4f18/packages/opencode/src/session/revert.ts)

The OpenCode 2 reader follows the adopted `@opencode/cli 2.1.4` contract at
`68bc26b7da1990ce10ffecfbc1e09eb13aa5d824`:

- [Projected storage](https://github.com/anomalyco/opencode/blob/68bc26b7da1990ce10ffecfbc1e09eb13aa5d824/packages/core/src/session/sql.ts)
- [Native message schema](https://github.com/anomalyco/opencode/blob/68bc26b7da1990ce10ffecfbc1e09eb13aa5d824/packages/schema/src/session-message.ts)
- [Fork and revert projection](https://github.com/anomalyco/opencode/blob/68bc26b7da1990ce10ffecfbc1e09eb13aa5d824/packages/core/src/session/projector.ts)

These revisions identify the native oracle, not runtime version pins.
