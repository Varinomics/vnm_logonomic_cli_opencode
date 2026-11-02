# P21 · OpenCode assets and lifecycle

The provider changes are on `work/linux-review-20261102-p21-provider-opencode`, based on `09e01a3`. The Windows process code-page check remains open. [Draft PR #9](https://github.com/Varinomics/vnm_logonomic_cli_opencode/pull/9)

| Finding | Change and evidence |
|---|---|
| 5 · Managed assets | Importing the 1.x entry point from a fresh directory containing only declared assets failed with `ERR_MODULE_NOT_FOUND` for the reply module. Declaring that module made the same import pass. |
| 47 · Wake state | 130 child sessions exhausted the old table and prevented the next root from obtaining wake state. The provider now separates pending input from confirmed root state. Tests exercise child churn, old events racing deletion, failed lookups, a question preceding root admission, revocation during a notification and receipt replay. If all 128 pending slots contain unclassified open requests and another ask is lost, the plugin declines wakes for newly encountered sessions until restart. |
| 48 · Returning to a root | The old classifier emitted START when a fresh turn returned from B to retired root A. Logonomic rejects START on a retired ID. The 1.x projection now identifies the first working status confirmed by a newer root input, and the classifier emits RESUME when switching roots on that evidence. Delayed statuses and idle events cannot re-admit A. |
| 49 · Windows package root | The source decoded a narrow environment value as UTF-8. Launch and resume now read the UTF-16 Windows process environment. The Linux fixture matched the exact plugin path under `generation-é-東`. Run `opencode_launcher_tests` under a non-UTF-8 Windows ANSI code page to validate the Windows implementation. |
| 140 · Inline configuration | A valid setting containing `{{file}}` failed host patch validation. User keys and values now use equivalent JSON escaping for adjacent opening braces, leaving the provider's plugin placeholder available to the host. Tests expand the managed asset and compare the decoded launch, resume and delegated settings. |
| 168 · Registration lifetime | An isolated bridge process left a 150-byte registration on clean process exit without disposal and after SIGKILL. Clean exit now removes the launch's file without plugin disposal. A hard kill leaves it in host-owned generation scratch until retirement. In an SDK host fixture with two real packaged adapters sharing one profile, retiring the killed bridge's generation removed its file and preserved the live peer. Source inspection confirmed desktop generation retirement owns the same cleanup. |
| 139 · ACP reply persistence | The delegated inspector persisted the same growing reply before its next notification could consume that state. It now encodes the reply when returning a continuation. Tests cover message-ID changes, a split record, and exact output at close. |
| 167 · Unknown context capacity | OpenCode 1.18.33 returned `context: 0` for a configured model with no specified limit. The provider now remembers an explicit zero for the current model and continues to omit capacity from telemetry. Catalogue failures and missing models remain retryable. |

## Bounded measurements

| Workload | Original | Patched |
|---|---:|---:|
| Result encodes for 128 ACP chunks of 256 bytes in a single 56,892-byte read, followed by close | 129 | 1 |
| Reply bytes consumed by those encodes | 2,146,304 | 32,768 |
| Catalogue requests for 12 distinct root usage events on a model reporting zero | 12 | 1 |
| Median time to enqueue and drain those 12 events through the compiled relay | 269 ms | 94 ms |
| Positive-capacity control, identical workload | 1 request, 87 ms | 1 request, 97 ms |

The ACP fixture exercised the compiled inspector, counted result encodes at the SDK boundary, and checked the full 32 KiB returned reply. The catalogue fixture used the real SDK client inside an isolated OpenCode 1.18.33 server, a warmed 3-provider, 89-model catalogue, five fresh plugin instances per condition, synthetic root updates and a capture executable in place of the final host. All 12 emitted token updates matched in every trial. The timings describe these fixture workloads.

## Linux validation

The standalone provider consumed the add-on SDK from Logonomic `b9e44f6fe131624d48b0338c23295dae67d526bd`. The 137 SDK source, header, CMake and tool files matched their Git blobs. The public Linux SDK `2026.10.23.1` matched both release and pinned SHA-256 checksums and passed `sdk-preflight`. The Release build used GCC 13.3, CMake 3.31.10, SQLite 3.45.1, persistent ccache, one queued configure slot and at most four queued build slots. The isolated membership rebuild used the same cache and a four-slot lease.

All 12 focused CTests passed. The normal run included `opencode_managed_plugin_import`, `opencode_package_validation` and the `opencode_{preparation,launcher,lifecycle,acp,context_plugin,plugin,wake_plugin,tui_plugin,plugin_disposal}_tests` targets. `opencode_package_membership` passed in its separately queued nested build. The wake and TUI fixtures require loopback socket access.

The isolated native OpenCode 1.18.33 server exercised same-session wake, A/B/A prompts, nonce revocation, receipt replay and preservation of Plan, model and variant through the declared asset layout. The empty-profile rerun timed out during bootstrap. The documented seed containing the public 1.18.33 plugin dependencies let the same fixture complete. Its model endpoint ran on loopback. The separate OpenCode 2.1.4 TUI fixture used the packaged adapter's actual launch patch, a temporary profile and a fake model. Busy deferral, duplicate suppression, mailbox environment, the user's configured TUI plugin, Plan, model, variant and the pre-existing unsent draft all passed.

## Remaining check

On Windows with an ordinary non-UTF-8 system ANSI code page, run `opencode_launcher_tests` and confirm both 2.x launch and resume emit the exact plugin directory under `generation-é-東`. The Linux tests exercised the same path comparison. The 1.x native run exercised session changes by sending prompts through the server API. The 1.x TUI picker alone was outside that check.
