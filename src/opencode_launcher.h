#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opencode {

namespace addon = logonomic::cli_addon;

struct Launcher_command
{
    std::string                                      program;
    std::vector<std::string>                         prefix_arguments;
    std::vector<std::pair<std::string, std::string>> environment;
};

struct Verified_launch
{
    std::vector<std::string>                         argv;
    std::string                                      version;
    std::vector<std::pair<std::string, std::string>> environment;
    bool                                            auto_approve = false;
};

std::optional<Launcher_command> recognize_launcher(std::string_view script, std::string_view runner);
std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request);

// The launcher arguments after its program that reopen OpenCode session
// `session` in `working_directory`: a direct launcher's own arguments, or its
// PowerShell script, with the session selector added. nullopt when the
// launcher's options could select another session or directory, add a turn, or
// leave out the managed plugin. `effective_environment` supplies the PWD that
// OpenCode resolves a relative project argument against.
std::optional<std::vector<std::string>> resume_arguments(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    std::string_view                session);

// Appends one first message to the admitted invocation, retaining its selected
// program, shell, shims, options and environment semantics.
std::optional<std::vector<std::string>> initial_prompt_invocation(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    const Verified_launch&         verified,
    std::string_view                prompt);

// OpenCode 2 uses a shared service by default. A managed TUI owns a private
// server so its model tools retain this launch's Logonomic mailbox authority.
std::optional<std::vector<std::string>> tui_invocation(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    std::string_view                resume_session = {});

bool uses_tui_plugin(std::string_view version);

bool local_interactive_launcher(const addon::Launcher_request& launcher,
    const std::vector<std::string>& launcher_argv, const addon::Json_value& effective_environment,
    std::string_view working_directory);

bool version_is_at_least(std::string_view version, int major, int minor, int patch);
std::string parse_version_output(std::string_view output, std::string_view errors);

struct Delegated_launcher_options
{
    bool                      auto_approve = false;
    std::optional<std::string> model;
};

std::optional<Delegated_launcher_options> delegated_launcher_options(const std::vector<std::string>& literal);

// Probes only a native image or a verified stock npm installation. Delegated
// probes execute the selected shim, preserving its behavior and version evidence.
std::optional<Verified_launch> verified_launch(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    bool                            delegated,
    addon::Invocation_context&     context);

} // namespace opencode
