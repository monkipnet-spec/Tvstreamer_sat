#pragma once

#include <boost/asio.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class NewcamdClient {
public:
    NewcamdClient(const std::string& host, int port, const std::string& user, const std::string& pass, const std::string& des);
    ~NewcamdClient();

    bool connect();
    bool login();
    void disconnect();
    void start_receiver();
    void set_key_update_callback(std::function<void(const uint8_t*)> cb) { callback_ = cb; }

private:
    void receiver_loop();

    std::string host_, user_, pass_, des_;
    int port_;
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
    std::thread receiver_thread_;
    std::function<void(const uint8_t*)> callback_;
    std::atomic<bool> running_{false};
};
