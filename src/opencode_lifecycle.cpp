#include "opencode_lifecycle.h"
#include "opencode_conversation.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace opencode {
namespace {

namespace addon = logonomic::cli_addon;

void diagnose_field(
    const addon::Classify_event_request& request,
    std::string field,
    addon::Provider_event_problem problem,
    addon::Provider_event_diagnostic* diagnostic,
    std::string rule = {})
{
    if (diagnostic) {
        *diagnostic = addon::make_provider_event_diagnostic(
            request.event.name, field, problem, request.event.body.find(field), {}, std::move(rule));
    }
}

void diagnose_metadata(
    const addon::Classify_event_request& request,
    const std::string& member,
    addon::Provider_event_diagnostic* diagnostic,
    std::size_t limit = addon::limits::k_max_lifecycle_identity_bytes)
{
    if (!diagnostic) {
        return;
    }
    const auto* value = request.event.body.find(member);
    auto problem = addon::Provider_event_problem::MISSING;
    std::optional<std::uint64_t> size;
    if (value) {
        problem = addon::Provider_event_problem::WRONG_TYPE;
        if (value->is_string()) {
            const auto& text = value->as_string();
            const auto begin = text.find_first_not_of(" \t\r\n");
            problem = addon::Provider_event_problem::EMPTY;
            if (begin != std::string::npos) {
                const auto trimmed = text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
                size = trimmed.size();
                problem = trimmed.size() > limit ? addon::Provider_event_problem::TOO_LARGE
                                                : addon::Provider_event_problem::CONTROL_CHARACTER;
            }
        }
    }
    *diagnostic = addon::make_provider_event_diagnostic(
        request.event.name, member, problem, value, limit, "trimmed_metadata");
    if (size) {
        diagnostic->actual_size = size;
    }
}

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

void retain_resume_record(addon::Lifecycle_observation& observation, const addon::Json_value& body)
{
    addon::Json_value record = addon::Json_value::object();
    record.set("provider_session_id", addon::Json_value(observation.provider_session_id));
    const auto* source = body.find("native_source");
    if (source && source->is_string() && is_conversation_source(source->as_string())) {
        record.set("source", *source);
    }
    observation.resume_record = std::move(record);
}

void selected_directory(const addon::Classify_event_request& request, const std::string& session,
    bool transition, addon::Classify_event_result& result)
{
    if (request.execution_context_version != addon::k_execution_context_version ||
        request.execution_failure)
    {
        return;
    }
    const auto& body = request.event.body;
    const auto* nonce = body.find("activation_nonce");
    const auto* sequence = body.find("execution_sequence");
    if (!nonce || !nonce->is_string() || !addon::is_valid_transition_nonce_hex(nonce->as_string()) ||
        !sequence || !sequence->is_integer() || sequence->as_integer() < 0)
    {
        return;
    }
    // Never attach delayed source evidence to whichever activation happens
    // to be current when this helper delivery reaches the host.
    const auto& expected = transition ? request.transition_nonce : request.ledger.current_transition->nonce;
    if (nonce->as_string() != expected) {
        return;
    }
    addon::Execution_context_update update;
    update.provider_session_id = session;
    update.transition_nonce = nonce->as_string();
    update.sequence = static_cast<std::uint64_t>(sequence->as_integer());
    update.source = "opencode_tui_session_location";
    const auto* location = body.find("native_location");
    if (location && location->is_object()) {
        const auto* workspace = location->find("workspaceID");
        const auto* directory = location->find("directory");
        if (workspace && workspace->is_string() && !workspace->as_string().empty()) {
            update.status = addon::Execution_context_status::UNAVAILABLE;
            update.environment = "unsupported";
        }
        else
        if (!workspace && directory && directory->is_string() &&
            addon::is_execution_local_path(directory->as_string()))
        {
            update.status = addon::Execution_context_status::CONFIRMED;
            update.environment = "local";
            update.cwd = directory->as_string();
        }
    }
    result.execution_updates.push_back(std::move(update));
}

std::optional<addon::Classify_event_result> classify(
    const addon::Classify_event_request& request, addon::Provider_event_diagnostic* diagnostic)
{
    const auto& body = request.event.body;
    const std::string& name = request.event.name;
    addon::Classify_event_result result;
    const auto body_name = metadata(body, "event");
    if (!body_name) {
        diagnose_metadata(request, "event", diagnostic);
        return std::nullopt;
    }
    if (*body_name != name) {
        diagnose_field(request, "event", addon::Provider_event_problem::MISMATCH, diagnostic, "event_name");
        return std::nullopt;
    }
    if (request.context_failure) {
        result.context_failure = addon::Context_failure{
            metadata(body, "session_id").value_or(""), *request.context_failure};
    }
    if (name == "message.updated" || name == "session.compacted") {
        if (request.context_state_version != addon::k_context_state_capability_version ||
            !request.ledger.current_transition)
        {
            return result;
        }
        addon::decode_context_updates(body, result.context_updates, result.context_failure);
        for (const auto& update : result.context_updates) {
            if (update.provider_session_id != request.ledger.current_transition->provider_session_id) {
                result.context_updates.clear();
                break;
            }
        }
        return result;
    }
    if (name == "logonomic.delivery_overflow") {
        result.lifecycle_degradation = addon::Lifecycle_degradation{};
        return result;
    }
    if (name == "logonomic.turn_correlation_degraded") {
        const auto session = metadata(body, "session_id");
        const auto cause   = metadata(body, "degradation_type");
        if (!session || !cause) {
            diagnose_metadata(request, !session ? "session_id" : "degradation_type", diagnostic);
            return std::nullopt;
        }
        if (*cause != "candidate_capacity" && *cause != "conflicting_confirmation") {
            diagnose_field(request, "degradation_type", addon::Provider_event_problem::INVALID_VALUE,
                diagnostic, "correlation_degradation");
            return std::nullopt;
        }
        addon::Lifecycle_degradation degradation;
        degradation.provider_session_id = *session;
        result.lifecycle_degradation = std::move(degradation);
        return result;
    }
    if (name == "logonomic.session.resumed") {
        const auto session = metadata(body, "session_id");
        if (!session) {
            diagnose_metadata(request, "session_id", diagnostic);
            return std::nullopt;
        }
        // The managed plugin read the resumed session from the resumed OpenCode's
        // own store, before any turn. It starts the runtime's first session only;
        // a runtime already following a session keeps it.
        if (request.ledger.current_transition) {
            return result;
        }
        addon::Lifecycle_observation observation;
        observation.provider_event_name = name;
        observation.provider_session_id = *session;
        observation.session_transition  = addon::Session_transition::STARTED;
        observation.session_source      = "resume";
        retain_resume_record(observation, body);
        result.classification = addon::Classification::TRANSITION;
        result.transition     = addon::Transition_kind::START;
        result.observations.push_back(std::move(observation));
        return result;
    }
    const auto* api_generation = body.find("api_generation");
    const bool native_inbox = api_generation && api_generation->is_integer() && api_generation->as_integer() == 2;
    if (name == "logonomic.session.selected") {
        const auto session = metadata(body, "session_id");
        const auto status  = metadata(body, "status_type");
        if (!native_inbox) {
            diagnose_field(request, "api_generation", !api_generation ? addon::Provider_event_problem::MISSING
                    : !api_generation->is_integer() ? addon::Provider_event_problem::WRONG_TYPE
                                                   : addon::Provider_event_problem::INVALID_VALUE,
                diagnostic, "native_inbox_generation");
            return std::nullopt;
        }
        if (!session || !status) {
            diagnose_metadata(request, !session ? "session_id" : "status_type", diagnostic);
            return std::nullopt;
        }
        if (*status != "running" && *status != "idle") {
            diagnose_field(request, "status_type", addon::Provider_event_problem::INVALID_VALUE,
                diagnostic, "selection_status");
            return std::nullopt;
        }
        const auto* sequence = body.find("execution_sequence");
        if (sequence && sequence->is_integer() && sequence->as_integer() > 0 &&
            (!request.ledger.current_transition || request.ledger.current_transition->provider_session_id != *session))
        {
            // Later readiness/directory snapshots belong to the selection
            // captured at sequence zero. They cannot themselves select A
            // again after the host has moved to B.
            return result;
        }
        const auto* activation = body.find("activation_nonce");
        if (request.ledger.current_transition && request.ledger.current_transition->provider_session_id == *session &&
            activation && (!activation->is_string() || activation->as_string() != request.ledger.current_transition->nonce))
        {
            // A delayed A observation must not alter a newer activation of A.
            return result;
        }
        addon::Lifecycle_observation observation;
        observation.provider_event_name = name;
        observation.provider_session_id = *session;
        observation.phase_transition = *status == "running"
            ? addon::Phase_transition::WORKING : addon::Phase_transition::AWAITING_INPUT;
        observation.phase_evidence = addon::Evidence::CONFIRMED;
        result.classification = addon::Classification::EVENT;
        if (!request.ledger.current_transition || request.ledger.current_transition->provider_session_id != *session) {
            observation.session_transition = addon::Session_transition::STARTED;
            retain_resume_record(observation, body);
            result.classification = addon::Classification::TRANSITION;
            // Explicit native selection can return to a session retired by an
            // earlier selection. The host requires RESUME to re-admit that ID.
            result.transition = request.ledger.current_transition
                ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        }
        selected_directory(request, *session, result.classification == addon::Classification::TRANSITION, result);
        result.observations.push_back(std::move(observation));
        return result;
    }
    const bool execution_started     = native_inbox && name == "session.execution.started";
    const bool execution_succeeded   = native_inbox && name == "session.execution.succeeded";
    const bool execution_failed      = native_inbox && name == "session.execution.failed";
    const bool execution_interrupted = native_inbox && name == "session.execution.interrupted";
    const bool execution = execution_started || execution_succeeded || execution_failed || execution_interrupted;
    const bool child_start = name == "logonomic.subagent.started";
    const bool child_stop  = name == "logonomic.subagent.stopped";
    const bool deleted    = name == "session.deleted";
    const bool status     = name == "session.status";
    const bool idle       = name == "session.idle";
    const bool error      = name == "session.error";
    const bool asked      = name == "permission.asked";
    const bool replied    = name == "permission.replied" ||
        (native_inbox && name == "logonomic.permission.resolved");
    const bool question_asked    = name == "question.asked" || (native_inbox && name == "form.created");
    const bool question_replied  = name == "question.replied" || (native_inbox && name == "form.replied");
    const bool question_resolved = name == "question.rejected" ||
        (native_inbox && (name == "form.cancelled" || name == "logonomic.form.resolved"));
    const bool submitted        = name == "chat.message" ||
        (native_inbox && name == "session.inbox.enqueued" && metadata(body, "input_type") == "user");
    if (!(child_start || child_stop || deleted || status || idle || error || asked || replied ||
          question_asked || question_replied || question_resolved || submitted || execution))
    {
        return result;
    }
    const auto session = metadata(body, "session_id");
    if (!session) {
        diagnose_metadata(request, "session_id", diagnostic);
        return std::nullopt;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = name;
    observation.provider_session_id = *session;
    result.classification = addon::Classification::EVENT;
    if (child_start || child_stop) {
        const auto agent = metadata(body, "agent_id");
        if (!agent) {
            diagnose_metadata(request, "agent_id", diagnostic);
            return std::nullopt;
        }
        observation.agent_id = *agent;
        observation.subagent_transition = child_start
            ? addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED;
        if (body.find("agent_label")) {
            const auto label = metadata(body, "agent_label", addon::limits::k_max_lifecycle_label_bytes);
            if (!label) {
                diagnose_metadata(request, "agent_label", diagnostic, addon::limits::k_max_lifecycle_label_bytes);
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
    const bool attention = asked || replied || question_asked || question_replied || question_resolved;
    const auto agent     = attention ? metadata(body, "agent_id") : std::nullopt;
    if (attention && body.find("agent_id") && !agent) {
        diagnose_metadata(request, "agent_id", diagnostic);
        return std::nullopt;
    }
    if (agent) {
        observation.agent_id = *agent;
    }
    // A child prompt can outlive or overlap root turns. Its session and request
    // identities establish attention without attributing it to a root turn.
    const auto turn = metadata(body, "turn_id");
    if (!turn && ((!native_inbox && !agent) || body.find("turn_id"))) {
        diagnose_metadata(request, "turn_id", diagnostic);
        return std::nullopt;
    }
    // OpenCode 2 terminal events can be observed after attaching to a run.
    // They identify the session, but do not invent a missing execution start ID.
    if (turn) observation.provider_turn_id = *turn;
    // Native OpenCode reports session identity on each root fact. The host's
    // ledger, rather than adapter process memory, decides whether it starts a
    // new Session; repeated facts must not manufacture fresh host epochs.
    if (!request.ledger.current_transition || request.ledger.current_transition->provider_session_id != *session) {
        observation.session_transition = addon::Session_transition::STARTED;
        retain_resume_record(observation, body);
        result.classification = addon::Classification::TRANSITION;
        result.transition = addon::Transition_kind::START;
    }
    if (execution) {
        observation.phase_transition = execution_started
            ? addon::Phase_transition::WORKING : addon::Phase_transition::AWAITING_INPUT;
        observation.phase_evidence = addon::Evidence::CONFIRMED;
        if (!execution_started && turn) {
            observation.outcome_transition = execution_succeeded ? addon::Outcome_transition::SUCCEEDED :
                execution_failed ? addon::Outcome_transition::FAILED : addon::Outcome_transition::INTERRUPTED;
            observation.outcome_evidence = addon::Evidence::CONFIRMED;
        }
    }
    else
    if (status) {
        const auto type = metadata(body, "status_type");
        if (!type) {
            diagnose_metadata(request, "status_type", diagnostic);
            return std::nullopt;
        }
        if (*type != "busy" && *type != "retry" && *type != "idle") {
            diagnose_field(request, "status_type", addon::Provider_event_problem::INVALID_VALUE,
                diagnostic, "session_status");
            return std::nullopt;
        }
        // Only the first working status for newly confirmed user input can
        // re-admit a retired 1.x root. Trailing facts keep the host's fence.
        const auto* fresh = body.find("fresh_user_turn");
        if (!native_inbox && result.transition && request.ledger.current_transition &&
            *type != "idle" && fresh && fresh->is_boolean() && fresh->as_boolean()) {
            result.transition = addon::Transition_kind::RESUME;
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
            if (!type) {
                diagnose_metadata(request, "error_type", diagnostic);
                return std::nullopt;
            }
            if (*type != "failed" && *type != "aborted") {
                diagnose_field(request, "error_type", addon::Provider_event_problem::INVALID_VALUE,
                    diagnostic, "session_error");
                return std::nullopt;
            }
            observation.outcome_transition = *type == "aborted"
                ? addon::Outcome_transition::INTERRUPTED : addon::Outcome_transition::FAILED;
            observation.outcome_evidence = addon::Evidence::CONFIRMED;
        }
    }
    else
    if (question_asked || question_replied || question_resolved) {
        const auto question = metadata(body, "question_id");
        if (!question) {
            diagnose_metadata(request, "question_id", diagnostic);
            return std::nullopt;
        }
        if (body.find("question_id")->as_string() != *question) {
            diagnose_field(request, "question_id", addon::Provider_event_problem::INVALID_VALUE,
                diagnostic, "untrimmed_identity");
            return std::nullopt;
        }
        if (question_replied) {
            const auto* nonempty = body.find("answer_nonempty");
            if (!nonempty || !nonempty->is_boolean()) {
                diagnose_field(request, "answer_nonempty", !nonempty ? addon::Provider_event_problem::MISSING
                        : addon::Provider_event_problem::WRONG_TYPE,
                    diagnostic, "boolean");
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
            diagnose_metadata(request, "permission_id", diagnostic);
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
    const addon::Attention_classification_request& request, addon::Provider_event_diagnostic* diagnostic)
{
    const auto base = classify(request.base, diagnostic);
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
        const bool permission = name == "permission.asked" || name == "permission.replied" ||
            name == "logonomic.permission.resolved";
        result.attention_kind = addon::Attention_kind{0, permission ? "permission" : "interactive_question"};
    }
    else
    if ((name == "chat.message" || name == "session.inbox.enqueued") &&
        request.pending_conversational_request_id)
    {
        observation.attention_transition = addon::Attention_transition::RESOLVED;
        observation.attention_evidence = addon::Evidence::INFERRED;
        observation.attention_request_id = *request.pending_conversational_request_id;
        result.attention_kind = addon::Attention_kind{0, "conversational_hint"};
    }
    else
    if (name == "session.idle" ||
        (name == "session.status" && metadata(body, "status_type") == "idle") ||
        (name == "session.execution.succeeded" &&
            observation.outcome_transition == addon::Outcome_transition::SUCCEEDED))
    {
        const auto* normal = body.find("normal_completion");
        const auto* evidence = body.find("assistant_reply_evidence");
        if (normal && normal->is_boolean() && normal->as_boolean() && evidence &&
            addon::looks_like_reply_request_evidence(*evidence))
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
    addon::Provider_event_diagnostic diagnostic;
    if (context.request().capability.capability_id == addon::k_attention_kind_capability_id) {
        const auto request = addon::decode_attention_classification_request(context.request().payload, &failure);
        const auto result = request ? classify_attention(*request, &diagnostic) : std::nullopt;
        if (!result) {
            context.fail(addon::Result_status::FAILED, request ? "invalid_provider_event" : failure.reason_code,
                request ? addon::encode_provider_event_diagnostic(diagnostic) : failure.detail);
        }
        else {
            context.succeed(addon::encode_attention_classification_result(*result));
        }
        return;
    }
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto result = classify(*request, &diagnostic);
    if (!result) {
        context.fail(addon::Result_status::FAILED, "invalid_provider_event",
            addon::encode_provider_event_diagnostic(diagnostic));
        return;
    }
    context.succeed(addon::encode_classify_event_result(*result));
}

} // namespace opencode
