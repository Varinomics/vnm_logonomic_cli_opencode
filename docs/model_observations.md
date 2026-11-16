# Native model observations

The optional `model-state@1` capability reports native serving-model facts through
the existing helper classification path. The model text is the native provider
namespace followed by `/` and the native model identity. Launcher selections,
ACP configuration acknowledgments and reasoning variants are not model evidence.

OpenCode 1 samples non-summary assistant `message.updated` events. The native
`providerID` and `modelID` are captured synchronously with the root activation
nonce, before asynchronous lookups or delivery. A root fact can accompany the
existing native start carrier. Child facts require the child's own observed busy
or retry boundary and native ancestry to that root. Idle, error and deletion
close the source observation window; late stopped-run updates cannot seed the
next child run. Inactive ancestry is evicted within the existing bounded child
capacity. No child inherits the root model.

OpenCode 2 samples the selected root's native `session.step.started` event and
its dispatched `Model.Ref`. The adopted source is `SessionEvent.Step.Started` and
`publish-llm-event.ts` at upstream revision
`68bc26b7da1990ce10ffecfbc1e09eb13aa5d824` (`@opencode/cli 2.1.4`). Compaction and
title-generation event families are separate. The current integration does not
admit child execution spans, so model telemetry does not introduce child
lifecycle events or additional Session Log rows. This generation's child badge
path remains unavailable until an existing lifecycle carrier can admit it.

These are event-bound observations. The provider does not promise an update for
a native model change until its next supported event. OpenCode ACP's selected
model/configuration responses do not establish the model serving an execution;
that structured badge path remains unconfirmed.

## Native producer gate

The opt-in gate uses an isolated profile, the actual OpenCode executable and
managed plugin, the compiled helper, and a fixture transport that captures helper
events. Its scripted model endpoint runs only on loopback and requires no user
credentials. Supply an optional directory containing only public installed
plugin dependencies, or `-` for a clean dependency bootstrap:

```sh
node tests/opencode_wake_native_tests.mjs <opencode-executable> <managed-plugin> <dependency-seed-or-dash> <opencode_helper> <opencode_fake_vnmctl>
```

With the helper arguments, the gate checks distinct native root and child
models, a root model change, and fresh A-to-B-to-A activation nonces in addition
to the existing wake contract. It verifies the native producer/helper path;
package conformance, host admission, timeline rendering and owner visual
acceptance retain their separate gates.
