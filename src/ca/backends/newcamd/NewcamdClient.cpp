#include "NewcamdClient.h"

#include <arpa/inet.h>

#include <iostream>
#include <vector>

using boost::asio::ip::tcp;

NewcamdClient::NewcamdClient(const std::string& host, int port, const std::string& user, const std::string& pass, const std::string& des)
    : host_(host), port_(port), user_(user), pass_(pass), des_(des) {}

NewcamdClient::~NewcamdClient() {
    disconnect();
}

bool NewcamdClient::connect() {
    try {
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host_, std::to_string(port_));
        socket_ = std::make_unique<tcp::socket>(io_context_);
        boost::asio::connect(*socket_, endpoints);
        return true;
    } catch (...) {
        return false;
    }
}

bool NewcamdClient::login() {
    if (!socket_ || !socket_->is_open()) return false;

    std::vector<uint8_t> server_nonce(14);
    boost::asio::read(*socket_, boost::asio::buffer(server_nonce));
    // LOGIN64 handling is intentionally left minimal until the full Newcamd
    // handshake is implemented.
    return true;
}


void NewcamdClient::start_receiver() {
    if (running_) return;
    running_ = true;
    receiver_thread_ = std::thread(&NewcamdClient::receiver_loop, this);
}

void NewcamdClient::receiver_loop() {
    while (running_) {
        try {
            uint8_t header[2];
            if (boost::asio::read(*socket_, boost::asio::buffer(header, 2)) != 2) break;
            uint16_t len = (static_cast<uint16_t>(header[0]) << 8) | header[1];
            std::vector<uint8_t> msg(len);
            if (boost::asio::read(*socket_, boost::asio::buffer(msg)) != len) break;

            std::function<void(const uint8_t*)> callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                callback = callback_;
            }
            if (!msg.empty() && msg[0] == 0x81 && msg.size() >= 17 && callback) {
                callback(msg.data() + 1);
            }
        } catch (...) {
            break;
        }
    }
}

void NewcamdClient::disconnect() {
    running_ = false;
    if (socket_) {
        boost::system::error_code ignored;
        socket_->close(ignored);
    }
    if (receiver_thread_.joinable()) receiver_thread_.join();
}
