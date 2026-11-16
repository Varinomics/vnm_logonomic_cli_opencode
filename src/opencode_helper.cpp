#include "opencode_helper.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/limits.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace opencode {
namespace {

namespace addon = logonomic::cli_addon;

// Two deliveries and one acknowledgement each own their wrapper deadline and
// reap allowance. The indexed plugin reserves this interval before requesting
// cancellation; actual exit confirmation remains the SDK's responsibility.
static_assert(3 * (addon::limits::k_helper_client_wrapper_deadline_ms +
                  addon::limits::k_reap_deadline_ms) + 2000 <= 47000);

int refuse()
{
    std::fputs("OpenCode event helper: invalid request or unavailable session.\n", stderr);
    return addon::k_helper_exit_invalid_arguments;
}

bool parent_pipe_closed()
{
#if defined(_WIN32)
    // The plugin owns the write end. EOF is its cancellation signal; it does
    // not terminate this process before the SDK can reap the wrapper it owns.
    DWORD available = 0;
    return !PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr) || available != 0;
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    const int ready = poll(&input, 1, 0);
    return ready > 0 && (input.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
#endif
}

std::optional<addon::Json_value> read_event(bool watch_parent)
{
    std::string bytes;
    while (true) {
        const int next = std::fgetc(stdin);
        if (next == EOF || (watch_parent && next == '\n')) {
            if (watch_parent && next == EOF) {
                return std::nullopt;
            }
            break;
        }
        if (bytes.size() >= addon::limits::k_max_provider_event_bytes) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<char>(next));
    }
    if (std::ferror(stdin)) {
        return std::nullopt;
    }
    addon::Json_parse_error failure;
    auto event = addon::parse_json(bytes, addon::Json_parse_limits{}, &failure);
    if (!event || !event->is_object()) {
        return std::nullopt;
    }
    const auto* name = event->find("event");
    if (!name || !name->is_string() || name->as_string().empty()) {
        return std::nullopt;
    }
    return event;
}

} // namespace

int run_helper(int argc, char** argv)
{
#if defined(_WIN32)
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1)
    {
        return refuse();
    }
#endif
    const bool watch_parent = argc == 3 && std::string_view(argv[2]) == "--watch-parent";
    if ((argc != 2 && !watch_parent) || std::string_view(argv[1]) != "event") {
        return refuse();
    }
    const auto event = read_event(watch_parent);
    addon::Contract_failure failure;
    auto environment = addon::read_helper_environment(&failure);
    if (!event || !environment) {
        return refuse();
    }
    const auto key   = addon::random_nonce_hex();
    const auto* activation = event->find("activation_nonce");
    if (activation && (!activation->is_string() ||
        !addon::is_valid_transition_nonce_hex(activation->as_string())))
    {
        return refuse();
    }
    // The TUI captures this nonce when the native root is selected. Reusing
    // it for that transition binds queued directory evidence to the same
    // activation, including when the user later returns A -> B -> A.
    const auto* sequence = event->find("execution_sequence");
    const bool selection = activation && sequence && sequence->is_integer() && sequence->as_integer() == 0 &&
        event->find("event")->as_string() == "logonomic.session.selected";
    const auto* fresh_turn = event->find("fresh_user_turn");
    const auto& name = event->find("event")->as_string();
    const bool source_transition = activation && (selection || name == "logonomic.session.resumed" ||
        (name == "session.status" && fresh_turn && *fresh_turn == addon::Json_value(true)));
    const auto nonce = source_transition ? activation->as_string() : addon::random_nonce_hex();
    const auto cancelled = [watch_parent] { return watch_parent && parent_pipe_closed(); };
    addon::Helper_client client(std::move(*environment), cancelled);
    auto outcome = client.deliver_provider_event(event->find("event")->as_string(), *event, key, nonce);
    if (!outcome.committed && !cancelled() &&
        (outcome.exit_code == addon::k_helper_exit_transport_failure ||
         outcome.exit_code == addon::k_helper_exit_pending))
    {
        // A lost response must replay the original evidence and transition,
        // never mint a second lifecycle transition.
        outcome = client.deliver_provider_event(event->find("event")->as_string(), *event, key, nonce);
    }
    if (!outcome.committed) {
        std::fputs("OpenCode event helper: delivery did not commit.\n", stderr);
        return outcome.exit_code;
    }
    addon::Json_value result = addon::Json_value::object();
    result.set("accepted", addon::Json_value(outcome.accepted));
    if (!outcome.reason.empty()) {
        result.set("reason", addon::Json_value(outcome.reason));
    }
    if (outcome.helper_response) {
        result.set("helper_response", *outcome.helper_response);
    }
    const std::string bytes = addon::serialize_json(result) + "\n";
    if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() || std::fflush(stdout) != 0) {
        return addon::k_helper_exit_transport_failure;
    }
    // Consumption releases the host's retained private response. A failed
    // acknowledgement does not undo an already committed delivery.
    (void)client.acknowledge_provider_event(key);
    return outcome.accepted ? addon::k_helper_exit_committed : addon::k_helper_exit_transport_failure;
}

} // namespace opencode

int main(int argc, char** argv)
{
    return opencode::run_helper(argc, argv);
}
