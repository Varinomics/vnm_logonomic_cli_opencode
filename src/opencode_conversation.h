#pragma once

#include <logonomic_cli_addon/conversation_export.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opencode {

namespace addon = logonomic::cli_addon;

inline constexpr const char* k_export_source_environment = "VNM_OPENCODE_EXPORT_SOURCE";

struct Verified_launch;

std::optional<std::string> prepare_conversation_source(const addon::Launcher_request& launcher,
    const std::vector<std::string>& launcher_argv, const addon::Json_value& effective_environment,
    std::string_view working_directory, const Verified_launch* verified);

bool append_legacy_message(const addon::Json_value& info, const std::vector<addon::Json_value>& parts,
    addon::Conversation_writer& writer, addon::Contract_failure* failure);
bool append_tui_message(const addon::Json_value& message,
    addon::Conversation_writer& writer, addon::Contract_failure* failure);

std::optional<addon::Conversation_export_result> export_conversation(
    const addon::Conversation_export_request& request, const std::function<bool()>& cancelled,
    addon::Contract_failure* failure);

} // namespace opencode
