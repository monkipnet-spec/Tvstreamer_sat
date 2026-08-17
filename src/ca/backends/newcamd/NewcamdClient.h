#pragma once

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class NewcamdClient {
public:
    using KeyUpdateCallback = std::function<void(uint16_t, uint8_t, const uint8_t*)>;

    NewcamdClient(const std::string& host, int port, const std::string& user, const std::string& pass, const std::string& des);
    ~NewcamdClient();

    bool connect();
    bool login();
    bool send_ecm(uint16_t service_id, uint16_t caid, uint32_t provid, const std::vector<uint8_t>& ecm, uint16_t* message_id = nullptr);
    void disconnect();
    void start_receiver();
    void set_key_update_callback(KeyUpdateCallback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
    }
    bool authenticated() const { return authenticated_; }
    size_t pending_ecms() const { return pending_ecms_.load(std::memory_order_relaxed); }
    std::string last_error() const;

private:
    struct Message {
        uint16_t id = 0;
        std::vector<uint8_t> payload;
    };

    bool parse_des_key();
    bool derive_key_from_seed(const uint8_t* seed, size_t seed_size);
    std::string md5_crypt_password() const;
    bool send_message(std::vector<uint8_t> payload, uint16_t service_id, uint16_t caid, uint32_t provid, bool use_msg_id, uint16_t* message_id = nullptr);
    bool receive_message(Message& message, bool check_msg_id);
    void receiver_loop();
    void set_error(const std::string& error);

    std::string host_, user_, pass_, des_;
    int port_;
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
    std::thread receiver_thread_;
    mutable std::mutex mutex_;
    std::mutex write_mutex_;
    KeyUpdateCallback callback_;
    std::string last_error_;
    std::array<uint8_t, 14> base_des_key_{};
    std::array<uint8_t, 16> session_key_{};
    uint16_t msg_id_ = 0;
    bool des_key_ready_ = false;
    bool session_key_ready_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> authenticated_{false};
    std::atomic<size_t> pending_ecms_{0};
};