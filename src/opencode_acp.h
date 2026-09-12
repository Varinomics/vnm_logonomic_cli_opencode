#pragma once

#include <logonomic_cli_addon/session_contracts.h>

#include <string>

namespace opencode {

logonomic::cli_addon::Inspection_setup make_acp_inspection(
    const std::string& prompt,
    const std::string& working_directory,
    const std::string& provider_version,
    const logonomic::cli_addon::Json_value& mcp_servers,
    const logonomic::cli_addon::Json_value& attestations);

std::string acp_initial_input();

logonomic::cli_addon::Structured_inspect_result inspect_acp(
    const logonomic::cli_addon::Structured_inspection& inspection);

} // namespace opencode
