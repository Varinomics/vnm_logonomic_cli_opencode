#include "opencode_acp.h"
#include "opencode_conversation.h"
#include "opencode_launcher.h"
#include "opencode_launcher_approval.h"
#include "opencode_lifecycle.h"
#include "opencode_preparation.h"
#include "opencode_wake.h"

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/session_wake.h"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

void discover(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_discover_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto launcher = opencode::discover_launcher(*request);
    context.succeed(addon::encode_candidates(launcher ? std::vector<addon::Launcher_candidate>{*launcher} : std::vector<addon::Launcher_candidate>{}));
}

void recognize(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_recognize_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    addon::Launcher_recognition recognition;
    recognition.recognized = opencode::recognize_launcher(request->launcher.script_body).has_value();
    if (recognition.recognized) {
        recognition.display_name = "OpenCode";
    }
    context.succeed(addon::encode_recognition(recognition));
}

void invoke(addon::Invocation_context& context)
{
    const auto& request = context.request();
    try {
        if (request.capability.capability_id == addon::k_conversation_export_capability_id) {
            addon::handle_conversation_export(context, opencode::export_conversation);
        }
        else
        if (request.capability.capability_id == addon::k_launcher_approval_capability_id) {
            addon::handle_launcher_approval(context, opencode::launcher_approval_rules());
        }
        else
        if (request.capability.capability_id == "launcher-discovery") {
            if (request.operation == addon::k_discover_operation) {
                discover(context);
            }
            else
            if (request.operation == addon::k_recognize_operation) {
                recognize(context);
            }
            else {
                context.reject_unknown_operation();
            }
        }
        else
        if (request.capability.capability_id == "profile-storage" && request.operation == "acknowledge") {
            if (request.payload.as_object().size() != 1) {
                context.fail(addon::Result_status::FAILED, "frame_violation");
                return;
            }
            addon::acknowledge_profile_storage(context);
        }
        else
        if (request.capability.capability_id == "lifecycle-events" ||
            request.capability.capability_id == "helper-ingress" ||
            request.capability.capability_id == addon::k_attention_kind_capability_id)
        {
            opencode::handle_lifecycle(context);
        }
        else
        if (request.capability.capability_id == "session-wake") {
            opencode::handle_wake(context);
        }
        else
        if (request.capability.capability_id == "structured-session" && request.operation == "inspect") {
            addon::Contract_failure failure;
            const auto inspection = addon::decode_structured_inspect_request(request.payload, &failure);
            if (!inspection) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            if (inspection->purpose != addon::Structured_purpose::IMPLEMENTATION) {
                context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
                return;
            }
            auto result = addon::encode_structured_inspect_result(opencode::inspect_acp(inspection->inspection));
            if (!addon::validate_session_exchange_payload(result, &failure)) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            context.succeed(std::move(result));
        }
        else {
            opencode::handle_preparation(context);
        }
    }
    catch (const std::exception& error) {
        const std::string_view reason = error.what();
        if (reason == "OpenCode configuration contains an unterminated comment." ||
            reason == "OpenCode configuration has multiple environment case variants." ||
            reason == "OpenCode configuration must be a JSON object." ||
            reason == "OpenCode instructions must be an array of strings." ||
            reason == "OpenCode plugin configuration must be an array." ||
            reason == "OpenCode plugin entries must be names or name/options pairs." ||
            reason == "OpenCode plugins must be an array." ||
            reason == "OpenCode plugins must be names or package/options objects.")
        {
            context.fail(addon::Result_status::FAILED, "opencode_settings_invalid", "OpenCode settings are invalid.");
            return;
        }
        // Vendor configuration and private filesystem values never become diagnostics.
        context.fail(addon::Result_status::FAILED, "opencode_preparation_failed",
            "OpenCode preparation could not preserve the supplied configuration or owned storage.");
    }
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "opencode-addon";
    options.adapter_version = "1.0.0";
    options.capabilities = {
        {addon::k_conversation_export_capability_id, addon::k_conversation_export_prepared_source_version},
        {addon::k_launcher_approval_capability_id, addon::k_launcher_approval_version},
        {"presentation", 1}, {"launcher-discovery", 1}, {"profile-storage", 1},
        {"launch-preparation", 3}, {"managed-assets", 1}, {"structured-session", 3},
        {addon::k_interactive_initial_prompt_capability_id, addon::k_interactive_initial_prompt_version},
        {"delegated-session", 3}, {"lifecycle-events", 2}, {"helper-ingress", 2},
        {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
        {addon::k_context_state_capability_id, addon::k_context_state_capability_version},
        {addon::k_execution_context_capability_id, addon::k_execution_context_version},
        {"interactive-resume", 2}, {"interactive-resume", 3}, {"interactive-resume", 4},
        {"session-wake", 1},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
