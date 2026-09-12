#include <logonomic_cli_addon/json.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

int main(int argc, char** argv)
{
    namespace addon = logonomic::cli_addon;
    if (argc != 4 || std::string_view(argv[1]) != "provider-event") {
        return 2;
    }
    if (std::string_view(argv[2]) == "acknowledge") {
        return 0;
    }
    if (std::string_view(argv[2]) != "deliver" || std::string_view(argv[3]) != "--args-stdin") {
        return 2;
    }
    const char* capture = std::getenv("OPENCODE_TEST_EVENT_CAPTURE");
    if (!capture) {
        return 2;
    }
    std::string bytes;
    char buffer[4096];
    while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount() > 0) {
        bytes.append(buffer, static_cast<std::size_t>(std::cin.gcount()));
        if (bytes.size() > 256 * 1024) {
            return 2;
        }
    }
    addon::Json_parse_error error;
    const auto value = addon::parse_json(bytes, addon::Json_parse_limits{}, &error);
    const auto* event = value ? value->find("event") : nullptr;
    const auto* body  = event ? event->find("body") : nullptr;
    if (!body || !body->is_object()) {
        return 2;
    }
    std::ofstream output(capture, std::ios::binary | std::ios::app);
    output << addon::serialize_json(*body) << '\n';
    output.close();
    if (!output) {
        return 1;
    }
    if (const char* requests = std::getenv("OPENCODE_TEST_REQUEST_CAPTURE")) {
        std::ofstream request_output(requests, std::ios::binary | std::ios::app);
        request_output << addon::serialize_json(*value) << '\n';
        request_output.close();
        if (!request_output) {
            return 1;
        }
    }
    const char* mode = std::getenv("OPENCODE_TEST_DELIVERY_MODE");
    if (mode && std::string_view(mode) == "wait-for-cancellation") {
        const char* pid_path = std::getenv("OPENCODE_TEST_WRAPPER_PID");
        if (!pid_path) {
            return 2;
        }
        std::ofstream pid_output(pid_path);
#if defined(_WIN32)
        pid_output << _getpid();
#else
        pid_output << getpid();
#endif
        pid_output.close();
        if (!pid_output) {
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 1;
    }
    if (mode && std::string_view(mode) == "retry") {
        std::ifstream captured(capture);
        std::size_t calls = 0;
        for (std::string line; std::getline(captured, line);) {
            ++calls;
        }
        if (calls == 1) {
            return 4;
        }
    }
    if (mode && std::string_view(mode) == "refuse") {
        std::cout << "{\"accepted\":false,\"reason\":\"retired_session\"}\n";
        return std::cout ? 0 : 1;
    }
    std::cout << "{\"accepted\":true,\"epoch\":1,\"helper_response\":{\"delivered\":true}}\n";
    return std::cout ? 0 : 1;
}
