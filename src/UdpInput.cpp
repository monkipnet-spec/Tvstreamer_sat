#include "UdpInput.h"

#include <gio/gio.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <regex>
#include <sys/socket.h>
#include <unordered_set>
#include <vector>

#include "utils.h"

namespace {

constexpr gint kSocketBufferSize = 16 * 1024 * 1024;

bool isMulticastHost(const std::string& host) {
    static const std::regex pattern(R"(^((22[4-9])|(23[0-9]))\.)");
    return std::regex_search(host, pattern);
}

std::vector<std::string> multicastInterfaceNames(
    const std::string& configuredInterface,
    std::string& error) {
    std::vector<std::string> names;
    std::unordered_set<std::string> added;
    for (const auto& iface : enumerateNetworkInterfaces(true)) {
        if (!configuredInterface.empty() &&
            iface.address != configuredInterface && iface.name != configuredInterface) {
            continue;
        }

        // Linux loopback normally does not advertise IFF_MULTICAST, but IPv4
        // multicast membership by interface index is supported on lo and is
        // useful for local producers feeding TVStreammerSAT5 without leaving
        // the host. Allow lo/127.0.0.0/8 as an input-only multicast interface.
        const bool loopbackInterface =
            iface.name == "lo" || iface.address.rfind("127.", 0) == 0;
        if (iface.name.empty() || !iface.isUp ||
            (!iface.supportsMulticast && !loopbackInterface)) {
            if (!configuredInterface.empty()) {
                error = "selected multicast input interface is down or does not support multicast: " +
                    configuredInterface;
                return {};
            }
            continue;
        }
        if (added.insert(iface.name).second) {
            names.push_back(iface.name);
        }
    }

    if (!configuredInterface.empty() && names.empty() && error.empty()) {
        error = "selected multicast input interface was not found: " + configuredInterface;
    }
    return names;
}

std::string joinedInterfaceNames(const std::vector<std::string>& interfaces) {
    std::string result;
    for (const auto& iface : interfaces) {
        if (!result.empty()) {
            result += ',';
        }
        result += iface;
    }
    return result;
}

bool parseIpv4(const std::string& text, in_addr& address) {
    return !text.empty() && inet_pton(AF_INET, text.c_str(), &address) == 1;
}

std::string interfaceIpv4Address(const std::string& interfaceName) {
    for (const auto& iface : enumerateNetworkInterfaces(true)) {
        if (iface.name == interfaceName) {
            return iface.address;
        }
    }
    return "";
}

bool joinSourceSpecificMulticast(
    GSocket* socket,
    const std::string& group,
    const std::string& source,
    const std::string& interfaceName,
    std::string& error) {
#if defined(IP_ADD_SOURCE_MEMBERSHIP)
    ip_mreq_source request{};
    if (!parseIpv4(group, request.imr_multiaddr)) {
        error = "invalid IPv4 multicast group: " + group;
        return false;
    }
    if (!parseIpv4(source, request.imr_sourceaddr)) {
        error = "invalid IPv4 multicast source address: " + source;
        return false;
    }

    const std::string interfaceAddress = interfaceName.empty()
        ? std::string()
        : interfaceIpv4Address(interfaceName);
    if (!interfaceName.empty() && interfaceAddress.empty()) {
        error = "failed to resolve IPv4 address for multicast interface: " + interfaceName;
        return false;
    }
    if (interfaceAddress.empty()) {
        request.imr_interface.s_addr = htonl(INADDR_ANY);
    } else if (!parseIpv4(interfaceAddress, request.imr_interface)) {
        error = "invalid IPv4 address for multicast interface " + interfaceName + ": " + interfaceAddress;
        return false;
    }

    const int fd = g_socket_get_fd(socket);
    if (fd < 0 || setsockopt(
            fd,
            IPPROTO_IP,
            IP_ADD_SOURCE_MEMBERSHIP,
            &request,
            sizeof(request)) != 0) {
        error = "failed to join source-specific multicast " + source + " -> " + group;
        if (!interfaceName.empty()) {
            error += " on " + interfaceName;
        }
        error += ": ";
        error += std::strerror(errno);
        return false;
    }
    return true;
#else
    (void)socket;
    (void)group;
    (void)source;
    (void)interfaceName;
    error = "source-specific multicast is not supported by this platform";
    return false;
#endif
}

std::string gErrorMessage(const std::string& prefix, GError* error) {
    std::string result = prefix;
    if (error && error->message) {
        result += ": ";
        result += error->message;
    }
    if (error) {
        g_error_free(error);
    }
    return result;
}

GSocket* createMulticastSocket(
    const std::string& group,
    int port,
    const std::string& configuredInterface,
    const std::string& configuredSource,
    std::vector<std::string>& joinedInterfaces,
    std::string& error) {
    GError* socketError = nullptr;
    GSocket* socket = g_socket_new(
        G_SOCKET_FAMILY_IPV4,
        G_SOCKET_TYPE_DATAGRAM,
        G_SOCKET_PROTOCOL_UDP,
        &socketError);
    if (!socket) {
        error = gErrorMessage("failed to create multicast input socket", socketError);
        return nullptr;
    }

    // Bind to the wildcard address, as multicast receivers normally do. The
    // group and the selected VLAN are applied separately below. This avoids
    // relying on udpsrc's platform-dependent multicast bind behaviour.
    GInetAddress* anyAddress = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
    GSocketAddress* bindAddress = anyAddress
        ? g_inet_socket_address_new(anyAddress, static_cast<guint16>(port))
        : nullptr;
    if (anyAddress) {
        g_object_unref(anyAddress);
    }
    if (!bindAddress) {
        g_object_unref(socket);
        error = "failed to create multicast wildcard bind address";
        return nullptr;
    }

    if (!g_socket_bind(socket, bindAddress, TRUE, &socketError)) {
        g_object_unref(bindAddress);
        g_object_unref(socket);
        error = gErrorMessage(
            "failed to bind multicast input socket to 0.0.0.0:" + std::to_string(port),
            socketError);
        return nullptr;
    }
    g_object_unref(bindAddress);

    GInetAddress* groupAddress = g_inet_address_new_from_string(group.c_str());
    if (!groupAddress || !g_inet_address_get_is_multicast(groupAddress)) {
        if (groupAddress) {
            g_object_unref(groupAddress);
        }
        g_object_unref(socket);
        error = "invalid IPv4 multicast group: " + group;
        return nullptr;
    }

    std::string interfaceError;
    const auto interfaces = multicastInterfaceNames(configuredInterface, interfaceError);
    if (!interfaceError.empty()) {
        g_object_unref(groupAddress);
        g_object_unref(socket);
        error = interfaceError;
        return nullptr;
    }

    const std::string sourceAddress = normalizeIpAddress(configuredSource);
    if (!sourceAddress.empty()) {
        in_addr parsedSource{};
        if (!parseIpv4(sourceAddress, parsedSource)) {
            g_object_unref(groupAddress);
            g_object_unref(socket);
            error = "invalid IPv4 multicast source address: " + sourceAddress;
            return nullptr;
        }

        if (interfaces.empty()) {
            std::string joinError;
            if (!joinSourceSpecificMulticast(socket, group, sourceAddress, "", joinError)) {
                g_object_unref(groupAddress);
                g_object_unref(socket);
                error = joinError;
                return nullptr;
            }
            joinedInterfaces.push_back("route-default");
        } else {
            for (const auto& iface : interfaces) {
                std::string joinError;
                if (joinSourceSpecificMulticast(socket, group, sourceAddress, iface, joinError)) {
                    joinedInterfaces.push_back(iface);
                    continue;
                }
                if (!configuredInterface.empty()) {
                    g_object_unref(groupAddress);
                    g_object_unref(socket);
                    error = joinError;
                    return nullptr;
                }
                std::cerr << "UDP input warning: " << joinError << std::endl;
            }
            if (joinedInterfaces.empty()) {
                g_object_unref(groupAddress);
                g_object_unref(socket);
                error = "failed to join source-specific multicast " + sourceAddress +
                    " -> " + group + " on any active interface";
                return nullptr;
            }
        }
    } else if (interfaces.empty()) {
        if (!g_socket_join_multicast_group(socket, groupAddress, FALSE, nullptr, &socketError)) {
            g_object_unref(groupAddress);
            g_object_unref(socket);
            error = gErrorMessage("failed to join multicast group " + group, socketError);
            return nullptr;
        }
        joinedInterfaces.push_back("route-default");
    } else {
        for (const auto& iface : interfaces) {
            socketError = nullptr;
            if (g_socket_join_multicast_group(
                    socket, groupAddress, FALSE, iface.c_str(), &socketError)) {
                joinedInterfaces.push_back(iface);
                continue;
            }

            const std::string joinError = gErrorMessage(
                "failed to join multicast group " + group + " on " + iface,
                socketError);
            if (!configuredInterface.empty()) {
                g_object_unref(groupAddress);
                g_object_unref(socket);
                error = joinError;
                return nullptr;
            }
            std::cerr << "UDP input warning: " << joinError << std::endl;
        }
        if (joinedInterfaces.empty()) {
            g_object_unref(groupAddress);
            g_object_unref(socket);
            error = "failed to join multicast group " + group + " on any active interface";
            return nullptr;
        }
    }

    g_object_unref(groupAddress);

    // Request the receive buffer before handing the socket to udpsrc. Failure
    // is non-fatal because Linux may clamp it to net.core.rmem_max.
    socketError = nullptr;
    if (!g_socket_set_option(
            socket, SOL_SOCKET, SO_RCVBUF, kSocketBufferSize, &socketError)) {
        std::cerr << "UDP input warning: "
                  << gErrorMessage("failed to set multicast receive buffer", socketError)
                  << std::endl;
    }
    return socket;
}

std::string effectiveInputInterfaceAddress(
    const StreamConfig& config,
    bool multicastInput,
    bool wildcardUriHost) {
    if (config.inputInterfaceAddressConfigured) {
        return config.inputInterfaceAddress;
    }

    // Configs created before input_interface_address existed used the output
    // interface for multicast and udp://@:port. An explicitly configured empty
    // input interface now means "listen on all interfaces" and must not fall
    // back. An explicit unicast URI is safely received on all local addresses.
    return (multicastInput || wildcardUriHost) ? config.interfaceAddress : "";
}

void configureQueue(GstElement* queue) {
    // UDP is a live source.  Keeping several seconds of old datagrams after a
    // CPU overload is worse than dropping them: once the scheduler recovers the
    // old queue is replayed as a burst, sockets overflow and the decoder can
    // remain corrupted.  Keep at most 750 ms and discard the oldest buffers.
    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", static_cast<guint64>(750 * GST_MSECOND),
        "leaky", 2,
        nullptr);
}

} // namespace

namespace UdpInput {

bool handles(const std::string& uri) {
    const std::string lower = toLower(uri);
    return lower.rfind("udp://", 0) == 0 || lower.rfind("rtp://", 0) == 0;
}

GstElement* build(
    GstElement* pipeline,
    const StreamConfig& config,
    GstElement*& terminalElement,
    std::string& error) {
    terminalElement = nullptr;
    const std::string inputUri = normalizeInputUri(config.inputUri);
    std::regex uriPattern(R"(^(udp|rtp)://@?([^:/]*):(\d+).*$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(inputUri, match, uriPattern) || match.size() < 4) {
        error = "invalid UDP/RTP input URI";
        return nullptr;
    }

    GstElement* src = gst_element_factory_make("udpsrc", "input_src");
    GstElement* queue = gst_element_factory_make("queue", "input_queue");
    if (!src || !queue || !gst_bin_add(GST_BIN(pipeline), src) || !gst_bin_add(GST_BIN(pipeline), queue)) {
        error = "failed to create UDP input elements";
        return nullptr;
    }
    configureQueue(queue);

    int port = 0;
    try {
        port = std::stoi(match[3].str());
    } catch (...) {
        error = "invalid UDP/RTP input port";
        return nullptr;
    }
    if (port <= 0 || port > 65535) {
        error = "UDP/RTP input port is out of range";
        return nullptr;
    }
    const std::string uriHost = match[2].str();
    const bool multicastInput = isMulticastHost(uriHost);
    const bool wildcardUriHost = uriHost.empty() || uriHost == "0.0.0.0";
    const std::string inputInterfaceAddress =
        effectiveInputInterfaceAddress(config, multicastInput, wildcardUriHost);
    const std::string inputSourceAddress = normalizeIpAddress(config.inputSourceAddress);

    // Multicast uses a wildcard-bound socket with an explicit group membership
    // created above. A unicast URI host is commonly the sender/destination
    // address (FFmpeg and VLC syntax), and binding a receiving socket to that
    // remote address fails with EADDRNOTAVAIL. Bind unicast to a selected local
    // interface or to all local interfaces instead.
    const std::string listenAddress = multicastInput
        ? "0.0.0.0"
        : (inputInterfaceAddress.empty() ? "0.0.0.0" : inputInterfaceAddress);

    std::vector<std::string> joinedInterfaces;
    if (multicastInput) {
        GSocket* socket = createMulticastSocket(
            uriHost, port, inputInterfaceAddress, inputSourceAddress, joinedInterfaces, error);
        if (!socket) {
            return nullptr;
        }
        g_object_set(src,
            "address", uriHost.c_str(),
            "port", port,
            "socket", socket,
            "close-socket", TRUE,
            "reuse", TRUE,
            "auto-multicast", FALSE,
            "do-timestamp", FALSE,
            "buffer-size", kSocketBufferSize,
            nullptr);
        g_object_unref(socket);
    } else {
        g_object_set(src,
            "address", listenAddress.c_str(),
            "port", port,
            "reuse", TRUE,
            "auto-multicast", FALSE,
            "do-timestamp", FALSE,
            "buffer-size", kSocketBufferSize,
            nullptr);
    }

    std::cerr << "UDP input: protocol=" << toLower(match[1].str())
              << " uri_host=" << (uriHost.empty() ? "@" : uriHost)
              << " listen=" << listenAddress << ":" << port;
    if (multicastInput) {
        std::cerr << " multicast_join=" << uriHost
                  << " multicast_iface=" << joinedInterfaceNames(joinedInterfaces)
                  << " multicast_source=" << (inputSourceAddress.empty() ? "any" : inputSourceAddress)
                  << " membership=" << (inputSourceAddress.empty() ? "ASM" : "SSM");
        if (std::find(joinedInterfaces.begin(), joinedInterfaces.end(), "lo") != joinedInterfaces.end()) {
            std::cerr << " loopback_multicast=on";
        }
    } else if (!inputSourceAddress.empty()) {
        std::cerr << " source_filter_ignored=non-multicast:" << inputSourceAddress;
    }
    std::cerr << " live_queue_ms=750 leaky=downstream" << std::endl;

    if (toLower(match[1].str()) == "rtp") {
        GstElement* depay = gst_element_factory_make("rtpmp2tdepay", "rtp_depay");
        if (!depay || !gst_bin_add(GST_BIN(pipeline), depay)) {
            error = "failed to create RTP depayloader";
            return nullptr;
        }

        GstCaps* caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=MP2T,clock-rate=90000");
        g_object_set(src, "caps", caps, nullptr);
        gst_caps_unref(caps);
        if (!gst_element_link_many(src, depay, queue, nullptr)) {
            error = "failed to link RTP input";
            return nullptr;
        }
    } else {
        // A UDP datagram commonly contains seven TS packets. Do not force a
        // buffer packet size or replace the transport stream's PCR/PTS clock.
        GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true");
        g_object_set(src, "caps", caps, nullptr);
        gst_caps_unref(caps);
        if (!gst_element_link(src, queue)) {
            error = "failed to link UDP input";
            return nullptr;
        }
    }

    terminalElement = queue;
    return src;
}

} // namespace UdpInput
