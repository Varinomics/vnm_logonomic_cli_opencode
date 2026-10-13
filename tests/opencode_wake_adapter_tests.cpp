#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_wake.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

addon::Json_value invoke(addon::Host_simulator& host, const char* operation, addon::Json_value payload)
{
    const auto id = host.invoke({"session-wake", 1}, operation, std::move(payload), 10000);
    const auto outcome = host.wait_outcome(id, 12000);
    require(outcome && outcome->status == addon::Host_status::SUCCESS, "adapter wake invocation failed");
    return outcome->payload;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 3, "expected package and live bridge registration");
        const std::string registration_text = argv[2];
        const std::string package_text = argv[1];
        const fs::path registration(std::u8string(registration_text.begin(), registration_text.end()));
        addon::Simulator_options options;
        options.package_directory = fs::path(std::u8string(package_text.begin(), package_text.end()));
        options.scratch_directory = registration.parent_path() / "simulator";
        addon::Host_simulator host(std::move(options), nullptr);
        addon::Contract_failure failure;
        require(host.start(&failure), "package adapter could not start");
        addon::Session_wake_target target;
        target.provider_session_id = "ses_recipient";
        target.working_directory = addon::path_to_utf8(registration.parent_path());
        target.launcher_argv = {"opencode"};
        target.launch_environment.platform_id = addon::host_platform_id();
        target.correlation.transition_nonce = std::string(32, 'd');
        target.binding.set("registration_file", addon::Json_value(argv[2]));
        const auto inspected = addon::decode_session_wake_readiness(invoke(host, "inspect",
            addon::encode_session_wake_inspect_request({target})), &failure);
        require(inspected && inspected->available, "adapter did not reach exact live native session");
        addon::Session_wake_notify_request notification;
        notification.target = target;
        notification.notification_id = "adapter_mail";
        notification.instruction = "Read the new message in your Logonomic mailbox.";
        const auto first = addon::decode_session_wake_notify_result(invoke(host, "notify",
            addon::encode_session_wake_notify_request(notification)), &failure);
        const auto replay = addon::decode_session_wake_notify_result(invoke(host, "notify",
            addon::encode_session_wake_notify_request(notification)), &failure);
        require(first && first->disposition == addon::Session_wake_disposition::ACCEPTED &&
            replay && replay->receipt == first->receipt, "adapter did not retain native admission identity");
        target.provider_session_id = "ses_other";
        const auto other = addon::decode_session_wake_readiness(invoke(host, "inspect",
            addon::encode_session_wake_inspect_request({target})), &failure);
        require(other && !other->available && other->reason == "provider_session_unavailable",
            "adapter routed a wake to an unrelated session");
        target.provider_session_id = "ses_recipient";
        target.binding.set("registration_file", addon::Json_value(addon::path_to_utf8(
            registration.parent_path() / "absent.json")));
        notification.target = target;
        const auto absent = addon::decode_session_wake_notify_result(invoke(host, "notify",
            addon::encode_session_wake_notify_request(notification)), &failure);
        require(absent && absent->disposition == addon::Session_wake_disposition::UNAVAILABLE,
            "absent bridge was mistaken for acceptance");
        std::cout << "OpenCode adapter wake bridge passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
