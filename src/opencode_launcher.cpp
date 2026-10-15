#include "opencode_launcher.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace opencode {
namespace {

using addon::lower;
using addon::set_environment;
using addon::regular_file;
using addon::resolve_program;
using addon::native_image;
using addon::path_to_utf8;

using addon::Environment;
using addon::Launcher_whitespace;
namespace fs = std::filesystem;

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last  = value.find_last_not_of(" \t\r\n");
    return first == std::string_view::npos
        ? std::string{} : std::string(value.substr(first, last - first + 1));
}

std::string unquote(std::string_view value)
{
    std::string result = trim(value);
    if (result.size() >= 2 &&
        ((result.front() == '\'' && result.back() == '\'') ||
         (result.front() == '"'  && result.back() == '"')))
    {
        return result.substr(1, result.size() - 2);
    }
    return result;
}

std::string base_name(std::string_view value)
{
    std::string name = lower(unquote(value));
    const auto separator = name.find_last_of("/\\");
    if (separator != std::string::npos) name.erase(0, separator + 1);
    for (const std::string_view suffix : {".exe", ".cmd", ".ps1", ".bat"}) {
        if (name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name;
}

// Words PowerShell passes through unchanged: no quoting, variables, escapes,
// separators, redirections, groupings or comments.
bool plain_powershell_command(std::string_view script)
{
    return !trim(script).empty() && std::all_of(script.begin(), script.end(), [](unsigned char character) {
        return std::isalnum(character) || std::string_view(" \t-_./\\:=").find(static_cast<char>(character)) !=
            std::string_view::npos;
    });
}

bool package_flag(std::string_view value)
{
    return value == "-y" || value == "--yes" || value == "--quiet" ||
        value == "--silent" || value == "--no-install" || value == "--bun";
}

bool environment_name(std::string_view name)
{
    if (name.empty() || (name.front() != '_' &&
        !std::isalpha(static_cast<unsigned char>(name.front())))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return character == '_' || (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
    });
}

std::optional<std::pair<std::string, std::string>> assignment(std::string_view text)
{
    const std::string value = unquote(text);
    const auto separator = value.find('=');
    if (separator == std::string::npos) return std::nullopt;
    const std::string name = trim(value.substr(0, separator));
    if (!environment_name(name) || value.find('\0') != std::string::npos) return std::nullopt;
    return std::pair{name, unquote(value.substr(separator + 1))};
}

std::vector<std::string> tokens(std::string_view text)
{
    static const std::regex pattern(R"re("(?:\\.|[^"])*"|'[^']*'|\S+)re");
    const std::string input(text);
    std::vector<std::string> result;
    for (std::sregex_iterator match(input.begin(), input.end(), pattern), end;
         match != end; ++match)
    {
        result.push_back(match->str());
    }
    return result;
}

std::vector<std::string> segments(std::string_view line)
{
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '\\' && quote == '"' && index + 1 < line.size()) {
            current += character;
            current += line[++index];
            continue;
        }
        if (character == quote) quote = 0;
        else if (!quote && (character == '\'' || character == '"')) quote = character;
        else if (!quote && (character == ';' || character == '&' || character == '|')) {
            if (!trim(current).empty()) result.push_back(trim(current));
            current.clear();
            continue;
        }
        current += character;
    }
    if (!trim(current).empty()) result.push_back(trim(current));
    return result;
}

std::optional<Launcher_command> recognize(std::string_view script, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            lower(line).starts_with("rem ")) continue;
        for (std::string segment : segments(line)) {
            const std::string normalized = lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = assignment(segment.substr(4));
            else if (normalized.starts_with("export ")) standalone = assignment(segment.substr(7));
            else if (normalized.starts_with("$env:")) standalone = assignment(segment.substr(5));
            if (standalone) {
                inherited.push_back(*standalone);
                continue;
            }
            Environment environment = inherited;
            // Leading assignment values may contain quoted whitespace, unlike
            // the command-token grammar used for the remainder of the line.
            static const std::regex leading(R"re(^([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|'[^']*'|[^\s]*)\s*)re");
            std::smatch match;
            while (std::regex_search(segment, match, leading)) {
                environment.emplace_back(match[1].str(), unquote(match[2].str()));
                segment.erase(0, match.length());
            }
            const auto words = tokens(segment);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = assignment(words[index]);
                const auto prefix = lower(unquote(words[index]));
                if (value) environment.push_back(*value);
                else if (prefix != "call" && prefix != "command" && prefix != "exec" && prefix != "&") break;
                ++index;
            }
            if (index == words.size()) continue;
            const std::string program = unquote(words[index]);
            const std::string base = base_name(program);
            if (base == "opencode") return Launcher_command{program, {}, environment};
            if (base == "npx" || base == "bunx") {
                std::vector<std::string> arguments;
                for (++index; index < words.size(); ++index) {
                    const auto argument = unquote(words[index]);
                    arguments.push_back(argument);
                    if (argument.starts_with('-')) {
                        if (!package_flag(argument)) break;
                    }
                    else {
                        const auto package = lower(argument);
                        if (package == "opencode-ai" || package.starts_with("opencode-ai@")) {
                            return Launcher_command{program, arguments, environment};
                        }
                        break;
                    }
                }
            }
            else if (base == "cmd" || base == "powershell" || base == "pwsh" ||
                     base == "bash" || base == "sh")
            {
                for (++index; index + 1 < words.size(); ++index) {
                    const auto argument = lower(unquote(words[index]));
                    if (argument != "/c" && argument != "-c" && argument != "-command") continue;
                    std::string tail;
                    for (++index; index < words.size(); ++index) {
                        if (!tail.empty()) tail += ' ';
                        tail += words[index];
                    }
                    auto command = recognize(unquote(tail), depth - 1);
                    if (command) {
                        command->environment.insert(command->environment.begin(), environment.begin(), environment.end());
                        return command;
                    }
                    break;
                }
            }
        }
    }
    return std::nullopt;
}

bool equal_name(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return lower(std::string(left)) == lower(std::string(right));
#else
    return left == right;
#endif
}

const std::string& semver_pattern()
{
    static const std::string pattern =
        R"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
        R"((?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?)"
        R"((?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)";
    return pattern;
}

bool semver(std::string_view value)
{
    static const std::regex pattern("^" + semver_pattern() + "$", std::regex::ECMAScript);
    return std::regex_match(value.begin(), value.end(), pattern);
}

std::optional<std::string> stream_version(std::string_view value)
{
    if (value.size() > 65536 || value.find('\0') != std::string_view::npos ||
        !addon::is_valid_utf8(value)) return std::nullopt;
    static const std::regex labelled("^(?:OpenCode CLI|opencode)(?:\\s+version)?(?:\\s*[:=]\\s*|\\s+)v?(" +
        semver_pattern() + ")(?:\\s+\\([^\\r\\n()]{1,128}\\))?\\s*$", std::regex::icase);
    std::vector<std::string> lines;
    std::string text(value);
    std::replace(text.begin(), text.end(), '\r', '\n');
    std::istringstream input(text);
    std::string version;
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.empty()) continue;
        lines.push_back(line);
        std::smatch match;
        if (std::regex_match(line, match, labelled)) {
            if (!version.empty() && version != match[1].str()) return std::nullopt;
            version = match[1].str();
        }
    }
    if (!version.empty()) return version;
    if (lines.size() == 1) {
        auto candidate = lines.front();
        if (candidate.starts_with('v') || candidate.starts_with('V')) candidate.erase(0, 1);
        if (semver(candidate)) return candidate;
    }
    return std::string{};
}

#ifdef _WIN32
std::string powershell_quote(std::string_view text)
{
    std::string result = "'";
    for (char character : text) {
        result += character;
        if (character == '\'') result += '\'';
    }
    return result + '\'';
}
#endif

} // namespace

std::optional<Launcher_command> recognize_launcher(std::string_view script)
{
    return recognize(script, 8);
}

std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request)
{
    Environment environment;
    for (const auto& member : request.ambient_environment.as_object()) {
        environment.emplace_back(member.name, member.value.as_string());
    }
    if (!resolve_program("opencode", environment, {}, Launcher_whitespace::ASCII)) return std::nullopt;
    return addon::Launcher_candidate{
        "interactive", "OpenCode", "opencode",
        request.platform_id == "windows-x86_64" ? addon::k_runner_powershell : addon::k_runner_direct,
        "", true, addon::Json_value::object(),
    };
}

std::vector<std::string> literal_launcher_argv(std::string_view script)
{
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    int quotes = 0;
    for (const char character : script) {
        if (character == '"') {
            if (++quotes == 3) { current += '"'; quotes = 0; }
            continue;
        }
        if (quotes == 1) quoted = !quoted;
        quotes = 0;
        if (!quoted && std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) { result.push_back(current); current.clear(); }
        }
        else current += character;
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

std::optional<std::vector<std::string>> resume_arguments(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    std::string_view                session)
{
    const auto command = recognize_launcher(launcher.script_body);
    const auto literal = literal_launcher_argv(launcher.script_body);
    if (!command || !command->prefix_arguments.empty() || !command->environment.empty() ||
        literal.empty() || base_name(literal.front()) != "opencode")
    {
        return std::nullopt;
    }
    // The TUI options (1.18) that keep the reopened session in this directory
    // and interface with plugins loaded, and a project argument naming this
    // directory. Enabled --continue, --session, --fork, --prompt and --pure pick
    // another session, add a turn or drop the plugin; anything unknown is refused
    // rather than guessed at.
    static const std::set<std::string_view> valued{
        "--model", "-m", "--agent", "--log-level", "--port", "--hostname", "--mdns-domain", "--replay-limit"};
    static const std::set<std::string_view> flags{
        "--auto", "--yolo", "--dangerously-skip-permissions", "--print-logs", "--mdns", "--mini"};
    static const std::set<std::string_view> negated{
        "--no-auto", "--no-yolo", "--no-dangerously-skip-permissions", "--no-print-logs", "--no-mdns", "--no-mini",
        "--no-replay", "--replay=false", "--no-continue", "--continue=false", "--no-fork", "--fork=false",
        "--no-pure", "--pure=false"};
    // OpenCode resolves a relative project argument against PWD when it is
    // set, and against its working directory otherwise.
    const auto session_directory = addon::path_from_utf8(working_directory);
    const auto pwd = addon::environment_value(addon::environment_pairs(effective_environment), "PWD");
    const auto project_base = pwd.empty() ? session_directory : addon::path_from_utf8(pwd);
    const auto same_directory = [&session_directory, &project_base](std::string_view project) {
        const auto path = addon::path_from_utf8(project);
        std::error_code project_error;
        std::error_code session_error;
        const auto resolved = fs::weakly_canonical(path.is_absolute() ? path : project_base / path, project_error);
        const auto expected = fs::weakly_canonical(session_directory, session_error);
        return !project_error && !session_error && resolved == expected;
    };
    bool project = false;
    for (std::size_t index = 1; index < literal.size(); ++index) {
        const std::string_view argument = literal[index];
        if (!argument.starts_with('-')) {
            if (project || !same_directory(argument)) {
                return std::nullopt;
            }
            project = true;
            continue;
        }
        if (flags.contains(argument) || negated.contains(argument)) {
            continue;
        }
        const auto equals = argument.find('=');
        if (equals != std::string_view::npos) {
            const auto name  = argument.substr(0, equals);
            const auto value = argument.substr(equals + 1);
            if (!value.empty() && (valued.contains(name) || name == "--cors" ||
                (flags.contains(name) && (value == "true" || value == "false"))))
            {
                continue;
            }
            return std::nullopt;
        }
        // --cors takes every following word up to the next option.
        const bool list = argument == "--cors";
        if ((!list && !valued.contains(argument)) || index + 1 == literal.size() ||
            literal[index + 1].starts_with('-'))
        {
            return std::nullopt;
        }
        ++index;
        while (list && index + 1 < literal.size() && !literal[index + 1].starts_with('-')) {
            ++index;
        }
    }
    if (launcher.runner_key == "direct" && launcher_argv.size() == literal.size() &&
        std::equal(literal.begin() + 1, literal.end(), launcher_argv.begin() + 1))
    {
        std::vector<std::string> arguments(launcher_argv.begin() + 1, launcher_argv.end());
        arguments.insert(arguments.end(), {"--session", std::string(session)});
        return arguments;
    }
    if (launcher.runner_key == "powershell" && plain_powershell_command(launcher.script_body) &&
        launcher_argv.size() == 4 && base_name(launcher_argv[0]) == "powershell" &&
        launcher_argv[1] == "-NoLogo" && launcher_argv[2] == "-Command" && launcher_argv[3] == launcher.script_body)
    {
        return std::vector<std::string>{"-NoLogo", "-Command", launcher.script_body + " --session " + std::string(session)};
    }
    return std::nullopt;
}

bool version_is_at_least(std::string_view version, int major, int minor, int patch)
{
    if (!semver(version)) return false;
    std::array<std::string_view, 3> components;
    auto remaining = version.substr(0, version.find_first_of("-+"));
    for (auto& component : components) {
        const auto separator = remaining.find('.');
        component = remaining.substr(0, separator);
        if (separator != std::string_view::npos) remaining.remove_prefix(separator + 1);
    }
    const std::array minima{major, minor, patch};
    for (std::size_t index = 0; index < minima.size(); ++index) {
        const auto minimum = std::to_string(minima[index]);
        if (components[index].size() != minimum.size()) return components[index].size() > minimum.size();
        if (components[index] != minimum) return components[index] > minimum;
    }
    const auto suffix = version.find_first_of("-+");
    return suffix == std::string_view::npos || version[suffix] != '-';
}

std::string parse_version_output(std::string_view output, std::string_view errors)
{
    const auto first  = stream_version(output);
    const auto second = stream_version(errors);
    if (!first || !second || (!first->empty() && !second->empty() && *first != *second)) return {};
    return first->empty() ? *second : *first;
}

std::optional<Verified_launch> verified_launch(
    const addon::Launcher_request& launcher, const std::vector<std::string>& launcher_argv,
    const addon::Json_value& effective_environment, std::string_view working_directory,
    bool delegated, addon::Invocation_context& context)
{
    auto command = recognize_launcher(launcher.script_body);
    if (!command || !command->prefix_arguments.empty() || launcher_argv.empty()) return std::nullopt;
    const auto literal = literal_launcher_argv(launcher.script_body);
    if (literal.empty() || base_name(literal.front()) != "opencode") return std::nullopt;
    if (delegated && literal.size() != 1) return std::nullopt;
    if (launcher.runner_key == "direct") {
        if (launcher_argv.size() != literal.size() ||
            !std::equal(literal.begin() + 1, literal.end(), launcher_argv.begin() + 1)) return std::nullopt;
    }
    else if (launcher.runner_key != "powershell" || launcher_argv.size() != 4 ||
             base_name(launcher_argv[0]) != "powershell" || launcher_argv[1] != "-NoLogo" ||
             launcher_argv[2] != "-Command" || launcher_argv[3] != launcher.script_body)
    {
        return std::nullopt;
    }
    // An interactive launch runs the script itself. Without quoting, expansion
    // or separators PowerShell reads it as exactly its literal words, the first
    // one naming the command it runs.
    if (!delegated && launcher.runner_key == "powershell" && !plain_powershell_command(launcher.script_body)) {
        return std::nullopt;
    }
    Environment environment;
    if (!effective_environment.is_object()) return std::nullopt;
    for (const auto& member : effective_environment.as_object()) {
        if (!member.value.is_string()) return std::nullopt;
        set_environment(environment, member.name, member.value.as_string());
    }
    // The host already expanded and applied launcher.environment_text. Only
    // script-owned assignments remain to overlay on that effective view.
    for (const auto& [name, value] : command->environment) set_environment(environment, name, value);
    const auto selected = resolve_program(
        command->program, environment, working_directory, Launcher_whitespace::ASCII);
    if (!selected) return std::nullopt;
    if (launcher.runner_key == "direct") {
        const auto requested = resolve_program(
            launcher_argv.front(), environment, working_directory, Launcher_whitespace::ASCII);
        if (!requested || !equal_name(path_to_utf8(*requested), path_to_utf8(*selected))) return std::nullopt;
    }
    auto executable = *selected;
    std::string package_version;
    std::vector<fs::path> files{*selected};
    const auto extension = lower(selected->extension().string());
    if (extension == ".cmd" || extension == ".ps1") {
        const auto root = selected->parent_path() / "node_modules" / "opencode-ai";
        const auto manifest = regular_file(root / "package.json");
        if (!manifest) return std::nullopt;
        std::error_code error;
        if (fs::file_size(*manifest, error) > 1024 * 1024 || error) return std::nullopt;
        std::ifstream input(*manifest, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        addon::Json_parse_error parse_error;
        const auto package = addon::parse_json(bytes, {}, &parse_error);
        if (!package || !package->is_object()) return std::nullopt;
        const auto* name    = package->find("name");
        const auto* version = package->find("version");
        const auto* bin     = package->find("bin");
        if (bin && bin->is_object()) bin = bin->find("opencode");
        if (!name || !name->is_string() || name->as_string() != "opencode-ai" ||
            !version || !version->is_string() || !semver(version->as_string()) ||
            !bin || !bin->is_string()) return std::nullopt;
        std::string bin_path = bin->as_string();
        std::replace(bin_path.begin(), bin_path.end(), '\\', '/');
        if (addon::path_from_utf8(bin_path).lexically_normal() != fs::path("bin/opencode.exe")) return std::nullopt;
        const auto image = regular_file(root / addon::path_from_utf8(bin_path));
        if (!image) return std::nullopt;
        executable      = *image;
        package_version = version->as_string();
        files.push_back(*manifest);
        files.push_back(executable);
    }
    if (!native_image(executable)) return std::nullopt;
    std::vector<std::string> probe_argv{path_to_utf8(executable), "--version"};
    std::vector<std::string> launch_argv{path_to_utf8(executable)};
    int timeout_ms = 3000;
    bool cacheable = true;
    if (delegated) {
        launch_argv = {path_to_utf8(*selected), "acp", "--cwd", std::string(working_directory)};
        probe_argv  = {path_to_utf8(*selected), "--version"};
        if (launcher.runner_key == "powershell" || !native_image(*selected)) {
#ifdef _WIN32
            const auto shell = resolve_program(launcher.runner_key == "powershell"
                ? launcher_argv.front() : "powershell.exe", environment, working_directory, Launcher_whitespace::ASCII);
            if (!shell || !native_image(*shell)) return std::nullopt;
            const auto make_shell = [&](const std::vector<std::string>& arguments) {
                std::string script = "& " + powershell_quote(path_to_utf8(*selected));
                for (const auto& argument : arguments) script += " " + powershell_quote(argument);
                script += "; exit $LASTEXITCODE";
                return std::vector<std::string>{path_to_utf8(*shell), "-NoLogo", "-Command", script};
            };
            probe_argv  = make_shell({"--version"});
            launch_argv = make_shell({"acp", "--cwd", std::string(working_directory)});
            files.push_back(*shell);
            timeout_ms = 10000;
#else
            return std::nullopt;
#endif
        }
    }
    else if (launcher.runner_key == "powershell") {
#ifdef _WIN32
        // Probe the program the launch's own shell resolves and runs, which may
        // be a shim other than the installation the package located. PowerShell
        // resolves it by rules no file identity here tracks (.ps1 precedence,
        // profiles), so this evidence is never reused.
        const auto shell = resolve_program(launcher_argv.front(), environment, working_directory, Launcher_whitespace::ASCII);
        if (!shell || !native_image(*shell)) return std::nullopt;
        probe_argv = {path_to_utf8(*shell), "-NoLogo", "-Command", literal.front() + " --version; exit $LASTEXITCODE"};
        files.push_back(*shell);
        timeout_ms = 10000;
        cacheable  = false;
#else
        return std::nullopt;
#endif
    }
    // The proof belongs to these exact installation bytes. A replacement while
    // the probe runs cannot publish evidence about a different executable.
    std::vector<std::pair<std::uintmax_t, fs::file_time_type>> identities;
    for (const auto& file : files) {
        std::error_code error;
        const auto size = fs::file_size(file, error);
        if (error) return std::nullopt;
        const auto modified = fs::last_write_time(file, error);
        if (error) return std::nullopt;
        identities.emplace_back(size, modified);
    }
    // Keep only a digest of potentially private environment input in the
    // bounded cache. Installation metadata and the exact probe argv are part
    // of the identity; failed and cancelled probes are never cached.
    addon::Sha256 digest;
    const auto append = [&](std::string_view value) {
        digest.update(std::to_string(value.size()));
        digest.update(":");
        digest.update(value);
    };
    append(working_directory);
    for (const auto& argument : probe_argv) append(argument);
    for (const auto& [name, value] : environment) { append(name); append(value); }
    for (std::size_t index = 0; index < files.size(); ++index) {
        append(path_to_utf8(files[index]));
        append(std::to_string(identities[index].first));
        append(addon::file_time_ticks_text(identities[index].second));
    }
    const auto cache_key = addon::to_hex(digest.finish());
    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cache;
    if (cacheable) {
        std::lock_guard lock(cache_mutex);
        const auto cached = cache.find(cache_key);
        if (cached != cache.end() && !context.cancel_requested()) {
            if ((!package_version.empty() && cached->second != package_version) ||
                !version_is_at_least(cached->second, 1, 18, delegated ? 25 : 11)) return std::nullopt;
            return Verified_launch{std::move(launch_argv), cached->second, command->environment};
        }
    }
    const auto output = addon::probe(probe_argv, environment, working_directory, timeout_ms, context);
    if (!output) return std::nullopt;
    for (std::size_t index = 0; index < files.size(); ++index) {
        std::error_code error;
        if (fs::file_size(files[index], error) != identities[index].first || error) return std::nullopt;
        if (fs::last_write_time(files[index], error) != identities[index].second || error) return std::nullopt;
    }
    const auto version = parse_version_output(output->output, output->errors);
    if ((!package_version.empty() && version != package_version) ||
        !version_is_at_least(version, 1, 18, delegated ? 25 : 11)) return std::nullopt;
    if (cacheable) {
        std::lock_guard lock(cache_mutex);
        if (cache.size() >= 32) cache.erase(cache.begin());
        cache[cache_key] = version;
    }
    return Verified_launch{std::move(launch_argv), version, command->environment};
}

} // namespace opencode
