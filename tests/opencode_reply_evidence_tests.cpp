#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/lifecycle_events.h>

#include <cstddef>
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

void require(bool condition, const char* detail)
{
    if (!condition) {
        throw std::runtime_error(detail);
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::size_t case_index = 0;
    try {
        require(argc == 3, "expected package directory and captured reply cases");
        const std::string package_text = argv[1];
        const std::string captured_text = argv[2];
        const fs::path package(std::u8string(package_text.begin(), package_text.end()));
        const fs::path captured(std::u8string(captured_text.begin(), captured_text.end()));
        std::ifstream input(captured, std::ios::binary);
        require(input.is_open(), "captured reply cases could not be opened");
        const std::string bytes(std::istreambuf_iterator<char>{input}, {});
        const auto cases = addon::parse_json(bytes, addon::Json_parse_limits{}, nullptr);
        require(cases && cases->is_array(), "captured reply cases must be an array");

        addon::Simulator_options options;
        options.package_directory = package;
        options.scratch_directory = captured.parent_path() / "reply-adapter";
        addon::Host_simulator host(std::move(options), nullptr);
        addon::Contract_failure failure;
        require(host.start(&failure), "packaged adapter could not start");
        for (const auto& item : cases->as_array()) {
            ++case_index;
            const auto* text = item.find("text");
            const auto* body = item.find("body");
            const auto* expected_hint = item.find("hint");
            require(text && text->is_string() && body && body->is_object() &&
                    expected_hint && expected_hint->is_boolean(),
                "captured reply case is malformed");
            const auto expected = addon::assistant_reply_evidence(text->as_string());
            const auto* actual = body->find("assistant_reply_evidence");
            require(expected.is_null() ? actual == nullptr : actual && *actual == expected,
                "native text-part stream disagrees with complete-text SDK evidence");

            addon::Attention_classification_request request;
            request.base.event.name = body->find("event")->as_string();
            request.base.event.body = *body;
            request.base.transition_nonce = std::string(32, 'd');
            request.base.ledger.current_epoch = 1;
            request.base.ledger.current_transition = addon::Ledger_transition{
                addon::Transition_kind::START, body->find("session_id")->as_string(), std::string(32, 'e')};
            const auto id = host.invoke(
                {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
                addon::k_classify_event_operation,
                addon::encode_attention_classification_request(request), 5000);
            const auto outcome = host.wait_outcome(id, 7000);
            require(outcome && outcome->status == addon::Host_status::SUCCESS,
                "packaged reply classification did not succeed");
            const auto result = addon::decode_attention_classification_result(outcome->payload, &failure);
            require(result && result->base.observations.size() == 1,
                "packaged classifier lost the normal root completion");
            const auto& observation = result->base.observations.front();
            const bool hint = result->attention_kind && result->attention_kind->kind == "conversational_hint";
            require(hint == expected_hint->as_boolean(), "packaged classifier returned the wrong reply hint");
            if (hint) {
                require(observation.attention_transition == addon::Attention_transition::REQUESTED &&
                        observation.attention_evidence == addon::Evidence::INFERRED &&
                        !observation.attention_request_id.empty(),
                    "a conversational hint must retain inferred evidence and its request identity");
            }
        }
        std::cout << "OpenCode reply evidence passed " << case_index << " captured completions\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL reply case " << case_index << ": " << error.what() << '\n';
        return 1;
    }
}
