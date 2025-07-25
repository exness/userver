#include "create_socket.hpp"

#include <string>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <userver/engine/io/socket.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/fs/blocking/write.hpp>
#include <userver/net/blocking/get_addr_info.hpp>

USERVER_NAMESPACE_BEGIN

namespace server::net {

namespace {

engine::io::Socket CreateUnixSocket(const std::string& path, int backlog, boost::filesystem::perms perms) {
    const auto addr = engine::io::Sockaddr::MakeUnixSocketAddress(path);

    /* Use blocking API here, it is not critical as CreateUnixSocket() is called
     * on startup only */

    if (fs::blocking::GetFileType(path) == boost::filesystem::file_type::socket_file)
        fs::blocking::RemoveSingleFile(path);

    engine::io::Socket socket{addr.Domain(), engine::io::SocketType::kStream};
    socket.Bind(addr);
    socket.Listen(backlog);

    fs::blocking::Chmod(path, perms);
    return socket;
}

engine::io::Socket CreateIpv6Socket(const std::string& address, uint16_t port, int backlog) {
    std::vector<engine::io::Sockaddr> addrs;

    try {
        addrs = USERVER_NAMESPACE::net::blocking::GetAddrInfo(address, std::to_string(port).c_str());
    } catch (const std::runtime_error&) {
        throw std::runtime_error(fmt::format("Address string '{}' is invalid", address));
    }

    UASSERT(!addrs.empty());

    if (addrs.size() > 1)
        throw std::runtime_error(fmt::format(
            "Address string '{}' designates multiple addresses, while only 1 "
            "address per listener is supported. The addresses are: {}\nYou can "
            "specify '::' as address to listen on all local addresses",
            fmt::join(addrs, ", "),
            address
        ));

    auto& addr = addrs.front();
    engine::io::Socket socket{addr.Domain(), engine::io::SocketType::kStream};
    socket.Bind(addr);
    socket.Listen(backlog);
    return socket;
}

engine::io::Socket DoCreateSocket(const ListenerConfig& config, const PortConfig& port_config) {
    if (port_config.unix_socket_path.empty())
        return CreateIpv6Socket(port_config.address, port_config.port, config.backlog);
    else
        return CreateUnixSocket(port_config.unix_socket_path, config.backlog, port_config.unix_socket_perms);
}

}  // namespace

engine::io::Socket CreateSocket(const ListenerConfig& config, const PortConfig& port_config) {
    // Note: socket creation accesses filesystem
    auto& tp = engine::current_task::GetBlockingTaskProcessor();
    return engine::AsyncNoSpan(tp, &DoCreateSocket, std::ref(config), std::ref(port_config)).Get();
}

}  // namespace server::net

USERVER_NAMESPACE_END
