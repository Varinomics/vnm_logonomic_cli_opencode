#include "opencode_lifecycle.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sidecar.h>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

class Classification_call : public addon::Invocation_context
{
public:
    explicit Classification_call(const addon::Classify_event_request& request)
    {
        m_request.operation = addon::k_classify_event_operation;
        m_request.payload   = addon::encode_classify_event_request(request);
    }

    explicit Classification_call(const addon::Attention_classification_request& request)
    {
        m_request.capability = {addon::k_attention_kind_capability_id, addon::k_attention_kind_version};
        m_request.operation = addon::k_classify_event_operation;
        m_request.payload = addon::encode_attention_classification_request(request);
    }

    std::optional<addon::Attention_classification_result> attention_result() const
    {
        addon::Contract_failure failure;
        return addon::decode_attention_classification_result(m_payload, &failure);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int) override { return m_cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    bool finished() const override { return m_finished; }
    void cancel() { m_cancelled = true; }
    void succeed(addon::Json_value payload) override
    {
        m_finished = true;
        m_status   = addon::Result_status::SUCCESS;
        m_payload  = std::move(payload);
    }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_finished = true;
        m_status   = status;
        m_failure_reason = std::move(reason);
    }
    addon::Result_status status() const { return m_status; }
    const std::string& failure_reason() const { return m_failure_reason; }
    std::optional<addon::Classify_event_result> result() const
    {
        addon::Contract_failure failure;
        return addon::decode_classify_event_result(m_payload, &failure, addon::k_helper_ingress_degradation_version);
    }

private:
    addon::Invoke_request m_request;
    addon::Json_value m_payload;
    addon::Result_status m_status = addon::Result_status::FAILED;
    std::string m_failure_reason;
    bool m_finished  = false;
    bool m_cancelled = false;
};

void require(bool condition, const char* detail)
{
    if (!condition) {
        std::cerr << "FAIL: " << detail << '\n';
        std::exit(1);
    }
}

addon::Classify_event_request event(std::string name, bool current_epoch = true)
{
    addon::Classify_event_request request;
    request.event.name = name;
    request.event.body.set("event", addon::Json_value(name));
    request.event.body.set("session_id", addon::Json_value("session-primary"));
    request.event.body.set("turn_id", addon::Json_value("turn-user"));
    request.transition_nonce = std::string(32, 'd');
    if (current_epoch) {
        request.ledger.current_epoch = 1;
        request.ledger.current_transition = addon::Ledger_transition{
            addon::Transition_kind::START, "session-primary", std::string(32, 'e')};
    }
    return request;
}

addon::Classify_event_result classify(const addon::Classify_event_request& request)
{
    Classification_call call(request);
    opencode::handle_lifecycle(call);
    if (call.status() != addon::Result_status::SUCCESS) {
        std::cerr << request.event.name << ": " << call.failure_reason() << '\n';
    }
    require(call.finished() && call.status() == addon::Result_status::SUCCESS, "classification must succeed");
    auto result = call.result();
    require(result.has_value(), "result must satisfy the public SDK codec");
    return *result;
}

void context_observations(const std::filesystem::path& package)
{
    const addon::Directory_package_source source(package);
    const auto bytes = source.read(
        addon::sidecar_relative_path(addon::k_context_state_capability_id, addon::k_context_state_capability_version),
        addon::limits::k_max_sidecar_bytes,
        nullptr);
    require(bytes && addon::parse_sidecar(*bytes, nullptr).has_value(),
        "the package declares the negotiated context capability");

    addon::Context_update usage;
    usage.provider_session_id = "session-primary";
    usage.event_id            = "evt-native-step";
    usage.used_tokens         = 1330;
    usage.context_window      = 4000;
    auto request = event("message.updated");
    addon::encode_context_updates(request.event.body, {usage});
    require(classify(request).context_updates.empty(), "context telemetry requires explicit negotiation");
    request.context_state_version = addon::k_context_state_capability_version;
    const auto measured = classify(request);
    require(measured.classification == addon::Classification::IGNORED && measured.observations.empty() &&
            measured.context_updates.size() == 1 && measured.context_updates.front().used_tokens == 1330 &&
            measured.context_updates.front().context_window == 4000,
        "native context measurements supplement the existing helper result");
    Classification_call attention(addon::Attention_classification_request{request, std::nullopt});
    opencode::handle_lifecycle(attention);
    require(attention.attention_result() && attention.attention_result()->base.context_updates.size() == 1,
        "attention companion classification preserves negotiated context");

    addon::Context_state state;
    addon::apply_context_update(state, measured.context_updates.front());
    addon::Context_update compacted;
    compacted.provider_session_id = "session-primary";
    compacted.event_id            = "evt-native-compaction";
    compacted.compacted           = true;
    request.event.name = "session.compacted";
    request.event.body.set("event", addon::Json_value(request.event.name));
    addon::encode_context_updates(request.event.body, {compacted});
    const auto completion = classify(request);
    require(completion.context_updates.size() == 1 && completion.context_updates.front().compacted &&
            completion.context_updates.front().event_id == compacted.event_id,
        "successful native compaction identity survives classification");
    addon::apply_context_update(state, completion.context_updates.front());
    require(state.compaction_count == 1 && !state.compaction_count_complete && !state.used_tokens,
        "successful event clears occupancy without inventing complete history");
    addon::apply_context_update(state, usage);
    addon::apply_context_update(state, completion.context_updates.front());
    require(state.compaction_count == 1 && state.used_tokens == 1330,
        "redelivery after usage neither counts nor clears usage again");

    request.ledger.current_transition->provider_session_id = "session-other";
    require(classify(request).context_updates.empty(), "context belongs to the exact active provider session");
    request.ledger.current_transition.reset();
    request.ledger.current_epoch = 0;
    require(classify(request).context_updates.empty(), "context cannot create a session transition");

    auto malformed = event("message.updated");
    malformed.context_state_version = addon::k_context_state_capability_version;
    addon::encode_context_updates(malformed.event.body, {usage});
    auto& invalid_update = malformed.event.body.find("context_updates")->as_array().front();
    invalid_update.set("used_tokens", addon::Json_value((std::int64_t)-1));
    const auto rejected_context = classify(malformed);
    require(rejected_context.context_updates.empty() && rejected_context.context_failure &&
            rejected_context.context_failure->provider_session_id == "session-primary",
        "malformed optional context reports its own failure through the shared result codec");
    invalid_update.set("provider_session_id", addon::Json_value("session-retired"));
    const auto retired = classify(malformed);
    require(retired.context_failure && retired.context_failure->provider_session_id == "session-retired",
        "a malformed native event retains its source session instead of borrowing the active ledger");
    invalid_update.erase("provider_session_id");
    const auto unscoped = classify(malformed);
    require(unscoped.context_failure && unscoped.context_failure->provider_session_id.empty(),
        "a failure without native session evidence remains unscoped");
    invalid_update.set("provider_session_id", addon::Json_value("session-primary"));
    auto other_source = invalid_update;
    other_source.set("provider_session_id", addon::Json_value("session-retired"));
    malformed.event.body.find("context_updates")->push_back(std::move(other_source));
    const auto mixed = classify(malformed);
    require(mixed.context_failure && mixed.context_failure->provider_session_id.empty(),
        "mixed update sources cannot borrow the outer native body's identity");
    malformed.event.body.set("context_updates", addon::Json_value("invalid"));
    const auto invalid_envelope = classify(malformed);
    require(invalid_envelope.context_failure && invalid_envelope.context_failure->provider_session_id.empty(),
        "a malformed optional envelope cannot acquire session authority");

    auto status = event("session.status");
    status.context_state_version = 999;
    status.event.body.set("status_type", addon::Json_value("busy"));
    status.event.body.set("context_updates", addon::Json_value("invalid"));
    const auto unsupported = classify(status);
    require(unsupported.observations.front().phase_transition == addon::Phase_transition::WORKING &&
            unsupported.context_failure && unsupported.context_failure->provider_session_id == "session-primary",
        "unsupported optional telemetry cannot discard a valid native lifecycle fact");
}

// Oracle: the host's feature gate. It rejects an observation carrying a fact
// the package's own declaration leaves unsupported, phase and all.
void declared_profile_admits_every_shape(const std::filesystem::path& package)
{
    const addon::Directory_package_source source(package);
    const auto bytes = source.read(
        addon::sidecar_relative_path(addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version),
        addon::limits::k_max_sidecar_bytes,
        nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    const auto declaration = sidecar ? addon::decode_lifecycle_declaration(sidecar->payload, nullptr) : std::nullopt;
    require(declaration.has_value(), "package lifecycle declaration is readable");
    std::optional<std::string> pending;
    const auto admitted = [
            &declaration,
            &pending
        ](
            addon::Classify_event_request request,
            const char*                   shape)
        {
            Classification_call call(addon::Attention_classification_request{std::move(request), pending});
            opencode::handle_lifecycle(call);
            const auto result = call.attention_result();
            require(result && !result->base.observations.empty(), shape);
            for (const auto& observation : result->base.observations) {
                require(addon::lifecycle_profile_admits(declaration->features, observation), shape);
            }
            return *result;
        };
    auto request = event("session.status", false);
    request.event.body.set("status_type", addon::Json_value("busy"));
    admitted(request, "the declaration refuses the status that starts a Session");
    for (const auto* type : {"busy", "retry", "idle"}) {
        request = event("session.status");
        request.event.body.set("status_type", addon::Json_value(type));
        admitted(request, "the declaration refuses a status");
    }
    request = event("session.idle");
    request.event.body.set("normal_completion", addon::Json_value(true));
    request.event.body.set("assistant_reply_evidence", addon::assistant_reply_evidence("Which target should I use?"));
    const auto hint = admitted(request, "the declaration refuses a completion that ends in a question");
    pending = hint.base.observations.front().attention_request_id;
    admitted(event("chat.message"), "the declaration refuses the reply that resolves a conversational hint");
    pending.reset();
    for (const auto* type : {"failed", "aborted"}) {
        request = event("session.error");
        request.event.body.set("error_type", addon::Json_value(type));
        admitted(request, "the declaration refuses a failed or aborted turn");
    }
    for (const auto* name : {"permission.asked", "permission.replied"}) {
        request = event(name);
        request.event.body.set("permission_id", addon::Json_value("permission-primary"));
        admitted(request, "the declaration refuses an approval request or its reply");
    }
    for (const auto* name : {"question.asked", "question.replied", "question.rejected"}) {
        request = event(name);
        request.event.body.set("question_id", addon::Json_value("que_native_question"));
        request.event.body.set("answer_nonempty", addon::Json_value(true));
        admitted(request, "the declaration refuses a structured question or its answer");
    }
    for (const auto* name : {"logonomic.subagent.started", "logonomic.subagent.stopped"}) {
        request = event(name);
        request.event.body.set("agent_id", addon::Json_value("child-explore"));
        admitted(request, "the declaration refuses a subagent transition");
    }
    admitted(event("session.deleted"), "the declaration refuses a session end");
    admitted(event("logonomic.session.resumed", false), "the declaration refuses a resumed session's start");
}

} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "usage: opencode_lifecycle_tests <package directory>");
    declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
    context_observations(addon::path_from_utf8(argv[1]));
    auto selected = event("logonomic.session.selected", false);
    selected.event.body.erase("turn_id");
    selected.event.body.set("api_generation", addon::Json_value(2));
    selected.event.body.set("status_type", addon::Json_value("running"));
    const auto selected_observation = classify(selected).observations.front();
    require(selected_observation.session_transition == addon::Session_transition::STARTED &&
        selected_observation.phase_transition == addon::Phase_transition::WORKING &&
        selected_observation.provider_turn_id.empty(),
        "selecting an already-running native session observes its phase without inventing a turn");
    selected.ledger.current_epoch = 2;
    selected.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "session-other", std::string(32, 'f')};
    const auto reselected = classify(selected);
    require(reselected.classification == addon::Classification::TRANSITION &&
        reselected.transition == addon::Transition_kind::RESUME &&
        reselected.observations.front().provider_session_id == "session-primary",
        "explicit native re-selection resumes a session that the host may have retired");
    for (const auto* name : {"session.execution.succeeded", "session.execution.failed", "session.execution.interrupted"}) {
        auto terminal = event(name);
        terminal.event.body.set("api_generation", addon::Json_value(2));
        const auto completed = classify(terminal).observations.front();
        require(completed.phase_transition == addon::Phase_transition::AWAITING_INPUT &&
            completed.outcome_transition != addon::Outcome_transition::NONE && completed.provider_turn_id == "turn-user",
            "native execution outcome belongs to its observed start identity");
        terminal.event.body.erase("turn_id");
        const auto attached = classify(terminal).observations.front();
        require(attached.phase_transition == addon::Phase_transition::AWAITING_INPUT &&
            attached.outcome_transition == addon::Outcome_transition::NONE && attached.provider_turn_id.empty(),
            "terminal events after attaching retain phase but cannot assert an outcome for an unknown turn");
    }
    for (const auto* name : {"form.created", "form.replied", "form.cancelled"}) {
        auto request = event(name);
        request.event.body.set("api_generation", addon::Json_value(2));
        request.event.body.set("question_id", addon::Json_value("frm_native_question"));
        request.event.body.set("answer_nonempty", addon::Json_value(true));
        const auto observation = classify(request).observations.front();
        require(observation.attention_request_id == "frm_native_question" &&
            observation.attention_transition == (std::string_view(name) == "form.created"
                ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED),
            "native form events preserve attention identity without answer prose");
    }

    for (const auto* name : {"logonomic.permission.resolved", "logonomic.form.resolved"}) {
        const bool permission = std::string_view(name) == "logonomic.permission.resolved";
        auto request = event(name);
        request.event.body.erase("turn_id");
        request.event.body.set("api_generation", addon::Json_value(2));
        request.event.body.set(permission ? "permission_id" : "question_id", addon::Json_value("request-removed"));
        const auto observation = classify(request).observations.front();
        require(observation.provider_event_name == name && observation.provider_turn_id.empty() &&
                observation.attention_request_id == "request-removed" &&
                observation.attention_transition == addon::Attention_transition::RESOLVED &&
                observation.attention_evidence == addon::Evidence::CONFIRMED &&
                observation.phase_transition == addon::Phase_transition::NONE &&
                observation.outcome_transition == addon::Outcome_transition::NONE,
            "native pending-list absence resolves its exact request without inventing a reply or execution outcome");
        Classification_call call(addon::Attention_classification_request{request, std::nullopt});
        opencode::handle_lifecycle(call);
        const auto result = call.attention_result();
        require(result && result->attention_kind &&
                result->attention_kind->kind == (permission ? "permission" : "interactive_question"),
            "snapshot resolution preserves the native prompt kind");
        request.event.body.erase("api_generation");
        require(classify(request).observations.empty(), "snapshot resolution belongs only to the native TUI contract");
    }
    for (const auto* name : {"question.asked", "question.replied", "question.rejected"}) {
        auto request = event(name);
        request.event.body.set("question_id", addon::Json_value("que_native_question"));
        if (std::string_view(name) == "question.replied") {
            request.event.body.set("answer_nonempty", addon::Json_value(true));
        }
        const auto observation = classify(request).observations.front();
        require(observation.provider_event_name == name &&
                observation.attention_request_id == "que_native_question" &&
                observation.attention_transition == (std::string_view(name) == "question.asked"
                    ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED) &&
                observation.attention_evidence == addon::Evidence::CONFIRMED,
            "native question events preserve identity and distinguish reply from rejection");
        Classification_call call(addon::Attention_classification_request{request, std::nullopt});
        opencode::handle_lifecycle(call);
        const auto result = call.attention_result();
        require(result && result->attention_kind && result->attention_kind->kind == "interactive_question",
            "optional companion identifies explicit native questions");
    }
    auto invalid_reply = event("question.replied");
    invalid_reply.event.body.set("question_id", addon::Json_value("que_native_question"));
    Classification_call missing_answer(invalid_reply);
    opencode::handle_lifecycle(missing_answer);
    require(missing_answer.status() == addon::Result_status::FAILED,
        "missing answer indication cannot be treated as a verified reply");
    invalid_reply.event.body.set("answer_nonempty", addon::Json_value(false));
    require(classify(invalid_reply).observations.front().provider_event_name == "question.replied",
        "an empty native reply remains a reply but cannot prove nonempty answering");

    auto permission = event("permission.asked");
    permission.event.body.set("permission_id", addon::Json_value("permission-separate"));
    Classification_call permission_call(addon::Attention_classification_request{permission, std::nullopt});
    opencode::handle_lifecycle(permission_call);
    require(permission_call.attention_result() && permission_call.attention_result()->attention_kind &&
            permission_call.attention_result()->attention_kind->kind == "permission",
        "permissions are not classified as questions");

    for (const auto* name : {"permission.asked", "permission.replied",
        "question.asked", "question.replied", "question.rejected"})
    {
        const bool is_permission = std::string_view(name).starts_with("permission.");
        auto request = event(name);
        request.event.body.erase("turn_id");
        request.event.body.set("agent_id", addon::Json_value("background-child"));
        request.event.body.set(is_permission ? "permission_id" : "question_id",
            addon::Json_value("background-request"));
        if (std::string_view(name) == "question.replied") {
            request.event.body.set("answer_nonempty", addon::Json_value(true));
        }
        const auto observation = classify(request).observations.front();
        require(observation.provider_session_id == "session-primary" &&
                observation.agent_id == "background-child" && observation.provider_turn_id.empty() &&
                observation.attention_request_id == "background-request" &&
                observation.attention_transition == (std::string_view(name).ends_with(".asked")
                    ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED) &&
                observation.phase_transition == addon::Phase_transition::NONE &&
                observation.outcome_transition == addon::Outcome_transition::NONE,
            "background child attention retains ownership without claiming a root turn or phase");
        Classification_call call(addon::Attention_classification_request{request, std::nullopt});
        opencode::handle_lifecycle(call);
        const auto result = call.attention_result();
        require(result && result->attention_kind &&
                result->attention_kind->kind == (is_permission ? "permission" : "interactive_question"),
            "background child prompts retain their confirmed attention kind");
        request.event.body.set("agent_id", addon::Json_value(""));
        Classification_call malformed_child(request);
        opencode::handle_lifecycle(malformed_child);
        require(malformed_child.status() == addon::Result_status::FAILED,
            "missing child identity cannot bypass root turn correlation");
    }

    for (const auto* name : {"session.idle", "session.status", "session.execution.succeeded"}) {
        auto completed = event(name);
        const bool native_inbox = std::string_view(name) == "session.execution.succeeded";
        if (native_inbox) completed.event.body.set("api_generation", addon::Json_value(2));
        completed.event.body.set("status_type", addon::Json_value("idle"));
        completed.event.body.set("normal_completion", addon::Json_value(true));
        completed.event.body.set("assistant_reply_evidence", addon::assistant_reply_evidence("Which target should I use?"));
        require(classify(completed).observations.front().attention_transition == addon::Attention_transition::NONE,
            "base helper ingress does not acquire optional hint semantics");
        Classification_call call(addon::Attention_classification_request{completed, std::nullopt});
        opencode::handle_lifecycle(call);
        const auto result = call.attention_result();
        require(result && result->attention_kind && result->attention_kind->kind == "conversational_hint" &&
                result->base.observations.front().attention_evidence == addon::Evidence::INFERRED,
            "normal completed text uses the local SDK heuristic as inferred evidence");
        const auto hint_id = result->base.observations.front().attention_request_id;
        auto submitted = event(native_inbox ? "session.inbox.enqueued" : "chat.message");
        if (native_inbox) {
            submitted.event.body.erase("turn_id");
            submitted.event.body.set("api_generation", addon::Json_value(2));
            submitted.event.body.set("input_type", addon::Json_value("user"));
        }
        Classification_call resolved(addon::Attention_classification_request{submitted, hint_id});
        opencode::handle_lifecycle(resolved);
        require(resolved.attention_result() &&
                resolved.attention_result()->base.observations.front().attention_request_id == hint_id &&
                resolved.attention_result()->base.observations.front().attention_transition == addon::Attention_transition::RESOLVED,
            "new user input resolves only the host-selected hint identity");
        if (native_inbox) {
            for (const auto* input_type : {"synthetic", "compaction", "move"}) {
                submitted.event.body.set("input_type", addon::Json_value(input_type));
                Classification_call unrelated(addon::Attention_classification_request{submitted, hint_id});
                opencode::handle_lifecycle(unrelated);
                require(unrelated.attention_result() && unrelated.attention_result()->base.observations.empty() &&
                        !unrelated.attention_result()->attention_kind,
                    "native synthetic or housekeeping input cannot answer a conversational hint");
            }
            auto attached = completed;
            attached.event.body.erase("turn_id");
            Classification_call unknown_turn(addon::Attention_classification_request{attached, std::nullopt});
            opencode::handle_lifecycle(unknown_turn);
            require(unknown_turn.attention_result() && !unknown_turn.attention_result()->attention_kind,
                "a terminal event without its observed start cannot infer a completed reply");
        }
        for (const auto& evidence : {addon::assistant_reply_evidence("Completed."),
            addon::assistant_reply_evidence("```\nWhich target?\n```"), addon::Json_value("malformed")})
        {
            completed.event.body.set("assistant_reply_evidence", evidence);
            Classification_call ignored(addon::Attention_classification_request{completed, std::nullopt});
            opencode::handle_lifecycle(ignored);
            require(ignored.attention_result() && !ignored.attention_result()->attention_kind,
                "nonquestion, excluded or malformed evidence does not request inferred attention");
        }
        completed.event.body.set("assistant_reply_evidence", addon::assistant_reply_evidence("Which target?"));
        completed.event.body.set("normal_completion", addon::Json_value(false));
        Classification_call interrupted(addon::Attention_classification_request{completed, std::nullopt});
        opencode::handle_lifecycle(interrupted);
        require(interrupted.attention_result() && !interrupted.attention_result()->attention_kind,
            "interruption cannot manufacture a completed reply hint");
    }
    // A resumed session starts the runtime's first session, from the resume,
    // and never replaces a session the runtime already follows.
    auto resumed = event("logonomic.session.resumed", false);
    resumed.event.body.erase("turn_id");
    const auto resumed_start = classify(resumed);
    require(resumed_start.classification == addon::Classification::TRANSITION &&
            resumed_start.transition == addon::Transition_kind::START &&
            resumed_start.observations.size() == 1 &&
            resumed_start.observations[0].session_transition == addon::Session_transition::STARTED &&
            resumed_start.observations[0].provider_session_id == "session-primary" &&
            resumed_start.observations[0].session_source == "resume" &&
            resumed_start.observations[0].phase_transition == addon::Phase_transition::NONE,
        "a resumed session starts from the resume without a turn");
    auto followed = event("logonomic.session.resumed");
    followed.event.body.set("session_id", addon::Json_value("session-resumed-later"));
    const auto ignored = classify(followed);
    require(ignored.classification == addon::Classification::IGNORED && ignored.observations.empty(),
        "a runtime that follows a session keeps it");
    // The native OpenCode event decoder and lifecycle ledger contracts are the
    // oracle: repeated status facts do not open a second provider epoch.
    auto started = event("session.status", false);
    started.event.body.set("status_type", addon::Json_value("busy"));
    const auto transition = classify(started);
    require(transition.classification == addon::Classification::TRANSITION &&
            transition.transition == addon::Transition_kind::START && transition.observations.size() == 1,
        "initial root status starts exactly one Session");
    require(transition.observations[0].session_transition == addon::Session_transition::STARTED &&
            transition.observations[0].phase_transition == addon::Phase_transition::WORKING &&
            transition.observations[0].phase_evidence == addon::Evidence::CONFIRMED,
        "initial root status preserves confirmed working evidence");
    for (const auto* type : {"busy", "retry", "idle"}) {
        auto request = event("session.status");
        request.event.body.set("status_type", addon::Json_value(type));
        const auto result = classify(request);
        require(result.classification == addon::Classification::EVENT &&
                result.observations[0].session_transition == addon::Session_transition::NONE,
            "same-session status does not manufacture epochs");
        require(result.observations[0].phase_transition == (std::string_view(type) == "idle"
                    ? addon::Phase_transition::AWAITING_INPUT : addon::Phase_transition::WORKING),
            "status preserves native phase mapping");
    }
    for (const auto* type : {"failed", "aborted"}) {
        auto request = event("session.error");
        request.event.body.set("error_type", addon::Json_value(type));
        const auto observation = classify(request).observations[0];
        require(observation.outcome_transition == (std::string_view(type) == "aborted"
                    ? addon::Outcome_transition::INTERRUPTED : addon::Outcome_transition::FAILED) &&
                observation.outcome_evidence == addon::Evidence::CONFIRMED &&
                observation.provider_turn_id == "turn-user",
            "error outcome stays correlated to its exact user turn");
    }
    for (const auto* name : {"permission.asked", "permission.replied"}) {
        auto request = event(name);
        request.event.body.set("permission_id", addon::Json_value("permission-primary"));
        const auto observation = classify(request).observations[0];
        require(observation.attention_request_id == "permission-primary" &&
                observation.attention_evidence == addon::Evidence::CONFIRMED &&
                observation.attention_transition == (std::string_view(name) == "permission.asked"
                    ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED),
            "permission request and response preserve pairing");
    }
    for (const auto* name : {"logonomic.subagent.started", "logonomic.subagent.stopped"}) {
        auto request = event(name);
        request.event.body.set("agent_id", addon::Json_value("child-explore"));
        request.event.body.set("agent_label", addon::Json_value("explore"));
        const auto observation = classify(request).observations[0];
        require(observation.agent_id == "child-explore" && observation.agent_label == "explore" &&
                observation.subagent_transition == (std::string_view(name) == "logonomic.subagent.started"
                    ? addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED),
            "child transitions retain agent identity and pairing");
    }
    require(classify(event("session.deleted")).observations[0].session_transition == addon::Session_transition::ENDED,
        "session deletion ends its exact Session");
    for (const bool current_epoch : {false, true}) {
        auto deleted = event("session.deleted", current_epoch);
        if (current_epoch) {
            deleted.ledger.current_transition->provider_session_id = "session-other";
        }
        const auto deletion = classify(deleted);
        require(deletion.classification == addon::Classification::EVENT && !deletion.transition &&
                deletion.observations[0].session_transition == addon::Session_transition::ENDED,
            "deletion cannot manufacture a start for an absent or different ledger Session");
    }
    const auto idle = classify(event("session.idle")).observations[0];
    require(idle.phase_transition == addon::Phase_transition::AWAITING_INPUT &&
            idle.outcome_transition == addon::Outcome_transition::NONE,
        "idle carries phase evidence, not an invented completion outcome");

    auto overflow = event("logonomic.delivery_overflow", false);
    overflow.event.body.erase("session_id");
    overflow.event.body.erase("turn_id");
    const auto lost = classify(overflow);
    require(lost.classification == addon::Classification::IGNORED && lost.observations.empty() &&
            lost.lifecycle_degradation && !lost.lifecycle_degradation->provider_session_id,
        "overflow explicitly degrades lifecycle before an epoch exists");
    for (const auto* cause : {"candidate_capacity", "conflicting_confirmation"}) {
        auto request = event("logonomic.turn_correlation_degraded");
        request.event.body.set("degradation_type", addon::Json_value(cause));
        const auto lost_correlation = classify(request);
        require(lost_correlation.lifecycle_degradation &&
                lost_correlation.lifecycle_degradation->provider_session_id == "session-primary" &&
                lost_correlation.observations.empty(),
            "correlation loss requests exact-session quarantine");
    }
    auto malformed = event("session.error");
    malformed.event.body.set("error_type", addon::Json_value("failed"));
    malformed.event.body.erase("turn_id");
    Classification_call invalid(malformed);
    opencode::handle_lifecycle(invalid);
    require(invalid.status() == addon::Result_status::FAILED, "missing turn cannot become an uncorrelated failure");
    Classification_call cancelled(started);
    cancelled.cancel();
    opencode::handle_lifecycle(cancelled);
    require(cancelled.status() == addon::Result_status::CANCELLED, "cancelled classification has one cancelled result");
    return 0;
}
