# Native model observations

The optional `model-state@1` capability reports native serving-model facts through
the existing helper classification path. The model text is the native provider
namespace followed by `/` and the native model identity. Launcher selections,
ACP configuration acknowledgments and reasoning variants are not model evidence.

OpenCode 1 samples non-summary assistant `message.updated` records. The native
`providerID`, `modelID` and message identity are captured synchronously, and
each user input captures its own activation nonce, before asynchronous lookups
or delivery. A record confirms its model only when the native `step-start` part
of that same message follows, under the activation that admitted the record or
the one its pending input's nonce opens. Task wrappers and shell records
dispatch no model step, so they never confirm a model, and a request's selected
model is never evidence. The plugin's ordered lifecycle projection then
attributes confirmed models: a root activation begins when that projection
selects an input's root, under the input's nonce. OpenCode starts that input's
first step after the selecting status, so its fact follows the start carrier; a
later input of the same root keeps the activation and its last confirmed fact.
A child fact requires a child run the lifecycle admitted under the active root,
at any depth it admits. An unknown session's kind comes from native session
facts or lookup, never from prompt arrival. Child run start and stop close the
observation window; late stopped-run updates or steps cannot seed the next child
run. No child inherits the root model. A step awaiting projection keeps its
arrival position among lifecycle events, and a repeated step of the same message
shares it until the next lifecycle event. A fact still awaiting delivery is
replaced by the same actor's newer observation. Model steps and facts never
occupy lifecycle admission or delivery capacity.

OpenCode 2 samples the selected root's native `session.step.started` event and
its dispatched `Model.Ref`. The adopted source is `SessionEvent.Step.Started` and
`publish-llm-event.ts` at upstream revision
`68bc26b7da1990ce10ffecfbc1e09eb13aa5d824` (`@opencode/cli 2.1.4`). Compaction and
title-generation event families are separate. The current integration does not
admit child execution spans, so model telemetry does not introduce child
lifecycle events or additional Session Log rows. This generation's child badge
path remains unavailable until an existing lifecycle carrier can admit it. A fact
still awaiting delivery is replaced by the newer fact of the same activation until
a lifecycle event or its delivery closes it, and model facts never occupy
lifecycle delivery capacity.

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
