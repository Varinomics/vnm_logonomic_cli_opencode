#include "opencode_conversation.h"
#include "opencode_launcher.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, std::string_view detail)
{
    if (!condition) {
        throw std::runtime_error(std::string(detail));
    }
}

void source_contract(const fs::path& root)
{
    // Native oracle: both pinned global data-root resolvers and database-path
    // rules in docs/conversation_export.md; no installed CLI is executed.
    auto environment = addon::Json_value::object();
    environment.set("HOME", addon::Json_value(addon::path_to_utf8(root / "home")));
    environment.set("USERPROFILE", addon::Json_value(addon::path_to_utf8(root / "home")));
    addon::Launcher_request launcher{"opencode", "direct", ""};
    std::vector<std::string> argv{"opencode"};
    opencode::Verified_launch verified{{"opencode"}, "1.18.33", {}};
    const auto select = [&] {
        return opencode::prepare_conversation_source(
            launcher, argv, environment, addon::path_to_utf8(root), &verified);
    };
    const auto expected = [&](const fs::path& path) {
        return std::string(opencode::uses_tui_plugin(verified.version) ? "v2|" : "v1|") +
            addon::path_to_utf8(path.lexically_normal());
    };
    require(select() == expected(root / "home/.local/share/opencode/opencode.db"),
        "stock legacy default uses the launch's native home");
    environment.set("XDG_DATA_HOME", addon::Json_value(addon::path_to_utf8(root / "data")));
    require(select() == expected(root / "data/opencode/opencode.db"),
        "the native XDG data override owns the source");
    environment.set("OPENCODE_DB", addon::Json_value("relative.db"));
    require(select() == expected(root / "data/opencode/relative.db"),
        "a relative database override is relative to native data, not the working directory");
    environment.set("OPENCODE_DB", addon::Json_value(addon::path_to_utf8(root / "absolute.db")));
    require(select() == expected(root / "absolute.db"), "an absolute database override is preserved");
    verified.environment = {{"OPENCODE_DB", "script.db"}};
    require(select() == expected(root / "data/opencode/script.db"),
        "verified script assignments override the host's pre-script environment");
    verified.environment.clear();
    environment.set("OPENCODE_DB", addon::Json_value(":memory:"));
    require(!select(), "an in-memory legacy database has no retained file source");
    environment.set("OPENCODE_DB", addon::Json_value(""));
    require(select() == expected(root / "data/opencode/opencode.db"),
        "legacy native truthiness treats the empty override as unset");

    verified.version = "2.1.4";
    launcher.script_body = "opencode2";
    argv = {"opencode2"};
    require(!select(), "OpenCode 2 nullish override handling leaves the empty value unexportable");
    environment.erase("OPENCODE_DB");
    require(select() == expected(root / "data/opencode/opencode.db"),
        "stock OpenCode 2 uses the same default file through its private server");
    launcher.script_body = "opencode2 --server https://example.invalid";
    argv = {"opencode2", "--server", "https://example.invalid"};
    require(!select(), "a remote server is not relabelled as local history");
    launcher.script_body = "opencode2";
    argv = {"opencode2"};
    environment.set(opencode::k_export_source_environment, addon::Json_value("v1|inherited.db"));
    require(select() == expected(root / "data/opencode/opencode.db"),
        "an inherited source fact never selects the new launch's store");
    require(!opencode::prepare_conversation_source(
            launcher, argv, environment, addon::path_to_utf8(root), nullptr),
        "an unverified process cannot publish an inherited source fact");
    environment.erase("XDG_DATA_HOME");
    environment.erase("HOME");
    environment.erase("USERPROFILE");
    require(!select(), "missing frozen native home does not use today's OS account");
    environment.set("OPENCODE_DB", addon::Json_value(addon::path_to_utf8(root / "explicit.db")));
    require(select() == expected(root / "explicit.db"),
        "an exact absolute override does not need a home-directory guess");
#ifdef _WIN32
    environment.set("OPENCODE_DB", addon::Json_value("C:relative.db"));
    require(!select(), "drive-relative Windows source selection is not frozen absolute context");
    environment.set("OPENCODE_DB", addon::Json_value("\\relative.db"));
    require(!select(), "root-relative Windows source selection does not inherit a current drive");
#endif
    require(!fs::exists(root / "data") && !fs::exists(root / "absolute.db"),
        "preparing a source fact creates no native directories or database");
}

} // namespace

int main()
{
    const auto root = fs::temp_directory_path() / ("opencode-source-" + addon::random_nonce_hex());
    fs::create_directories(root);
    int result = 0;
    try {
        source_contract(fs::canonical(root));
    }
    catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        result = 1;
    }
    fs::remove_all(root);
    return result;
}
