#include "opencode_launcher_approval.h"

namespace opencode {

const logonomic::cli_addon::Launcher_approval_rules& launcher_approval_rules()
{
    static const logonomic::cli_addon::Launcher_approval_rules rules{
        .program_names = {"opencode", "opencode.exe", "opencode.cmd", "opencode.bat", "opencode.ps1"},
        .canonical_argument = "--auto",
        .bypass_arguments = {"--auto", "--yolo", "--dangerously-skip-permissions"},
        .approval_modes = {},
        .conflicting_arguments = {"--no-auto", "--no-yolo", "--no-dangerously-skip-permissions"},
        .value_arguments = {
            "-m", "--model", "-s", "--session", "--prompt", "--agent", "--port", "--hostname",
            "--mdns-domain", "--cors", "--command", "-f", "--file", "--format", "--attach",
            "--password", "-p", "--username", "-u", "--dir", "--variant", "--log-level",
        },
        .optional_value_arguments = {"--title"},
        .flag_arguments = {"-c", "--continue", "--fork", "--mdns", "--thinking", "--print-logs"},
        .session_commands = {"run"},
        .unsupported_commands = {
            "agent", "attach", "auth", "completion", "github", "mcp", "models", "serve",
            "session", "stats", "export", "import", "web", "acp", "plugin", "pr", "db",
            "debug", "uninstall", "upgrade", "help",
        },
    };
    return rules;
}

} // namespace opencode
