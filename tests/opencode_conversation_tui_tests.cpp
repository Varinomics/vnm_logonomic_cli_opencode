#include "opencode_conversation_test_support.h"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>

namespace opencode_test {
namespace {

using Json = addon::Json_value;
using Format = addon::Conversation_export_format;

Json user(std::string text)
{
    auto message = json(R"({"time":{"created":1793172000000},"text":""})");
    message.set("text", Json(std::move(text)));
    return message;
}

Json assistant(std::string text)
{
    auto message = json(R"({
        "agent":"build","model":{"providerID":"example","id":"project-review"},
        "time":{"created":1793172000100,"completed":1793172000200},
        "finish":"stop","content":[]
    })");
    auto content = Json::array();
    auto block = Json::object();
    block.set("type", Json("text"));
    block.set("text", Json(std::move(text)));
    content.push_back(std::move(block));
    message.set("content", std::move(content));
    return message;
}

void projected_history(const fs::path& root)
{
    // Native oracle: 68bc26b7 packages/core/src/session/{sql,store,history}.ts.
    // seq orders the whole projected transcript; compaction bounds model context only.
    Database database(root / "projected-history.db");
    const std::string session = "ses_projectedhistory";
    database.session(session, true);
    const std::string question = "Review the retained caf\xC3\xA9 discussion and its code.\n"
        "## User heading\n```cpp\nrun();\n```";
    const std::string answer = "The final answer retains caf\xC3\xA9 and the original code.";
    database.tui_message("msg_Afinal", session, 30, "assistant", assistant(answer));
    database.tui_message("msg_Mcompaction", session, 25, "compaction", json(R"({
        "time":{"created":1793172000250},"status":"completed","reason":"auto",
        "summary":"Native compaction summary keeps earlier decisions."
    })"));
    database.tui_message("msg_Kprogress", session, 20, "assistant", json(R"({
        "agent":"build","model":{"providerID":"example","id":"project-review"},
        "time":{"created":1793172000200,"completed":1793172000240},"finish":"tool-calls",
        "content":[
            {"type":"reasoning","text":"Retained reasoning about the repository."},
            {"type":"text","text":"Progress text before the tool result."},
            {"type":"tool","id":"call_inspect","name":"inspect_repository",
             "time":{"created":1793172000201,"ran":1793172000202,"completed":1793172000230},
             "state":{"status":"completed","input":{"path":"src/project.cpp"},"content":[
                 {"type":"text","text":"Native tool result keeps the inspected source."},
                 {"type":"file","uri":"file:///workspace/result.txt","mime":"text/plain","name":"result.txt"},
                 {"type":"file","uri":"data:application/octet-stream;base64,dG9vbC1iaW5hcnktYnl0ZXM=",
                  "mime":"application/octet-stream","name":"tool-capture.bin"}
             ]}}
        ]
    })"));
    auto prompt = user(question);
    prompt.set("files", json(R"([
        {"data":"YXR0YWNobWVudC1iaW5hcnktYnl0ZXM=","mime":"application/octet-stream",
         "source":{"type":"inline"},"name":"capture.bin"},
        {"data":"","mime":"text/markdown","source":{"type":"uri","uri":"file:///workspace/notes.md"},
         "name":"notes.md","description":"Referenced project instructions."}
    ])"));
    prompt.set("agents", json(R"([{"name":"planner"}])"));
    prompt.set("skills", json(R"([
        {"id":"review-workspace","name":"Review workspace","text":"Selected skill context."}
    ])"));
    database.tui_message("msg_Zquestion", session, 10, "user", prompt);
    database.tui_message("msg_context1", session, 1, "system", json(R"({
        "time":{"created":1793171999991},"text":"Frozen system instructions.","description":"Repository context"
    })"));
    database.tui_message("msg_context2", session, 2, "synthetic", json(R"({
        "time":{"created":1793171999992},"text":"Frozen synthetic instructions.","description":"Context update"
    })"));
    database.tui_message("msg_context3", session, 3, "skill", json(R"({
        "time":{"created":1793171999993},"skill":"repository-review","name":"Repository review",
        "text":"Native skill instructions."
    })"));
    database.tui_message("msg_context4", session, 4, "agent-switched", json(R"({
        "time":{"created":1793171999994},"agent":"build","previous":"plan"
    })"));
    database.tui_message("msg_context5", session, 5, "model-switched", json(R"({
        "time":{"created":1793171999995},"model":{"providerID":"example","id":"project-review","variant":"careful"},
        "previous":{"providerID":"example","id":"previous-review"}
    })"));
    auto location = json(R"({"time":{"created":1793171999996}})");
    auto selected = Json::object();
    selected.set("directory", Json(addon::path_to_utf8(fs::absolute(root))));
    location.set("location", std::move(selected));
    auto previous = Json::object();
    previous.set("directory", Json(addon::path_to_utf8(fs::absolute(root / "previous"))));
    location.set("previous", std::move(previous));
    database.tui_message("msg_context6", session, 6, "location-switched", location);

    // Prompt.FileAttachment and Tool.Content retain references, not embedded data.
    for (const auto format : {Format::MARKDOWN, Format::TEXT, Format::CONVERSATION}) {
        const auto exported = extract(database.path(), true, session,
            root / "projected-formats" / addon::conversation_export_format_name(format), format);
        require(exported.result.has_value(), "Each full format must export projected native history.");
        require(exported.result->format == format && exported.result->message_count == 10,
            "Full extraction must include every selected native row.");
        const auto question_at = exported.text.find(question);
        const auto summary_at  = exported.text.find("Native compaction summary keeps earlier decisions.");
        const auto answer_at   = exported.text.find(answer);
        require(question_at != std::string::npos && summary_at != std::string::npos &&
                answer_at != std::string::npos && question_at < summary_at && summary_at < answer_at,
            "Projected seq order must survive different insertion order and message-ID order.");
        require(occurrences(exported.text, question) == 1 && occurrences(exported.text, answer) == 1,
            "Native participant content must be preserved once in each full format.");
        for (const auto* retained : {
                "Frozen system instructions.", "Frozen synthetic instructions.", "Native skill instructions.",
                "Retained reasoning about the repository.", "Progress text before the tool result.",
                "Native tool result keeps the inspected source.", "src/project.cpp",
                "file:///workspace/result.txt", "file:///workspace/notes.md", "Referenced project instructions."})
        {
            require(exported.text.find(retained) != std::string::npos,
                std::string("Full ") + addon::conversation_export_format_name(format) + " export lost: " + retained);
        }
        // Markdown escapes the underscore in this structured tool label;
        // plain and conversation text retain the literal native identifier.
        const std::string_view tool_name = format == Format::MARKDOWN ? "inspect\\_repository" : "inspect_repository";
        require(exported.text.find(tool_name) != std::string::npos,
            "The native tool identity must survive the requested format's label encoding.");
        require(exported.result->attachment_count == 4 &&
                exported.text.find("YXR0YWNobWVudC1iaW5hcnktYnl0ZXM=") == std::string::npos &&
                exported.text.find("dG9vbC1iaW5hcnktYnl0ZXM=") == std::string::npos &&
                exported.text.find("data:application/octet-stream") == std::string::npos,
            "Attachment markers must not copy stored binary payloads or data URLs.");
    }
    const auto compact = extract(database.path(), true, session, root / "projected-compact", Format::MARKDOWN, true);
    require(compact.result && compact.result->compact && compact.text.find(question) != std::string::npos &&
            compact.text.find(answer) != std::string::npos,
        "Compact projected export must preserve the user discussion and completed assistant answer.");
    for (const auto* internal : {
            "Frozen system instructions.", "Retained reasoning about the repository.",
            "Progress text before the tool result.", "Native tool result keeps the inspected source.",
            "Native compaction summary keeps earlier decisions."})
    {
        require(compact.text.find(internal) == std::string::npos,
            "Compact discussion projection must omit known internal and continuing content.");
    }
}

void fork_and_revert(const fs::path& root)
{
    // Native oracle: SessionProjector.projectFork copies settled prefix rows under
    // child IDs. RevertEvent.Committed deletes from the selected seq, ignoring partID.
    Database database(root / "projected-branches.db");
    database.session("ses_parentprojected", true);
    database.session("ses_forkprojected", true);
    database.execute("UPDATE session_v2 SET fork_session_id = 'ses_parentprojected', "
        "fork_boundary = '{\"type\":\"after\",\"messageID\":\"msg_parentprefix\"}' "
        "WHERE id = 'ses_forkprojected'");
    database.tui_message("msg_parentprefix", "ses_parentprojected", 1, "user", user("Shared copied fork prefix."));
    database.tui_message("msg_parentlater", "ses_parentprojected", 2, "assistant",
        assistant("Parent-only branch answer."));
    database.tui_message("msg_forkevent_1", "ses_forkprojected", 1, "user", user("Shared copied fork prefix."));
    database.tui_message("msg_forkanswer", "ses_forkprojected", 3, "assistant", assistant("Child-only branch answer."));
    const auto fork = extract(database.path(), true, "ses_forkprojected", root / "projected-fork");
    require(fork.result && fork.result->message_count == 2 &&
            occurrences(fork.text, "Shared copied fork prefix.") == 1 &&
            fork.text.find("Child-only branch answer.") != std::string::npos &&
            fork.text.find("Parent-only branch answer.") == std::string::npos,
        "A projected fork must export its own copied prefix once without following its parent.");

    database.session("ses_revertprojected", true,
        json(R"({"messageID":"msg_revertboundary","partID":"prt_legacyignored"})"));
    database.tui_message("msg_revertbefore", "ses_revertprojected", 10, "user", user("Kept question before revert."));
    database.tui_message("msg_revertanswer", "ses_revertprojected", 20, "assistant",
        assistant("Kept answer before revert."));
    database.tui_message("msg_revertboundary", "ses_revertprojected", 30, "user", user("Discarded boundary question."));
    database.tui_message("msg_revertafter", "ses_revertprojected", 40, "assistant",
        assistant("Discarded later answer."));
    const auto reverted = extract(database.path(), true, "ses_revertprojected", root / "projected-revert");
    require(reverted.result && reverted.result->message_count == 2 &&
            reverted.text.find("Kept question before revert.") != std::string::npos &&
            reverted.text.find("Kept answer before revert.") != std::string::npos &&
            reverted.text.find("Discarded boundary question.") == std::string::npos &&
            reverted.text.find("Discarded later answer.") == std::string::npos,
        "Projected staged revert must export only the seq prefix before its boundary.");
    database.session("ses_revertprojected", true, json(R"({"messageID":"msg_missingboundary"})"));
    const auto missing = extract(database.path(), true, "ses_revertprojected", root / "projected-missing-boundary");
    require(!missing.result && missing.failure.reason_code == "conversation_branch_unavailable" &&
            fs::is_empty(missing.output),
        "A missing projected revert boundary must refuse without publishing a partial discussion.");
}

void inflight_and_failures(const fs::path& root)
{
    // Native oracle: SessionMessage schemas retain unfinished rows. The native
    // transfer's isSettled filter is intentionally not a full retained discussion.
    Database database(root / "projected-inflight.db");
    database.session("ses_assistantinflight", true);
    database.tui_message("msg_assistantpartial", "ses_assistantinflight", 1, "assistant", json(R"({
        "agent":"build","model":{"providerID":"example","id":"project-review"},
        "time":{"created":1793172000300},"content":[
            {"type":"text","text":"Partial assistant body remains available."},
            {"type":"tool","id":"call_partial","name":"search_partial","time":{"created":1793172000301},
             "state":{"status":"streaming","input":"{\"query\":\"partial"}},
            {"type":"tool","id":"call_running","name":"inspect_pending",
             "time":{"created":1793172000302,"ran":1793172000303},
             "state":{"status":"running","input":{"path":"src/pending.cpp"},"metadata":{}}}
        ]
    })"));
    const auto assistant_export = extract(database.path(), true, "ses_assistantinflight", root / "assistant-inflight");
    require(assistant_export.result && !assistant_export.result->incomplete_tail_omitted &&
            assistant_export.text.find("Partial assistant body remains available.") != std::string::npos &&
            assistant_export.text.find("src/pending.cpp") != std::string::npos &&
            assistant_export.text.find("incomplete") != std::string::npos &&
            assistant_export.text.find("streaming") != std::string::npos &&
            assistant_export.text.find("running") != std::string::npos,
        "Unfinished assistant and tool records must remain with truthful state markers.");

    database.session("ses_shellinflight", true);
    database.tui_message("msg_shellpartial", "ses_shellinflight", 1, "shell", json(R"({
        "time":{"created":1793172000400},"shellID":"sh_capturepartial","command":"git status --short",
        "status":"running","output":{"output":" M src/pending.cpp\n","cursor":19,"size":37,"truncated":true}
    })"));
    const auto shell_export = extract(database.path(), true, "ses_shellinflight", root / "shell-inflight");
    require(shell_export.result && !shell_export.result->incomplete_tail_omitted &&
            shell_export.text.find("git status --short") != std::string::npos &&
            shell_export.text.find(" M src/pending.cpp\n") != std::string::npos &&
            shell_export.text.find("running") != std::string::npos &&
            shell_export.text.find("truncated") != std::string::npos,
        "An unfinished shell must preserve its captured page and identify its native truncation.");

    database.session("ses_compactioninflight", true);
    database.tui_message("msg_compactionpartial", "ses_compactioninflight", 1, "compaction", json(R"({
        "time":{"created":1793172000500},"status":"running","reason":"manual",
        "summary":"Partial compaction summary remains retained."
    })"));
    const auto compaction_export = extract(database.path(), true, "ses_compactioninflight",
        root / "compaction-inflight");
    require(compaction_export.result && !compaction_export.result->incomplete_tail_omitted &&
            compaction_export.text.find("Partial compaction summary remains retained.") != std::string::npos &&
            compaction_export.text.find("incomplete") != std::string::npos,
        "A running compaction must preserve its retained summary with an incomplete-state marker.");

    database.session("ses_projectedfailures", true);
    database.tui_message("msg_assistantfailure", "ses_projectedfailures", 1, "assistant", json(R"({
        "agent":"build","model":{"providerID":"example","id":"project-review"},
        "time":{"created":1793172000600,"completed":1793172000640},"finish":"error",
        "content":[{"type":"tool","id":"call_failed","name":"read_missing_file",
            "time":{"created":1793172000601,"ran":1793172000602,"completed":1793172000610},
            "state":{"status":"error","input":{"path":"src/missing.cpp"},
                "error":{"type":"file_error","message":"The requested file does not exist."},
                "content":[{"type":"text","text":"Retained failed-tool detail."}]}}],
        "error":{"type":"provider_error","message":"The remote provider rejected the request.","status":503,
            "response":{"body":"Retained provider response."},"retryAfterMs":1200},
        "retry":{"attempt":2,"at":1793172000620,
            "error":{"type":"rate_limit","message":"Retained retry failure."}}
    })"));
    database.tui_message("msg_shellfailure", "ses_projectedfailures", 2, "shell", json(R"({
        "time":{"created":1793172000700,"completed":1793172000710},"shellID":"sh_failedcapture",
        "command":"git show missing-object","status":"exited","exit":128,
        "output":{"output":"Retained shell failure output.","cursor":30,"size":30,"truncated":false}
    })"));
    database.tui_message("msg_compactionfailure", "ses_projectedfailures", 3, "compaction", json(R"({
        "time":{"created":1793172000800},"status":"failed","reason":"auto",
        "error":{"type":"compaction_error","message":"Retained compaction failure."}
    })"));
    database.tui_message("msg_idlefailure", "ses_projectedfailures", 4, "idle", json(R"({
        "time":{"created":1793172000900},"outcome":"failed",
        "error":{"type":"execution_error","message":"Retained idle failure."}
    })"));
    const auto failures = extract(database.path(), true, "ses_projectedfailures", root / "projected-failures");
    require(failures.result.has_value(), "Typed native failure records must remain exportable.");
    for (const auto* retained : {
            "The requested file does not exist.", "Retained failed-tool detail.",
            "The remote provider rejected the request.", "503", "Retained provider response.", "1200",
            "Retained retry failure.", "Retained shell failure output.", "128",
            "Retained compaction failure.", "Retained idle failure."})
    {
        require(failures.text.find(retained) != std::string::npos,
            "Full projected extraction must retain typed errors, responses and terminal outcomes.");
    }
}

void family_and_identity(const fs::path& root)
{
    // A captured v2 locator selects session_v2/session_message even when the
    // same native ID also exists in session/message/part after migration.
    Database database(root / "projected-family.db");
    const std::string session = "ses_coexistingfamilies";
    database.session(session, true);
    database.session(session, false);
    database.tui_message("msg_currentfamily", session, 1, "user", user("Current projected discussion."));
    const auto old_user = json(R"({"role":"user","time":{"created":1793171999000},"agent":"build",
        "model":{"providerID":"example","modelID":"project-review"}})");
    database.legacy_message("msg_previousfamily", session, 1793171999000LL, old_user);
    database.legacy_part("prt_previousfamily", "msg_previousfamily", session,
        json(R"({"type":"text","text":"Superseded native-family discussion."})"));
    const auto selected = extract(database.path(), true, session, root / "projected-selected-family");
    require(selected.result && selected.result->message_count == 1 &&
            selected.text.find("Current projected discussion.") != std::string::npos &&
            selected.text.find("Superseded native-family discussion.") == std::string::npos,
        "A v2 source must read only its frozen projected family.");
    database.session("ses_legacyonly", false);
    database.legacy_message("msg_legacyonly", "ses_legacyonly", 1793171999000LL, old_user);
    database.legacy_part("prt_legacyonly", "msg_legacyonly", "ses_legacyonly",
        json(R"({"type":"text","text":"Another generation cannot substitute for an absent projected session."})"));
    const auto absent = extract(database.path(), true, "ses_legacyonly", root / "projected-absent-identity");
    require(!absent.result && absent.failure.reason_code == "conversation_not_found" && fs::is_empty(absent.output),
        "A missing projected identity must not fall back to another family or another session.");
}

} // namespace

bool tui_conversation_tests(const fs::path& root)
{
    projected_history(root);
    fork_and_revert(root);
    inflight_and_failures(root);
    family_and_identity(root);
    return true;
}

} // namespace opencode_test
