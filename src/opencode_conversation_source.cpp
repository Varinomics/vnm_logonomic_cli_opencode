#include "opencode_conversation.h"
#include "opencode_launcher.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <filesystem>
#include <string_view>
#include <utility>

namespace opencode {
namespace fs = std::filesystem;

std::optional<std::string> prepare_conversation_source(const addon::Launcher_request& launcher,
    const std::vector<std::string>& launcher_argv, const addon::Json_value& effective_environment,
    std::string_view working_directory, const Verified_launch* verified)
{
    if (!verified) {
        return {};
    }
    const bool tui = uses_tui_plugin(verified->version);
    if (tui ? !tui_invocation(launcher, launcher_argv, effective_environment, working_directory)
            : !local_interactive_launcher(launcher, launcher_argv, effective_environment, working_directory))
    {
        return {};
    }
    auto environment = addon::environment_pairs(effective_environment);
    for (const auto& [name, value] : verified->environment) {
        addon::set_environment(environment, name, value);
    }
    std::optional<std::string> database_override;
    for (const auto& [name, value] : environment) {
#ifdef _WIN32
        const bool selected = addon::lower(name) == "opencode_db";
#else
        const bool selected = name == "OPENCODE_DB";
#endif
        if (selected) {
            database_override = value;
        }
    }
    if (database_override && (*database_override == ":memory:" || (tui && database_override->empty()))) {
        return {};
    }
    fs::path database;
    if (database_override && !database_override->empty()) {
        database = addon::path_from_utf8(*database_override);
        // A Windows drive-relative or root-relative override depends on state
        // outside the frozen source environment.
        if (!database.is_absolute() && (database.has_root_name() || database.has_root_directory())) {
            return {};
        }
    }
    if (!database.is_absolute()) {
        const auto xdg = addon::environment_value(environment, "XDG_DATA_HOME");
        auto data = addon::path_from_utf8(xdg);
        if (xdg.empty()) {
#ifdef _WIN32
            const auto home = addon::environment_value(environment, "USERPROFILE");
#else
            const auto home = addon::environment_value(environment, "HOME");
#endif
            if (home.empty()) {
                return {};
            }
            data = addon::path_from_utf8(home) / ".local" / "share";
        }
        if (!data.is_absolute()) {
            return {};
        }
        data /= "opencode";
        database = data / (database.empty() ? fs::path("opencode.db") : database);
    }
    const auto path = addon::path_to_utf8(database.lexically_normal());
    const std::string source = std::string(tui ? "v2|" : "v1|") + path;
    // This optional fact must fit the existing launch environment grammar;
    // a literal native path is never an add-on asset substitution.
    if (source.size() > addon::limits::k_max_environment_value_bytes ||
        source.find('\0') != std::string::npos || source.find("{{") != std::string::npos)
    {
        return {};
    }
    return source;
}

} // namespace opencode
