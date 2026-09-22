#include "opencode_lifecycle.h"

#include <logonomic_cli_addon/capability_ids.h>
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
    void fail(addon::Result_status status, std::string, std::string) override
    {
        m_finished = true;
        m_status   = status;
    }
    addon::Result_status status() const { return m_status; }
    std::optional<addon::Classify_event_result> result() const
    {
        addon::Contract_failure failure;
        return addon::decode_classify_event_result(m_payload, &failure, addon::k_helper_ingress_degradation_version);
    }

private:
    addon::Invoke_request m_request;
    addon::Json_value m_payload;
    addon::Result_status m_status = addon::Result_status::FAILED;
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
    require(call.finished() && call.status() == addon::Result_status::SUCCESS, "classification must succeed");
    auto result = call.result();
    require(result.has_value(), "result must satisfy the public SDK codec");
    return *result;
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
    request.event.body.set("assistant_text", addon::Json_value("Which target should I use?"));
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
}

} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "usage: opencode_lifecycle_tests <package directory>");
    declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
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

    for (const auto* name : {"session.idle", "session.status"}) {
        auto completed = event(name);
        completed.event.body.set("status_type", addon::Json_value("idle"));
        completed.event.body.set("normal_completion", addon::Json_value(true));
        completed.event.body.set("assistant_text", addon::Json_value("Which target should I use?"));
        require(classify(completed).observations.front().attention_transition == addon::Attention_transition::NONE,
            "base helper ingress does not acquire optional hint semantics");
        Classification_call call(addon::Attention_classification_request{completed, std::nullopt});
        opencode::handle_lifecycle(call);
        const auto result = call.attention_result();
        require(result && result->attention_kind && result->attention_kind->kind == "conversational_hint" &&
                result->base.observations.front().attention_evidence == addon::Evidence::INFERRED,
            "normal completed text uses the local SDK heuristic as inferred evidence");
        const auto hint_id = result->base.observations.front().attention_request_id;
        Classification_call resolved(addon::Attention_classification_request{event("chat.message"), hint_id});
        opencode::handle_lifecycle(resolved);
        require(resolved.attention_result() &&
                resolved.attention_result()->base.observations.front().attention_request_id == hint_id &&
                resolved.attention_result()->base.observations.front().attention_transition == addon::Attention_transition::RESOLVED,
            "new user input resolves only the host-selected hint identity");
        for (const auto& text : {std::string("Completed."), std::string(2048, 'x') + "?"}) {
            completed.event.body.set("assistant_text", addon::Json_value(text));
            Classification_call ignored(addon::Attention_classification_request{completed, std::nullopt});
            opencode::handle_lifecycle(ignored);
            require(ignored.attention_result() && !ignored.attention_result()->attention_kind,
                "nonquestion or overlong text does not request inferred attention");
        }
        completed.event.body.set("assistant_text", addon::Json_value("Which target?"));
        completed.event.body.set("normal_completion", addon::Json_value(false));
        Classification_call interrupted(addon::Attention_classification_request{completed, std::nullopt});
        opencode::handle_lifecycle(interrupted);
        require(interrupted.attention_result() && !interrupted.attention_result()->attention_kind,
            "interruption cannot manufacture a completed reply hint");
    }
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
