#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launch_preparation.h"

#include <string>
#include <string_view>

namespace opencode {

struct Inline_config
{
    std::string environment_name = "OPENCODE_CONFIG_CONTENT";
    logonomic::cli_addon::Json_value value = logonomic::cli_addon::Json_value::object();
};

Inline_config inline_config(const logonomic::cli_addon::Json_value& environment,
    std::string_view environment_name = "OPENCODE_CONFIG_CONTENT");
std::string materialize_instruction(std::string_view profile_root, std::string_view instruction,
    bool changed_files_tools = true);
void handle_preparation(logonomic::cli_addon::Invocation_context& context);

} // namespace opencode
