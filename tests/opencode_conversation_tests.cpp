#include "opencode_conversation_test_support.h"

#include <logonomic_cli_addon/ids.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace opencode_test {
namespace {

using Json = addon::Json_value;
using Format = addon::Conversation_export_format;

Json user(std::int64_t created = 1000)
{
    auto message = json(R"({"role":"user","time":{"created":1000},"agent":"build",
        "model":{"providerID":"fixture-provider","modelID":"fixture-model"}})");
    message.find("time")->set("created", Json(created));
    return message;
}

Json assistant(bool completed = true, std::int64_t created = 1100)
{
    auto message = json(R"({"role":"assistant","time":{"created":1100,"completed":1200},
        "parentID":"msg_user","modelID":"fixture-model","providerID":"fixture-provider",
        "mode":"build","agent":"build","path":{"cwd":"/workspace/project","root":"/workspace/project"},
        "cost":0,"tokens":{"input":100,"output":20,"reasoning":5,"cache":{"read":0,"write":0}},
        "finish":"stop"})");
    message.find("time")->set("created", Json(created));
    message.find("time")->set("completed", Json(created + 100));
    if (!completed) {
        message.find("time")->erase("completed");
        message.erase("finish");
    }
    return message;
}

Json text(std::string_view value)
{
    auto part = json(R"({"type":"text","text":""})");
    part.set("text", Json(value));
    return part;
}

void succeeded(const Export& exported)
{
    require(exported.result.has_value(), "Native conversation export failed: " + exported.failure.reason_code);
    require(exported.result->bytes == exported.text.size(), "Descriptor must name the complete exported byte count.");
    require(exported.result->message_count > 0, "Successful discussion export must contain messages.");
    require(!exported.result->incomplete_tail_omitted, "Retained in-flight rows must not be reported as omitted.");
}

void includes(const std::string& document, std::string_view value, std::string_view message)
{
    require(document.find(value) != std::string::npos, message);
}

void excludes(const std::string& document, std::string_view value, std::string_view message)
{
    require(document.find(value) == std::string::npos, message);
}

void projections(const fs::path& root)
{
    // Oracle: native session-v1 User/Assistant and all twelve Part variants;
    // message-v2 hydrate keeps raw data, and native export passes those records through.
    Database database(root / "projections" / "opencode.db");
    const std::string session = "ses_projection";
    database.session(session, false);
    auto question = user();
    question.set("summary", json(R"({"title":"Loading-row investigation","body":"Keep the user's original question.",
        "diffs":[{"file":"src/repository.cpp","patch":"@@ -1 +1 @@\n-before\n+after",
        "additions":1,"deletions":1,"status":"modified"}]})"));
    database.legacy_message("msg_user", session, 1000, question);
    const std::string prompt = "Explain the loading repository row. Preserve `row.selected` and Unicode Δοκιμή.";
    database.legacy_part("prt_user_text", "msg_user", session, text(prompt));
    database.legacy_part("prt_user_file", "msg_user", session, json(R"({"type":"file","mime":"application/pdf",
        "filename":"display-notes.pdf","url":"","source":{"type":"file","path":"notes/repository-source.pdf",
        "text":{"value":"@repository-source.pdf","start":0,"end":22}}})"));
    database.legacy_part("prt_user_agent", "msg_user", session, json(R"({"type":"agent","name":"review",
        "source":{"value":"@review","start":0,"end":7}})"));
    database.legacy_message("msg_answer", session, 1100, assistant());
    database.legacy_part("prt_answer_text", "msg_answer", session, text("The original question remains visible."));
    database.legacy_part("prt_answer_reasoning", "msg_answer", session,
        json(R"({"type":"reasoning","text":"Compare the selected state with the loading state.",
            "time":{"start":1101,"end":1102}})"));
    database.legacy_part("prt_answer_subtask", "msg_answer", session, json(R"({"type":"subtask",
        "agent":"review","prompt":"Check click-to-deselect behavior.","description":"Review the repository row.",
        "command":"review-row","model":{"providerID":"review-provider","modelID":"review-model"}})"));
    database.legacy_part("prt_answer_snapshot", "msg_answer", session,
        json(R"({"type":"snapshot","snapshot":"repository-snapshot-reference"})"));
    database.legacy_part("prt_answer_patch", "msg_answer", session,
        json(R"({"type":"patch","hash":"repository-patch-reference","files":["src/repository.cpp"]})"));
    database.legacy_part("prt_answer_start", "msg_answer", session, json(R"({"type":"step-start"})"));
    database.legacy_part("prt_answer_finish", "msg_answer", session, json(R"({"type":"step-finish","reason":"stop",
        "cost":0,"tokens":{"input":100,"output":20,"reasoning":5,"cache":{"read":0,"write":0}}})"));
    database.legacy_part("prt_answer_retry", "msg_answer", session, json(R"({"type":"retry","attempt":1,
        "error":{"name":"APIError","data":{"message":"The fixture request can be retried.","isRetryable":true}},
        "time":{"created":1103}})"));
    database.legacy_part("prt_answer_compaction", "msg_answer", session,
        json(R"({"type":"compaction","auto":true,"overflow":false})"));
    database.legacy_part("prt_tool_pending", "msg_answer", session, json(R"({"type":"tool","tool":"read",
        "callID":"call_waiting","state":{"status":"pending","input":{},"raw":"{\"path\":\"src/"}})"));
    database.legacy_part("prt_tool_running", "msg_answer", session, json(R"({"type":"tool","tool":"read",
        "callID":"call_active","state":{"status":"running","input":{"path":"src/repository.cpp"},
        "title":"Read repository behavior","time":{"start":1104}}})"));
    auto tool = json(R"({"type":"tool","tool":"read","callID":"call_completed","state":{"status":"completed",
        "input":{"path":"src/repository.cpp"},"output":"","title":"Retained source evidence","metadata":{},
        "time":{"start":1105,"end":1106},"attachments":[{"id":"prt_tool_image","sessionID":"ses_projection",
        "messageID":"msg_answer","type":"file","mime":"image/png",
        "filename":"row.png","url":"data:image/png;base64,ZW1iZWRkZWQtcGF5bG9hZA=="}]}})");
    const std::string tool_output =
        "Complete tool output\n" + std::string(70000, 'x') + "\n````````````\nEnd tool output";
    tool.find("state")->set("output", Json(tool_output));
    database.legacy_part("prt_tool_completed", "msg_answer", session, tool);
    database.legacy_part("prt_tool_error", "msg_answer", session, json(R"({"type":"tool","tool":"read",
        "callID":"call_error","state":{"status":"error","input":{"path":"missing.cpp"},
        "error":"The requested fixture file is missing.","time":{"start":1107,"end":1108}}})"));
    auto summary = assistant(true, 1300);
    summary.set("summary", Json(true));
    database.legacy_message("msg_compaction_summary", session, 1300, summary);
    database.legacy_part("prt_summary_text", "msg_compaction_summary", session,
        text("Retained assistant compaction summary."));
    auto incomplete = assistant(false, 1400);
    incomplete.set("error",
        json(R"({"name":"MessageAbortedError","data":{"message":"The fixture response ended early."}})"));
    database.legacy_message("msg_incomplete", session, 1400, incomplete);
    database.legacy_part("prt_incomplete_text", "msg_incomplete", session, text("Retained partial assistant text."));

    for (const auto format : {Format::MARKDOWN, Format::TEXT, Format::CONVERSATION}) {
        const auto exported = extract(database.path(), false, session,
            root / "projections" / addon::conversation_export_format_name(format), format);
        succeeded(exported);
        require(exported.result->format == format, "Descriptor must preserve the requested format.");
        require(exported.result->file_name == addon::conversation_export_file_name(format),
            "Format owns its output filename.");
        require(exported.result->attachment_count == 2, "Both retained attachment references must be exported.");
        require(occurrences(exported.text, prompt) == 1,
            "User summary metadata cannot replace or duplicate original user parts.");
        includes(exported.text, "Loading-row investigation", "Object-valued user summary title must survive.");
        includes(exported.text, "Keep the user's original question.", "Object-valued user summary body must survive.");
        includes(exported.text, "repository.cpp", "Object-valued user summary diffs must survive.");
        includes(exported.text, "@@ -1 +1 @@", "The retained user-summary patch hunk must survive.");
        includes(exported.text, "-before", "The retained user-summary removed line must survive.");
        includes(exported.text, "+after", "The retained user-summary added line must survive.");
        require(exported.text.find(prompt) < exported.text.find("Loading-row investigation"),
            "User summary metadata must follow the original user parts.");
        includes(exported.text, "Retained assistant compaction summary.",
            "Boolean assistant summary must retain its source text.");
        includes(exported.text, "Compacted conversation summary", "Assistant summary must remain distinguishable.");
        require(occurrences(exported.text, "Compacted conversation summary") == 1,
            "Object-valued user summary cannot be labelled as assistant compaction.");
        includes(exported.text, "display-notes.pdf", "Empty attachment URL must retain its display filename.");
        includes(exported.text, "application/pdf", "Empty attachment URL must retain its media type.");
        includes(exported.text, "notes/repository-source.pdf",
            "Empty attachment URL must retain its explicit source path.");
        includes(exported.text, "review-provider", "Subtask model provider is a retained call argument.");
        includes(exported.text, "review-model", "Subtask model identity is a retained call argument.");
        includes(exported.text, tool_output, "Tool text exceeding IPC string bounds must remain complete.");
        includes(exported.text, "pending", "Pending tool state must remain explicit.");
        includes(exported.text, "running", "Running tool state must remain explicit.");
        includes(exported.text, "The requested fixture file is missing.",
            "Tool failures must retain their native error text.");
        includes(exported.text, "The fixture response ended early.",
            "Assistant failures must retain their native error text.");
        includes(exported.text, "Retained partial assistant text.", "In-flight assistant text must remain present.");
        includes(exported.text, "incomplete", "In-flight assistant state must be explicit.");
        excludes(exported.text, "ZW1iZWRkZWQtcGF5bG9hZA==", "Embedded attachment bytes must not enter the transcript.");
    }
    const auto compact = extract(database.path(), false, session,
        root / "projections" / "compact", Format::MARKDOWN, true);
    succeeded(compact);
    require(compact.result->compact, "Compact result must acknowledge its requested projection.");
    includes(compact.text, prompt, "Compact export retains the original question.");
    includes(compact.text, "The original question remains visible.",
        "Unclassified assistant discussion stays visible.");
    excludes(compact.text, "Loading-row investigation", "User change metadata is excluded from compact discussion.");
    excludes(compact.text, "Retained assistant compaction summary.",
        "Compact discussion excludes native compaction summaries.");
    excludes(compact.text, "Compare the selected state", "Compact discussion excludes explicit reasoning.");
    excludes(compact.text, tool_output, "Compact discussion excludes tool traffic.");
}

void order_and_identity(const fs::path& root)
{
    // Native page() sorts by time/id and hydrate() sorts each message's part IDs.
    Database database(root / "order" / "opencode.db");
    database.session("ses_order", false);
    database.session("ses_unrelated", false);
    database.session("ses_unrelated", true);
    database.legacy_message("msg_later", "ses_order", 2000, user(2000));
    database.legacy_part("prt_later", "msg_later", "ses_order", text("LATER_MESSAGE"));
    database.legacy_message("msg_b", "ses_order", 1000, user());
    database.legacy_part("prt_b", "msg_b", "ses_order", text("SAME_TIME_SECOND_MESSAGE"));
    database.legacy_message("msg_a", "ses_order", 1000, user());
    database.legacy_part("prt_second", "msg_a", "ses_order", text("SECOND_ORDERED_PART"));
    database.legacy_part("prt_first", "msg_a", "ses_order", text("FIRST_ORDERED_PART"));
    database.legacy_message("msg_foreign", "ses_unrelated", 900, user(900));
    database.legacy_part("prt_foreign", "msg_foreign", "ses_unrelated", text("UNRELATED_MESSAGE"));
    const auto exported = extract(database.path(), false, "ses_order", root / "order" / "export");
    succeeded(exported);
    require(exported.text.find("FIRST_ORDERED_PART") < exported.text.find("SECOND_ORDERED_PART") &&
        exported.text.find("SECOND_ORDERED_PART") < exported.text.find("SAME_TIME_SECOND_MESSAGE") &&
        exported.text.find("SAME_TIME_SECOND_MESSAGE") < exported.text.find("LATER_MESSAGE"),
        "Native chronological message and part order must survive out-of-order insertion.");
    excludes(exported.text, "UNRELATED_MESSAGE", "Exact native identity excludes neighboring sessions.");
    const auto absent = extract(database.path(), false, "ses_absent", root / "order" / "absent");
    require(!absent.result && absent.failure.reason_code == "conversation_not_found",
        "Missing exact identity cannot select another session.");
    database.session("ses_order", true);
    const auto migrated = extract(database.path(), false, "ses_order", root / "order" / "migrated");
    require(!migrated.result && migrated.failure.reason_code == "conversation_source_unavailable",
        "Same identity in the newer native generation cannot export an obsolete legacy projection.");
}

void revert_and_fork(const fs::path& root)
{
    // Oracle: legacy SessionRevert.cleanup retains strictly the pre-boundary prefix.
    Database database(root / "revert" / "opencode.db");
    const std::string session = "ses_revert";
    database.session(session, false);
    for (const auto id : {"msg_prefix", "msg_target", "msg_tail"}) {
        const std::int64_t created = id == std::string_view("msg_prefix") ? 1000 :
            id == std::string_view("msg_target") ? 1100 : 1200;
        database.legacy_message(id, session, created, user(created));
    }
    database.legacy_part("prt_prefix", "msg_prefix", session, text("REVERT_PREFIX"));
    database.legacy_part("prt_a", "msg_target", session, text("TARGET_PREFIX"));
    database.legacy_part("prt_b", "msg_target", session, text("TARGET_BOUNDARY"));
    database.legacy_part("prt_c", "msg_target", session, text("TARGET_SUFFIX"));
    database.legacy_part("prt_tail", "msg_tail", session, text("LATER_TURN"));
    database.session(session, false, json(R"({"messageID":"msg_target"})"));
    const auto message_revert = extract(database.path(), false, session, root / "revert" / "message");
    succeeded(message_revert);
    includes(message_revert.text, "REVERT_PREFIX", "Message revert retains earlier discussion.");
    excludes(message_revert.text, "TARGET_", "Message revert excludes its whole target message.");
    excludes(message_revert.text, "LATER_TURN", "Message revert excludes later turns.");
    database.session(session, false, json(R"({"messageID":"msg_target","partID":"prt_b"})"));
    const auto part_revert = extract(database.path(), false, session, root / "revert" / "part");
    succeeded(part_revert);
    includes(part_revert.text, "TARGET_PREFIX", "Part revert retains earlier parts in its target message.");
    excludes(part_revert.text, "TARGET_BOUNDARY", "Part revert excludes its boundary part.");
    excludes(part_revert.text, "TARGET_SUFFIX", "Part revert excludes later target parts.");
    excludes(part_revert.text, "LATER_TURN", "Part revert excludes later messages.");
    database.session(session, false, json(R"({"messageID":"msg_target","partID":"prt_missing"})"));
    const auto missing_part = extract(database.path(), false, session, root / "revert" / "missing-part");
    succeeded(missing_part);
    includes(missing_part.text, "TARGET_SUFFIX", "Native cleanup preserves target parts when the part is absent.");
    excludes(missing_part.text, "LATER_TURN", "Missing part still leaves later messages reverted.");
    database.session(session, false, json(R"({"messageID":"msg_missing"})"));
    const auto missing_message = extract(database.path(), false, session, root / "revert" / "missing-message");
    succeeded(missing_message);
    includes(missing_message.text, "LATER_TURN",
        "Native cleanup with an absent message leaves retained history intact.");

    database.session("ses_parent", false);
    database.session("ses_fork", false);
    database.session("ses_subagent", false);
    database.execute("UPDATE session SET parent_id = 'ses_fork' WHERE id = 'ses_subagent'");
    database.legacy_message("msg_parent", "ses_parent", 1000, user());
    database.legacy_part("prt_parent", "msg_parent", "ses_parent", text("COPIED_FORK_PREFIX"));
    database.legacy_message("msg_copied", "ses_fork", 1000, user());
    database.legacy_part("prt_copied", "msg_copied", "ses_fork", text("COPIED_FORK_PREFIX"));
    database.legacy_message("msg_child", "ses_subagent", 1100, user(1100));
    database.legacy_part("prt_child", "msg_child", "ses_subagent", text("SUBAGENT_CHILD_ONLY"));
    const auto fork = extract(database.path(), false, "ses_fork", root / "revert" / "fork");
    succeeded(fork);
    require(occurrences(fork.text, "COPIED_FORK_PREFIX") == 1, "A fork's copied prefix must be exported exactly once.");
    excludes(fork.text, "SUBAGENT_CHILD_ONLY", "Subagent parent links do not join another native conversation.");
}

void wal_and_failures(const fs::path& root)
{
    Database database(root / "wal" / "opencode.db");
    database.session("ses_wal", false);
    database.legacy_message("msg_wal", "ses_wal", 1000, user());
    database.legacy_part("prt_wal", "msg_wal", "ses_wal", text("COMMITTED_WAL_DISCUSSION"));
    const auto wal_path = fs::path(addon::path_to_utf8(database.path()) + "-wal");
    const auto database_before = read(database.path());
    const auto wal_before = read(wal_path);
    require(!wal_before.empty(), "Live fixture writer must retain committed WAL frames.");
    const auto exported = extract(database.path(), false, "ses_wal", root / "wal" / "export");
    succeeded(exported);
    includes(exported.text, "COMMITTED_WAL_DISCUSSION",
        "Read-only extraction must see committed WAL rows while writer remains open.");
    require(read(database.path()) == database_before && read(wal_path) == wal_before,
        "Read-only extraction cannot modify the source database or committed WAL bytes.");
    const auto cancelled = extract(database.path(), false, "ses_wal", root / "wal" / "cancelled",
        Format::MARKDOWN, false, [] { return true; });
    require(!cancelled.result && cancelled.failure.reason_code == "cancelled", "Cancellation must remain explicit.");
    require(fs::is_empty(cancelled.output), "Cancelled extraction cannot publish a partial document.");
    database.legacy_message("msg_unsupported", "ses_wal", 1100, user(1100));
    database.legacy_part("prt_unknown", "msg_unsupported", "ses_wal",
        json(R"({"type":"future-content","text":"UNSUPPORTED_PART"})"));
    const auto unknown = extract(database.path(), false, "ses_wal", root / "wal" / "unknown");
    require(!unknown.result && unknown.failure.reason_code == "conversation_content_unavailable",
        "Unknown native content must fail explicitly rather than disappear from a successful export.");
    require(fs::is_empty(unknown.output), "Failed extraction must remove its incomplete output.");
}

} // namespace
} // namespace opencode_test

int main()
{
    namespace test = opencode_test;
    const auto root = test::fs::temp_directory_path() / ("opencode-conversation-" + test::addon::random_nonce_hex());
    try {
        test::fs::create_directories(root);
        test::projections(root);
        test::order_and_identity(root);
        test::revert_and_fork(root);
        test::wal_and_failures(root);
        test::require(test::tui_conversation_tests(root), "TUI-native conversation regressions must pass.");
        test::fs::remove_all(root);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL OpenCode conversation: " << error.what() << '\n';
        std::error_code cleanup_error;
        test::fs::remove_all(root, cleanup_error);
        return 1;
    }
}
