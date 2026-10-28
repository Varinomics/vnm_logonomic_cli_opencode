#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/managed_assets.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int prepare(int argc, char** argv)
{
    require(argc == 4, "expected package directory, fixture directory and prepare request file");
    std::ifstream input(addon::path_from_utf8(argv[3]), std::ios::binary);
    require(input.good(), "cannot read native prepare request");
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto payload = addon::parse_json(bytes, {}, nullptr);
    require(payload.has_value(), "native prepare request is not valid JSON");
    addon::Contract_failure failure;
    auto request = addon::decode_prepare_request(*payload, &failure, 3);
    require(request.has_value(), "native prepare request does not conform to launch-preparation@3");

    const fs::path root = addon::path_from_utf8(argv[2]);
    addon::Simulator_options options;
    options.package_directory    = addon::path_from_utf8(argv[1]);
    options.scratch_directory    = root / "adapter-scratch";
    options.profile_storage_root = root / "profiles";
    addon::Host_simulator host(std::move(options), nullptr);
    const bool started = host.start(&failure);
    require(started, "packaged adapter did not start: " + failure.reason_code);

    bool assets_found = false;
    for (const auto& sidecar : host.package()->sidecars) {
        if (sidecar.capability_id != "managed-assets" || sidecar.version != 1) {
            continue;
        }
        const auto assets = addon::decode_managed_assets_declaration(sidecar.payload, host.package()->index, &failure);
        require(assets.has_value(), "packaged managed assets declaration is invalid");
        request->declared_assets = assets->assets;
        assets_found = true;
        break;
    }
    require(assets_found, "packaged adapter has no managed assets declaration");
    std::string error;
    const auto profile = addon::allocate_profile_storage_root(host.profile_storage_root(), "logonomic.cli.opencode", &error);
    require(profile.has_value(), "cannot allocate native fixture profile: " + error);
    request->profile_storage = addon::Profile_storage_context{1, *profile};
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request->profile_storage;
    const auto id = host.invoke({"launch-preparation", 3}, "prepare",
        addon::encode_prepare_request(*request, 3), addon::limits::k_launch_prepare_deadline_ms, invocation);
    const auto outcome = host.wait_outcome(id, 18000);
    require(outcome.has_value(), "packaged native preparation did not complete");
    require(outcome->status == addon::Host_status::SUCCESS,
        "packaged native preparation failed: " + outcome->reason_code + " " + outcome->message);
    const auto patch = addon::decode_launch_patch(outcome->payload, &failure, 3);
    require(patch.has_value(), "packaged adapter returned an invalid launch patch");
    std::cout << addon::serialize_json(addon::encode_launch_patch(*patch, 3)) << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return prepare(argc, argv);
    }
    catch (const std::exception& error) {
        std::cerr << "Native preparation fixture: " << error.what() << '\n';
        return 1;
    }
}
