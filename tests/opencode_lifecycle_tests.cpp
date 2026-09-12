#include "opencode_lifecycle.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/lifecycle_events.h>

#include <cstdlib>
#include <cstdint>
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

} // namespace

int main()
{
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
