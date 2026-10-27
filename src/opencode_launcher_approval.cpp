#include "opencode_launcher_approval.h"

namespace opencode {

const logonomic::cli_addon::Launcher_approval_rules& launcher_approval_rules()
{
    static const logonomic::cli_addon::Launcher_approval_rules rules{
        .program_names = {
            "opencode", "opencode.exe", "opencode.cmd", "opencode.bat", "opencode.ps1",
            "opencode2", "opencode2.exe", "opencode2.cmd", "opencode2.bat", "opencode2.ps1",
        },
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
        .flag_arguments = {"-c", "--continue", "--fork", "--mdns", "--thinking", "--print-logs", "--standalone"},
        .session_commands = {"run"},
        .unsupported_commands = {
            "agent", "attach", "auth", "completion", "github", "mcp", "models", "serve",
            "session", "stats", "export", "import", "web", "acp", "plugin", "pr", "db",
            "debug", "uninstall", "upgrade", "help",
        },
        .value_arguments_consume_options = false,
        .flag_arguments_consume_booleans = true,
    };
    return rules;
}

} // namespace opencode
