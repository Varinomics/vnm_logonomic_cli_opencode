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
    const bool question_asked    = name == "question.asked";
    const bool question_replied  = name == "question.replied";
    const bool question_rejected = name == "question.rejected";
    const bool submitted        = name == "chat.message";
    if (!(child_start || child_stop || deleted || status || idle || error || asked || replied ||
          question_asked || question_replied || question_rejected || submitted))
    {
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
    else
    if (question_asked || question_replied || question_rejected) {
        const auto question = metadata(body, "question_id");
        if (!question || body.find("question_id")->as_string() != *question) {
            return std::nullopt;
        }
        if (question_replied) {
            const auto* nonempty = body.find("answer_nonempty");
            if (!nonempty || !nonempty->is_boolean()) {
                return std::nullopt;
            }
        }
        observation.attention_request_id = *question;
        observation.attention_transition = question_asked
            ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED;
        observation.attention_evidence = addon::Evidence::CONFIRMED;
    }
    else
    if (!submitted) {
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

std::optional<addon::Attention_classification_result> classify_attention(
    const addon::Attention_classification_request& request)
{
    const auto base = classify(request.base);
    if (!base) {
        return std::nullopt;
    }
    addon::Attention_classification_result result{*base, std::nullopt};
    if (result.base.observations.empty()) {
        return result;
    }
    auto& observation = result.base.observations.front();
    const auto& name = request.base.event.name;
    const auto& body = request.base.event.body;
    if (observation.attention_transition != addon::Attention_transition::NONE) {
        const bool permission = name == "permission.asked" || name == "permission.replied";
        result.attention_kind = addon::Attention_kind{0, permission ? "permission" : "interactive_question"};
    }
    else
    if (name == "chat.message" && request.pending_conversational_request_id) {
        observation.attention_transition = addon::Attention_transition::RESOLVED;
        observation.attention_evidence = addon::Evidence::INFERRED;
        observation.attention_request_id = *request.pending_conversational_request_id;
        result.attention_kind = addon::Attention_kind{0, "conversational_hint"};
    }
    else
    if (name == "session.idle" ||
        (name == "session.status" && metadata(body, "status_type") == "idle"))
    {
        const auto* normal = body.find("normal_completion");
        const auto* text = body.find("assistant_text");
        if (normal && normal->is_boolean() && normal->as_boolean() && text && text->is_string() &&
            text->as_string().size() <= 2048 && addon::looks_like_reply_request(text->as_string()))
        {
            observation.attention_transition = addon::Attention_transition::REQUESTED;
            observation.attention_evidence = addon::Evidence::INFERRED;
            observation.attention_request_id = "conversation-" + std::to_string(request.base.ledger.current_epoch) + "-" +
                (request.base.ledger.current_transition ? request.base.ledger.current_transition->nonce : std::string{}) + "-" +
                addon::provider_event_evidence_digest_hex(request.base.event);
            result.attention_kind = addon::Attention_kind{0, "conversational_hint"};
        }
    }
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
    if (context.request().capability.capability_id == addon::k_attention_kind_capability_id) {
        const auto request = addon::decode_attention_classification_request(context.request().payload, &failure);
        const auto result = request ? classify_attention(*request) : std::nullopt;
        if (!result) {
            context.fail(addon::Result_status::FAILED, request ? "invalid_provider_event" : failure.reason_code);
        }
        else {
            context.succeed(addon::encode_attention_classification_result(*result));
        }
        return;
    }
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
