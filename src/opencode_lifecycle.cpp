#include "opencode_lifecycle.h"

#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace opencode {
namespace {

namespace addon = logonomic::cli_addon;

std::optional<std::string> metadata(const addon::Json_value& body, std::string_view member,
    std::size_t limit = addon::limits::k_max_lifecycle_identity_bytes)
{
    const auto* value = body.find(member);
    if (!value || !value->is_string()) {
        return std::nullopt;
    }
    const std::string& text = value->as_string();
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::nullopt;
    }
    const std::string trimmed = text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
    if (trimmed.size() > limit || trimmed.find_first_of(std::string_view("\0\r\n", 3)) != std::string::npos) {
        return std::nullopt;
    }
    return trimmed;
}

std::optional<addon::Classify_event_result> classify(const addon::Classify_event_request& request)
{
    const auto& body = request.event.body;
    const std::string& name = request.event.name;
    addon::Classify_event_result result;
    const auto body_name = metadata(body, "event");
    if (!body_name || *body_name != name) {
        return std::nullopt;
    }
    if (name == "logonomic.delivery_overflow") {
        result.lifecycle_degradation = addon::Lifecycle_degradation{};
        return result;
    }
    if (name == "logonomic.turn_correlation_degraded") {
        const auto session = metadata(body, "session_id");
        const auto cause   = metadata(body, "degradation_type");
        if (!session || !cause || (*cause != "candidate_capacity" && *cause != "conflicting_confirmation")) {
            return std::nullopt;
        }
        addon::Lifecycle_degradation degradation;
        degradation.provider_session_id = *session;
        result.lifecycle_degradation = std::move(degradation);
        return result;
    }
    const bool child_start = name == "logonomic.subagent.started";
    const bool child_stop  = name == "logonomic.subagent.stopped";
    const bool deleted    = name == "session.deleted";
    const bool status     = name == "session.status";
    const bool idle       = name == "session.idle";
    const bool error      = name == "session.error";
    const bool asked      = name == "permission.asked";
    const bool replied    = name == "permission.replied";
    if (!(child_start || child_stop || deleted || status || idle || error || asked || replied)) {
        return result;
    }
    const auto session = metadata(body, "session_id");
    if (!session) {
        return std::nullopt;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = name;
    observation.provider_session_id = *session;
    result.classification = addon::Classification::EVENT;
    if (child_start || child_stop) {
        const auto agent = metadata(body, "agent_id");
        if (!agent) {
            return std::nullopt;
        }
        observation.agent_id = *agent;
        observation.subagent_transition = child_start
            ? addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED;
        if (body.find("agent_label")) {
            const auto label = metadata(body, "agent_label", addon::limits::k_max_lifecycle_label_bytes);
            if (!label) {
                return std::nullopt;
            }
            observation.agent_label = *label;
        }
        result.observations.push_back(std::move(observation));
        return result;
    }
    if (deleted) {
        observation.session_transition = addon::Session_transition::ENDED;
        result.observations.push_back(std::move(observation));
        return result;
    }
    const auto turn = metadata(body, "turn_id");
    if (!turn) {
        return std::nullopt;
    }
    observation.provider_turn_id = *turn;
    // Native OpenCode reports session identity on each root fact. The host's
    // ledger, rather than adapter process memory, decides whether it starts a
    // new Session; repeated facts must not manufacture fresh host epochs.
    if (!request.ledger.current_transition || request.ledger.current_transition->provider_session_id != *session) {
        observation.session_transition = addon::Session_transition::STARTED;
        result.classification = addon::Classification::TRANSITION;
        result.transition = addon::Transition_kind::START;
    }
    if (status) {
        const auto type = metadata(body, "status_type");
        if (!type || (*type != "busy" && *type != "retry" && *type != "idle")) {
            return std::nullopt;
        }
        observation.phase_transition = *type == "idle"
            ? addon::Phase_transition::AWAITING_INPUT : addon::Phase_transition::WORKING;
        observation.phase_evidence = addon::Evidence::CONFIRMED;
    }
    else
    if (idle || error) {
        observation.phase_transition = addon::Phase_transition::AWAITING_INPUT;
        observation.phase_evidence   = addon::Evidence::CONFIRMED;
        if (error) {
            const auto type = metadata(body, "error_type");
            if (!type || (*type != "failed" && *type != "aborted")) {
                return std::nullopt;
            }
            observation.outcome_transition = *type == "aborted"
                ? addon::Outcome_transition::INTERRUPTED : addon::Outcome_transition::FAILED;
            observation.outcome_evidence = addon::Evidence::CONFIRMED;
        }
    }
    else {
        const auto permission = metadata(body, "permission_id");
        if (!permission) {
            return std::nullopt;
        }
        observation.attention_request_id = *permission;
        observation.attention_transition = asked
            ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED;
        observation.attention_evidence = addon::Evidence::CONFIRMED;
    }
    result.observations.push_back(std::move(observation));
    return result;
}

} // namespace

void handle_lifecycle(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_classify_event_operation) {
        context.reject_unknown_operation();
        return;
    }
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto result = classify(*request);
    if (!result) {
        context.fail(addon::Result_status::FAILED, "invalid_provider_event");
        return;
    }
    context.succeed(addon::encode_classify_event_result(*result));
}

} // namespace opencode
