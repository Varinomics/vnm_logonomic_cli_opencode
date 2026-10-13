#include "opencode_wake.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/session_wake.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace opencode {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t k_invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t k_invalid_socket = -1;
#endif

class Socket
{
public:
    Socket()
    {
#ifdef _WIN32
        WSADATA data{};
        m_initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!m_initialized) return;
#endif
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == k_invalid_socket) return;
#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket(m_socket, FIONBIO, &nonblocking) != 0) close();
#else
        if (m_socket >= FD_SETSIZE) { close(); return; }
        if (fcntl(m_socket, F_SETFL, O_NONBLOCK) < 0) close();
#endif
    }
    ~Socket()
    {
        close();
#ifdef _WIN32
        if (m_initialized) WSACleanup();
#endif
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    socket_t get() const { return m_socket; }

private:
    void close()
    {
        if (m_socket == k_invalid_socket) return;
#ifdef _WIN32
        closesocket(m_socket);
#else
        ::close(m_socket);
#endif
        m_socket = k_invalid_socket;
    }
    socket_t m_socket = k_invalid_socket;
#ifdef _WIN32
    bool m_initialized = false;
#endif
};

bool wait_socket(socket_t socket, bool writable, Clock::time_point deadline, addon::Invocation_context& context)
{
    while (!context.cancel_requested() && Clock::now() < deadline) {
        fd_set selected;
        FD_ZERO(&selected);
        FD_SET(socket, &selected);
        timeval timeout{0, 100000};
        const int ready = select(static_cast<int>(socket + 1), writable ? nullptr : &selected,
            writable ? &selected : nullptr, nullptr, &timeout);
        if (ready > 0) return true;
        if (ready < 0) return false;
    }
    return false;
}

bool hex_string(const addon::Json_value* value, std::size_t size)
{
    if (!value || !value->is_string() || value->as_string().size() != size) return false;
    return std::all_of(value->as_string().begin(), value->as_string().end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::optional<addon::Json_value> registration(const addon::Json_value& binding)
{
    const auto* file = binding.find("registration_file");
    if (!binding.is_object() || binding.as_object().size() != 1 || !file || !file->is_string()) return std::nullopt;
    const auto& text = file->as_string();
    const fs::path path(std::u8string(text.begin(), text.end()));
    if (!path.is_absolute()) return std::nullopt;
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(path, error)) || error || fs::file_size(path, error) > 2048 || error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    char buffer[2049];
    input.read(buffer, sizeof(buffer));
    const auto count = input.gcount();
    if (count <= 0 || count > 2048 || input.bad()) return std::nullopt;
    addon::Json_parse_error failure;
    auto value = addon::parse_json(std::string_view(buffer, static_cast<std::size_t>(count)), {}, &failure);
    const auto* version = value ? value->find("version") : nullptr;
    const auto* port = value ? value->find("port") : nullptr;
    if (!version || !version->is_integer() || version->as_integer() != 1 ||
        !port || !port->is_integer() || port->as_integer() < 1 || port->as_integer() > 65535 ||
        !hex_string(value->find("token"), 64) || !hex_string(value->find("instance_id"), 32)) return std::nullopt;
    return value;
}

struct Exchange_result
{
    std::optional<addon::Json_value> response;
    bool sent = false;
};

Exchange_result exchange(const addon::Session_wake_target& target, addon::Json_value request,
    addon::Invocation_context& context)
{
    Exchange_result result;
    const auto binding = registration(target.binding);
    if (!binding || context.cancel_requested()) return result;
    request.set("token", *binding->find("token"));
    request.set("instance_id", *binding->find("instance_id"));
    request.set("provider_session_id", addon::Json_value(target.provider_session_id));
    request.set("transition_nonce", addon::Json_value(target.correlation.transition_nonce));
    const std::string bytes = addon::serialize_json(request) + "\n";
    Socket socket;
    if (socket.get() == k_invalid_socket) return result;
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = htons(static_cast<std::uint16_t>(binding->find("port")->as_integer()));
    const auto deadline = Clock::now() + std::chrono::seconds(6);
    (void)connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (!wait_socket(socket.get(), true, deadline, context)) return result;
    int error = 0;
#ifdef _WIN32
    int size = sizeof(error);
#else
    socklen_t size = sizeof(error);
#endif
    if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0 || error) return result;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!wait_socket(socket.get(), true, deadline, context)) return result;
#ifdef _WIN32
        const int flags = 0;
#else
        const int flags = MSG_NOSIGNAL;
#endif
        const auto sent = send(socket.get(), bytes.data() + offset, static_cast<int>(bytes.size() - offset), flags);
        if (sent <= 0) return result;
        result.sent = true;
        offset += static_cast<std::size_t>(sent);
    }
    std::string response;
    while (response.size() <= 2048 && wait_socket(socket.get(), false, deadline, context)) {
        char buffer[2049];
        const auto received = recv(socket.get(), buffer, sizeof(buffer), 0);
        if (received <= 0) return result;
        response.append(buffer, static_cast<std::size_t>(received));
        if (response.size() > 2048) return result;
        const auto newline = response.find('\n');
        if (newline == std::string::npos) continue;
        if (newline != response.size() - 1) return result;
        addon::Json_parse_error failure;
        result.response = addon::parse_json(response, {}, &failure);
        return result;
    }
    return result;
}

} // namespace

void handle_wake(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto& request = context.request();
    auto bridge_request = addon::Json_value::object();
    bridge_request.set("operation", addon::Json_value(request.operation));
    if (request.operation == addon::k_session_wake_inspect_operation) {
        const auto decoded = addon::decode_session_wake_inspect_request(request.payload, &failure);
        if (!decoded) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
        const auto result = exchange(decoded->target, std::move(bridge_request), context);
        const auto readiness = result.response ? addon::decode_session_wake_readiness(*result.response, &failure) : std::nullopt;
        context.succeed(addon::encode_session_wake_readiness(readiness.value_or(
            addon::Session_wake_readiness{false, "opencode_live_bridge_unavailable"})));
        return;
    }
    if (request.operation == addon::k_session_wake_notify_operation) {
        const auto decoded = addon::decode_session_wake_notify_request(request.payload, &failure);
        if (!decoded) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
        bridge_request.set("notification_id", addon::Json_value(decoded->notification_id));
        bridge_request.set("instruction", addon::Json_value(decoded->instruction));
        const auto result = exchange(decoded->target, std::move(bridge_request), context);
        const auto notified = result.response ? addon::decode_session_wake_notify_result(*result.response, &failure) : std::nullopt;
        addon::Session_wake_notify_result unavailable;
        unavailable.disposition = result.sent ? addon::Session_wake_disposition::UNCERTAIN : addon::Session_wake_disposition::UNAVAILABLE;
        unavailable.reason = result.sent ? "provider_acceptance_unknown" : "opencode_live_bridge_unavailable";
        context.succeed(addon::encode_session_wake_notify_result(notified.value_or(unavailable)));
        return;
    }
    context.reject_unknown_operation();
}

} // namespace opencode
