#include "opencode_acp.h"
#include "opencode_launcher.h"
#include "opencode_lifecycle.h"
#include "opencode_preparation.h"

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/legacy_state_import.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

void import_history(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_import_record_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (request->kind != addon::k_legacy_resume_record_kind) {
        context.fail(addon::Result_status::UNAVAILABLE, "unknown_record_kind");
        return;
    }
    const auto record = addon::decode_legacy_resume_record(request->record, &failure);
    if (!record) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    addon::Legacy_import_result result;
    if (record->provider_id == "opencode") {
        auto converted = addon::Json_value::object();
        converted.set("provider_session_id", addon::Json_value(record->provider_session_id));
        result = *addon::Legacy_import_result::from_conversion(addon::encode_legacy_resume_conversion(converted));
    }
    context.succeed(addon::encode_import_record_result(result));
}

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
            const auto* value = request.payload.find("profile_storage");
            std::string detail;
            const auto storage = value ? addon::decode_profile_storage_context(*value, &detail) : std::nullopt;
            if (!storage) {
                context.fail(addon::Result_status::FAILED, "profile_storage_mismatch");
                return;
            }
            auto result = addon::Json_value::object();
            result.set("profile_storage", addon::profile_storage_member(*storage));
            context.succeed(std::move(result));
        }
        else
        if (request.capability.capability_id == "legacy-state-import" && request.operation == "import_record") {
            import_history(context);
        }
        else
        if (request.capability.capability_id == "lifecycle-events" ||
            request.capability.capability_id == "helper-ingress" ||
            request.capability.capability_id == addon::k_attention_kind_capability_id)
        {
            opencode::handle_lifecycle(context);
        }
        else
        if (request.capability.capability_id == "structured-session" && request.operation == "inspect") {
            addon::Contract_failure failure;
            const auto inspection = addon::decode_structured_inspect_request(request.payload, &failure, 2);
            if (!inspection) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            if (inspection->purpose != addon::Structured_purpose::IMPLEMENTATION) {
                context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
                return;
            }
            auto result = addon::encode_structured_inspect_result(opencode::inspect_acp(inspection->inspection), 2);
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
    catch (const std::exception&) {
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
        {"presentation", 1}, {"launcher-discovery", 1}, {"profile-storage", 1},
        {"launch-preparation", 3}, {"managed-assets", 1}, {"structured-session", 2},
        {"delegated-session", 2}, {"lifecycle-events", 1}, {"helper-ingress", 2},
        {"legacy-state-import", 1}, {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
        {"interactive-resume", 2},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
