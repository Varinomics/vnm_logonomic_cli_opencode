#include "opencode_conversation_test_support.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/managed_assets.h>
#include <logonomic_cli_addon/profile_storage.h>

#include <algorithm>
#include <iostream>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using opencode_test::Database;
using opencode_test::json;
using opencode_test::require;

void native_records(const fs::path& root)
{
    Database database(root / "records.db");
    database.execute("CREATE TABLE session_inbox (session_id TEXT, type TEXT, data TEXT)");
    database.execute("CREATE TABLE session_part (session_id TEXT, message_id TEXT, data TEXT)");
    database.execute("CREATE TABLE session_pending (session_id TEXT, type TEXT, data TEXT)");
    const std::string source = "v2|" + addon::path_to_utf8(database.path());
    const auto inspect = [&](const char* id) { return opencode::inspect_conversation(source, id, {}); };
    require(inspect("ses_absent").availability == addon::Resume_availability::ABSENT,
        "a readable native store establishes exact absence");
    database.session("ses_empty", true);
    require(inspect("ses_empty").content == addon::Resume_content::EMPTY,
        "complete native root with no messages, children or queued input is empty");
    database.tui_message("msg_user", "ses_empty", 1, "user", json(R"({"text":"late native user input"})"));
    require(inspect("ses_empty").content == addon::Resume_content::NONEMPTY,
        "fresh WAL inspection observes late writes after an earlier empty result");
    database.session("ses_metadata", true);
    database.tui_message("msg_meta", "ses_metadata", 1, "model-switched", json(R"({"model":{"providerID":"local","id":"fixture"}})"));
    require(inspect("ses_metadata").content == addon::Resume_content::EMPTY,
        "native selection metadata alone is not user or assistant conversation");
    database.session("ses_badmeta", true);
    database.tui_message("msg_badmeta", "ses_badmeta", 1, "model-switched", json(R"({"model":{},"text":"unexpected content"})"));
    require(inspect("ses_badmeta").content == addon::Resume_content::UNKNOWN,
        "a familiar record tag alone cannot prove an unrecognized payload empty");
    database.session("ses_blankuser", true);
    database.tui_message("msg_blank", "ses_blankuser", 1, "user", json(R"({"text":"","skills":[{}]})"));
    require(inspect("ses_blankuser").content == addon::Resume_content::UNKNOWN,
        "blank or partial user messages do not become metadata-only emptiness");
    database.session("ses_childonly", true);
    database.session("ses_child", true);
    database.execute("UPDATE session_v2 SET parent_id = 'ses_childonly' WHERE id = 'ses_child'");
    require(inspect("ses_childonly").content == addon::Resume_content::UNKNOWN &&
        inspect("ses_child").availability == addon::Resume_availability::UNKNOWN,
        "child-only history neither proves empty main history nor supplies a resumable root");
    database.session("ses_fork", true);
    database.execute("UPDATE session_v2 SET fork_session_id = 'ses_empty' WHERE id = 'ses_fork'");
    require(inspect("ses_fork").content == addon::Resume_content::UNKNOWN, "fork history remains recoverable");
    database.session("ses_revert", true, json(R"({"messageID":"msg_user"})"));
    require(inspect("ses_revert").content == addon::Resume_content::UNKNOWN, "revert history remains recoverable");
    database.session("ses_queued", true);
    database.execute("INSERT INTO session_inbox VALUES ('ses_queued', 'user', '{}')");
    require(inspect("ses_queued").content == addon::Resume_content::UNKNOWN, "unconsumed input is not empty history");
    database.session("ses_partial", true);
    database.execute("INSERT INTO session_message VALUES ('msg_partial', 'ses_partial', 1, 'user', '{')");
    require(inspect("ses_partial").content == addon::Resume_content::UNKNOWN, "partial native content is unknown");
    database.session("ses_future", true);
    database.tui_message("msg_future", "ses_future", 1, "future-record", json("{}"));
    require(inspect("ses_future").content == addon::Resume_content::UNKNOWN, "unsupported native records cannot prove empty");
    database.tui_message("msg_future_user", "ses_future", 2, "user", json(R"({"text":"known main conversation after an unsupported preamble"})"));
    require(inspect("ses_future").content == addon::Resume_content::NONEMPTY,
        "known main user content proves nonempty even when other native records cannot establish emptiness");
    database.session("ses_parts", true);
    database.execute("INSERT INTO session_part VALUES ('ses_parts', 'msg_detached', '{}')");
    require(inspect("ses_parts").content == addon::Resume_content::UNKNOWN, "detached newer-format assistant parts are retained");
    database.session("ses_pending", true);
    database.execute("INSERT INTO session_pending VALUES ('ses_pending', 'user', '{}')");
    require(inspect("ses_pending").content == addon::Resume_content::UNKNOWN, "older-format pending input is retained");
    database.execute("DROP TABLE session_pending");
    require(inspect("ses_metadata").content == addon::Resume_content::EMPTY,
        "the current native layout does not require a removed pending table");

    const std::string legacy = "v1|" + addon::path_to_utf8(database.path());
    database.session("ses_legacy", false);
    require(opencode::inspect_conversation(legacy, "ses_legacy", {}).content == addon::Resume_content::EMPTY,
        "a complete empty legacy root remains natively resumable");
    database.legacy_message("msg_legacy", "ses_legacy", 1, json(R"({"role":"user"})"));
    require(opencode::inspect_conversation(legacy, "ses_legacy", {}).content == addon::Resume_content::UNKNOWN,
        "a message whose parts have not arrived is not proven empty");
    database.legacy_part("prt_legacy", "msg_legacy", "ses_legacy", json(R"({"type":"text","text":"retained user turn"})"));
    require(opencode::inspect_conversation(legacy, "ses_legacy", {}).content == addon::Resume_content::NONEMPTY,
        "committed legacy native user parts establish meaningful main content");
    database.session("ses_legacy", true);
    require(opencode::inspect_conversation(legacy, "ses_legacy", {}).availability == addon::Resume_availability::UNKNOWN,
        "a migrated root does not silently fall back to stale legacy storage");
    require(opencode::inspect_conversation("v2|" + addon::path_to_utf8(root / "missing.db"), "ses_empty", {}).availability ==
        addon::Resume_availability::UNKNOWN, "an unavailable store is not proof of exact absence");
    require(opencode::inspect_conversation(source, "ses_empty", [] { return true; }).content == addon::Resume_content::UNKNOWN,
        "cancelled inspection cannot establish emptiness");
}

void packaged_inspection(const fs::path& package, const fs::path& root, const fs::path& executable)
{
    const fs::path program = root /
#ifdef _WIN32
        "opencode.exe";
#else
        "opencode";
#endif
    fs::copy_file(executable, program);
    Database first(root / "account-a.db");
    first.execute("CREATE TABLE session_inbox (session_id TEXT)");
    first.session("ses_exact", true);
    Database second(root / "account-b.db");
    second.execute("CREATE TABLE session_inbox (session_id TEXT)");
    second.session("ses_exact", true);
    second.tui_message("msg_user", "ses_exact", 1, "user", json(R"({"text":"other account history"})"));

    addon::Simulator_options options;
    options.package_directory = package;
    options.scratch_directory = root / "adapter";
    options.profile_storage_root = root / "profiles";
    addon::Host_simulator host(std::move(options), nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), "packaged adapter starts with all resume versions");
    const addon::Json_value* declaration = nullptr;
    for (const auto& sidecar : host.package()->sidecars) {
        if (sidecar.capability_id != "interactive-resume") continue;
        if (declaration) require(*declaration == sidecar.payload, "@2, @3 and @4 declarations retain the same environment contract");
        declaration = &sidecar.payload;
    }
    addon::Resume_inspect_request request;
    request.context.launcher = {"opencode", "direct", ""};
    request.context.launcher_argv = {addon::path_to_utf8(program)};
    request.context.working_directory = addon::path_to_utf8(root);
    request.context.launch_environment = addon::Launch_environment{};
    request.context.launch_environment->platform_id = addon::host_platform_id();
    request.context.launch_environment->effective_environment.set("PATH", addon::Json_value(addon::path_to_utf8(root)));
    request.context.launch_environment->effective_environment.set("OPENCODE_DB", addon::Json_value(addon::path_to_utf8(first.path())));
    request.resume_record = json(R"({"provider_session_id":"ses_exact"})");
    request.resume_record.set("source", addon::Json_value("v2|" + addon::path_to_utf8(first.path())));
    const auto inspect = [&](std::uint16_t version) {
        const auto id = host.invoke({"interactive-resume", version}, addon::k_inspect_resume_operation,
            addon::encode_resume_inspect_request(request), addon::limits::k_launch_prepare_deadline_ms);
        const auto outcome = host.wait_outcome(id, 18000);
        require(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged read-only inspection succeeds");
        if (version == 3) require(!outcome->payload.find("content"), "@3 wire result omits the @4 extension");
        const auto result = addon::decode_resume_inspect_result(outcome->payload, &failure, version);
        require(result && addon::validate_resume_inspection(request, *result, &failure), "inspection digest binds the complete supplied context");
        return *result;
    };
    require(inspect(4).content == addon::Resume_content::EMPTY, "packaged @4 confirms exact empty root");
    require(inspect(3).availability == addon::Resume_availability::AVAILABLE, "packaged @3 retains availability compatibility");
    request.context.launch_environment->effective_environment.set("OPENCODE_DB", addon::Json_value(addon::path_to_utf8(second.path())));
    require(inspect(4).availability == addon::Resume_availability::UNKNOWN, "same id in another store cannot substitute for frozen provenance");
    request.resume_record.set("source", addon::Json_value("v2|" + addon::path_to_utf8(second.path())));
    require(inspect(4).content == addon::Resume_content::NONEMPTY, "exact other-account context inspects only its own root");
    request.resume_record.erase("source");
    require(inspect(4).availability == addon::Resume_availability::UNKNOWN && inspect(4).content == addon::Resume_content::UNKNOWN,
        "legacy id-only records cannot establish which native store originally owned the id");
    request.context.launch_environment->effective_environment.set("OPENCODE_DB", addon::Json_value(addon::path_to_utf8(first.path())));
    require(inspect(4).content == addon::Resume_content::UNKNOWN,
        "an unbound legacy id cannot inherit emptiness from another currently selected store");
    request.resume_record.set("provider_session_id", addon::Json_value("ses_absent"));
    require(inspect(4).availability == addon::Resume_availability::UNKNOWN,
        "an unbound legacy id cannot inherit absence from another currently selected store");
    request.resume_record.set("source", addon::Json_value("v2|" + addon::path_to_utf8(first.path())));
    require(inspect(4).availability == addon::Resume_availability::ABSENT, "packaged inspection reports exact absence");

    addon::Resume_prepare_request resume;
    resume.title_instruction = "Keep the test session title tied to its task.";
    resume.context = request.context;
    resume.resume_record = request.resume_record;
    std::string error;
    const auto profile = addon::allocate_profile_storage_root(host.profile_storage_root(), "logonomic.cli.opencode", &error);
    require(profile.has_value(), "test profile allocated");
    resume.context.profile_storage = addon::Profile_storage_context{1, *profile};
    for (const auto& sidecar : host.package()->sidecars) {
        if (sidecar.capability_id == "managed-assets") {
            const auto assets = addon::decode_managed_assets_declaration(sidecar.payload, host.package()->index, &failure);
            require(assets.has_value(), "managed assets are valid");
            resume.context.declared_assets = assets->assets;
        }
    }
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = resume.context.profile_storage;
    const auto id = host.invoke({"interactive-resume", 2}, addon::k_prepare_resume_operation,
        addon::encode_resume_prepare_request(resume, 2), addon::limits::k_launch_prepare_deadline_ms, invocation);
    const auto outcome = host.wait_outcome(id, 18000);
    require(outcome && outcome->status == addon::Host_status::UNAVAILABLE && outcome->reason_code == "resume_target_absent",
        "explicit preparation rechecks fresh native absence before proposing a launch");
    resume.resume_record.erase("source");
    const auto legacy_id = host.invoke({"interactive-resume", 2}, addon::k_prepare_resume_operation,
        addon::encode_resume_prepare_request(resume, 2), addon::limits::k_launch_prepare_deadline_ms, invocation);
    const auto legacy_outcome = host.wait_outcome(legacy_id, 18000);
    require(legacy_outcome && legacy_outcome->status == addon::Host_status::SUCCESS,
        "legacy @2 resume remains available without pretending to know the original native store");
    host.shutdown(2000, nullptr);
    addon::Simulator_options older;
    older.package_directory = package;
    older.scratch_directory = root / "older-host";
    older.adopted_capabilities = std::vector<addon::Capability_pair>{{"interactive-resume", 2}};
    addon::Host_simulator old_host(std::move(older), nullptr);
    require(old_host.start(&failure), "an older host can negotiate the unchanged @2 capability alone");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "opencode v2.2.6\n";
        return 0;
    }
    const auto root = fs::temp_directory_path() / ("opencode-resume-" + addon::random_nonce_hex());
    try {
        require(argc == 2, "expected packaged adapter directory");
        fs::create_directories(root);
        native_records(root);
        packaged_inspection(addon::path_from_utf8(argv[1]), root, fs::absolute(addon::path_from_utf8(argv[0])));
        fs::remove_all(root);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "OpenCode resume: " << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
