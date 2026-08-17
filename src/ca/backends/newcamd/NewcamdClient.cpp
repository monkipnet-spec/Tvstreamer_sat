#include "NewcamdClient.h"

#include <openssl/des.h>
#if __has_include(<crypt.h>)
#include <crypt.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>
#include <vector>

using boost::asio::ip::tcp;

namespace {

constexpr uint8_t kMsgClientLogin = 0xE0;
constexpr uint8_t kMsgClientLoginAck = 0xE1;
constexpr uint8_t kMsgClientLoginNak = 0xE2;
constexpr uint8_t kMsgCardDataReq = 0xE3;
constexpr uint8_t kMsgCardData = 0xE4;
constexpr uint16_t kClientId = 0x8888;
constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxMessageSize = 2048;
constexpr size_t kMaxPendingEcms = 1;

uint8_t hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(10 + ch - 'a');
    if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(10 + ch - 'A');
    return 0xFF;
}

void spread_des_key(const std::array<uint8_t, 14>& compact, std::array<uint8_t, 16>& spread) {
    spread[0] = compact[0] & 0xFE;
    spread[1] = static_cast<uint8_t>(((compact[0] << 7) | (compact[1] >> 1)) & 0xFE);
    spread[2] = static_cast<uint8_t>(((compact[1] << 6) | (compact[2] >> 2)) & 0xFE);
    spread[3] = static_cast<uint8_t>(((compact[2] << 5) | (compact[3] >> 3)) & 0xFE);
    spread[4] = static_cast<uint8_t>(((compact[3] << 4) | (compact[4] >> 4)) & 0xFE);
    spread[5] = static_cast<uint8_t>(((compact[4] << 3) | (compact[5] >> 5)) & 0xFE);
    spread[6] = static_cast<uint8_t>(((compact[5] << 2) | (compact[6] >> 6)) & 0xFE);
    spread[7] = static_cast<uint8_t>(compact[6] << 1);
    spread[8] = compact[7] & 0xFE;
    spread[9] = static_cast<uint8_t>(((compact[7] << 7) | (compact[8] >> 1)) & 0xFE);
    spread[10] = static_cast<uint8_t>(((compact[8] << 6) | (compact[9] >> 2)) & 0xFE);
    spread[11] = static_cast<uint8_t>(((compact[9] << 5) | (compact[10] >> 3)) & 0xFE);
    spread[12] = static_cast<uint8_t>(((compact[10] << 4) | (compact[11] >> 4)) & 0xFE);
    spread[13] = static_cast<uint8_t>(((compact[11] << 3) | (compact[12] >> 5)) & 0xFE);
    spread[14] = static_cast<uint8_t>(((compact[12] << 2) | (compact[13] >> 6)) & 0xFE);
    spread[15] = static_cast<uint8_t>(compact[13] << 1);
    DES_set_odd_parity(reinterpret_cast<DES_cblock*>(spread.data()));
    DES_set_odd_parity(reinterpret_cast<DES_cblock*>(spread.data() + 8));
}

uint8_t xor_sum(const uint8_t* data, size_t size) {
    uint8_t value = 0;
    for (size_t i = 0; i < size; ++i) value ^= data[i];
    return value;
}

void random_bytes(uint8_t* data, size_t size) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(dist(rng));
}

bool make_schedules(const std::array<uint8_t, 16>& key, DES_key_schedule& ks1, DES_key_schedule& ks2) {
    DES_cblock k1{};
    DES_cblock k2{};
    std::memcpy(k1, key.data(), 8);
    std::memcpy(k2, key.data() + 8, 8);
    DES_set_key_unchecked(&k1, &ks1);
    DES_set_key_unchecked(&k2, &ks2);
    return true;
}

std::string command_name(uint8_t command) {
    switch (command) {
    case kMsgClientLoginAck: return "LOGIN_ACK";
    case kMsgClientLoginNak: return "LOGIN_NAK";
    case kMsgCardData: return "CARD_DATA";
    case 0x80: return "ECM_EVEN";
    case 0x81: return "ECM_ODD";
    default: {
        std::ostringstream out;
        out << "0x" << std::hex << static_cast<int>(command);
        return out.str();
    }
    }
}

} // namespace

NewcamdClient::NewcamdClient(const std::string& host, int port, const std::string& user, const std::string& pass, const std::string& des)
    : host_(host), port_(port), user_(user), pass_(pass), des_(des) {}

NewcamdClient::~NewcamdClient() {
    disconnect();
}

void NewcamdClient::set_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error;
}

std::string NewcamdClient::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool NewcamdClient::parse_des_key() {
    std::string hex;
    hex.reserve(des_.size());
    for (char ch : des_) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) hex.push_back(ch);
    }
    if (hex.size() != base_des_key_.size() * 2) {
        set_error("Newcamd DES key must contain exactly 14 bytes / 28 hex digits");
        return false;
    }
    for (size_t i = 0; i < base_des_key_.size(); ++i) {
        const uint8_t high = hex_value(hex[i * 2]);
        const uint8_t low = hex_value(hex[i * 2 + 1]);
        if (high == 0xFF || low == 0xFF) {
            set_error("Newcamd DES key contains a non-hex digit");
            return false;
        }
        base_des_key_[i] = static_cast<uint8_t>((high << 4) | low);
    }
    des_key_ready_ = true;
    return true;
}

bool NewcamdClient::derive_key_from_seed(const uint8_t* seed, size_t seed_size) {
    if (!des_key_ready_ || !seed || seed_size == 0) return false;
    std::array<uint8_t, 14> compact = base_des_key_;
    for (size_t i = 0; i < seed_size; ++i) compact[i % compact.size()] ^= seed[i];
    spread_des_key(compact, session_key_);
    session_key_ready_ = true;
    return true;
}

std::string NewcamdClient::md5_crypt_password() const {
    static std::mutex cryptMutex;
    std::lock_guard<std::mutex> lock(cryptMutex);
    char* value = crypt(pass_.c_str(), "$1$abcdefgh$");
    return value ? std::string(value) : std::string();
}

bool NewcamdClient::connect() {
    try {
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host_, std::to_string(port_));
        socket_ = std::make_unique<tcp::socket>(io_context_);
        boost::asio::connect(*socket_, endpoints);
        set_error({});
        return true;
    } catch (const std::exception& ex) {
        set_error(ex.what());
        return false;
    } catch (...) {
        set_error("Newcamd TCP connect failed");
        return false;
    }
}

bool NewcamdClient::send_message(std::vector<uint8_t> payload, uint16_t service_id, uint16_t caid, uint32_t provid, bool use_msg_id, uint16_t* message_id) {
    if (!socket_ || !socket_->is_open()) {
        set_error("Newcamd socket is not open");
        return false;
    }
    if (!session_key_ready_) {
        set_error("Newcamd session key is not ready");
        return false;
    }
    if (payload.size() < 3 || payload.size() + kHeaderSize + 16 > kMaxMessageSize) {
        set_error("Newcamd message size is invalid");
        return false;
    }

    DES_key_schedule ks1{};
    DES_key_schedule ks2{};
    if (!make_schedules(session_key_, ks1, ks2)) {
        set_error("Newcamd DES key schedule failed");
        return false;
    }

    const uint16_t bodyLen = static_cast<uint16_t>(payload.size() - 3);
    payload[1] = static_cast<uint8_t>((payload[1] & 0xF0) | ((bodyLen >> 8) & 0x0F));
    payload[2] = static_cast<uint8_t>(bodyLen & 0xFF);

    std::vector<uint8_t> plain(kHeaderSize + payload.size(), 0);
    plain[4] = static_cast<uint8_t>(service_id >> 8);
    plain[5] = static_cast<uint8_t>(service_id & 0xFF);
    plain[6] = static_cast<uint8_t>(caid >> 8);
    plain[7] = static_cast<uint8_t>(caid & 0xFF);
    plain[8] = static_cast<uint8_t>((provid >> 16) & 0xFF);
    plain[9] = static_cast<uint8_t>((provid >> 8) & 0xFF);
    plain[10] = static_cast<uint8_t>(provid & 0xFF);
    std::memcpy(plain.data() + kHeaderSize, payload.data(), payload.size());

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (message_id) *message_id = 0;
        if (use_msg_id) {
            ++msg_id_;
            plain[2] = static_cast<uint8_t>(msg_id_ >> 8);
            plain[3] = static_cast<uint8_t>(msg_id_ & 0xFF);
            if (message_id) *message_id = msg_id_;
        }

        const size_t pad = (8 - ((plain.size() - 1) % 8)) % 8;
        const size_t paddedLen = plain.size() + pad + 1;
        std::vector<uint8_t> encrypted(paddedLen + 8, 0);
        std::copy(plain.begin(), plain.end(), encrypted.begin());
        if (pad > 0) random_bytes(encrypted.data() + plain.size(), pad);
        encrypted[paddedLen - 1] = xor_sum(encrypted.data() + 2, paddedLen - 2);

        DES_cblock ivec{};
        random_bytes(ivec, sizeof(ivec));
        std::memcpy(encrypted.data() + paddedLen, ivec, sizeof(ivec));
        DES_ede2_cbc_encrypt(encrypted.data() + 2, encrypted.data() + 2,
                             static_cast<long>(paddedLen - 2), &ks1, &ks2, &ivec, DES_ENCRYPT);
        const size_t totalLen = paddedLen + sizeof(ivec);
        encrypted[0] = static_cast<uint8_t>((totalLen - 2) >> 8);
        encrypted[1] = static_cast<uint8_t>((totalLen - 2) & 0xFF);

        try {
            boost::asio::write(*socket_, boost::asio::buffer(encrypted.data(), totalLen));
            return true;
        } catch (const std::exception& ex) {
            set_error(ex.what());
            return false;
        }
    }
}

bool NewcamdClient::receive_message(Message& message, bool check_msg_id) {
    message = Message{};
    if (!socket_ || !socket_->is_open()) {
        set_error("Newcamd socket is not open");
        return false;
    }
    if (!session_key_ready_) {
        set_error("Newcamd session key is not ready");
        return false;
    }

    DES_key_schedule ks1{};
    DES_key_schedule ks2{};
    if (!make_schedules(session_key_, ks1, ks2)) {
        set_error("Newcamd DES key schedule failed");
        return false;
    }

    try {
        uint8_t header[2]{};
        boost::asio::read(*socket_, boost::asio::buffer(header, sizeof(header)));
        const size_t encryptedLen = (static_cast<size_t>(header[0]) << 8) | header[1];
        if (encryptedLen == 0 || encryptedLen > kMaxMessageSize - 2) {
            set_error("Newcamd incoming message is too large");
            return false;
        }
        std::vector<uint8_t> netbuf(encryptedLen + 2, 0);
        netbuf[0] = header[0];
        netbuf[1] = header[1];
        boost::asio::read(*socket_, boost::asio::buffer(netbuf.data() + 2, encryptedLen));

        if (((netbuf.size() - 2) % 8) != 0 || netbuf.size() < 18) {
            set_error("Newcamd encrypted message has invalid block size");
            return false;
        }
        const size_t plainLen = netbuf.size() - 8;
        DES_cblock ivec{};
        std::memcpy(ivec, netbuf.data() + plainLen, sizeof(ivec));
        DES_ede2_cbc_encrypt(netbuf.data() + 2, netbuf.data() + 2,
                             static_cast<long>(plainLen - 2), &ks1, &ks2, &ivec, DES_DECRYPT);

        if (xor_sum(netbuf.data() + 2, plainLen - 2) != 0) {
            set_error("Newcamd checksum error; DES key or credentials may be wrong");
            return false;
        }
        if (plainLen < kHeaderSize + 3) {
            set_error("Newcamd message is shorter than the protocol header");
            return false;
        }

        message.id = static_cast<uint16_t>((netbuf[2] << 8) | netbuf[3]);
        if (check_msg_id && message.id != msg_id_) {
            std::ostringstream out;
            out << "Newcamd message id mismatch: got " << message.id << ", expected " << msg_id_;
            set_error(out.str());
            return false;
        }
        const size_t payloadLen = 3 + (((netbuf[kHeaderSize + 1] & 0x0F) << 8) | netbuf[kHeaderSize + 2]);
        if (kHeaderSize + payloadLen > plainLen) {
            set_error("Newcamd payload length is invalid");
            return false;
        }
        message.payload.assign(netbuf.begin() + kHeaderSize, netbuf.begin() + kHeaderSize + payloadLen);
        return true;
    } catch (const std::exception& ex) {
        set_error(ex.what());
        return false;
    }
}

bool NewcamdClient::login() {
    if (!socket_ || !socket_->is_open()) {
        set_error("Newcamd socket is not open");
        return false;
    }
    if (!parse_des_key()) return false;

    try {
        std::array<uint8_t, 14> serverNonce{};
        boost::asio::read(*socket_, boost::asio::buffer(serverNonce.data(), serverNonce.size()));
        if (!derive_key_from_seed(serverNonce.data(), serverNonce.size())) {
            set_error("Newcamd login key derivation failed");
            return false;
        }

        const std::string cryptPass = md5_crypt_password();
        if (cryptPass.empty()) {
            set_error("Newcamd password crypt() failed");
            return false;
        }

        std::vector<uint8_t> loginPayload(3, 0);
        loginPayload[0] = kMsgClientLogin;
        loginPayload.insert(loginPayload.end(), user_.begin(), user_.end());
        loginPayload.push_back(0);
        loginPayload.insert(loginPayload.end(), cryptPass.begin(), cryptPass.end());
        loginPayload.push_back(0);
        if (!send_message(std::move(loginPayload), kClientId, 0, 0, true)) return false;

        Message answer;
        if (!receive_message(answer, false) || answer.payload.empty()) return false;
        if (answer.payload[0] == kMsgClientLoginNak) {
            set_error("Newcamd login rejected by OSCam");
            return false;
        }
        if (answer.payload[0] != kMsgClientLoginAck) {
            set_error("Newcamd login expected LOGIN_ACK, got " + command_name(answer.payload[0]));
            return false;
        }

        if (!derive_key_from_seed(reinterpret_cast<const uint8_t*>(cryptPass.data()), cryptPass.size())) {
            set_error("Newcamd session key derivation failed");
            return false;
        }

        std::vector<uint8_t> cardReq{ kMsgCardDataReq, 0, 0 };
        if (!send_message(std::move(cardReq), 0, 0, 0, false)) return false;
        Message cardData;
        if (!receive_message(cardData, false) || cardData.payload.empty()) return false;
        if (cardData.payload[0] != kMsgCardData) {
            set_error("Newcamd CARD_DATA_REQ expected CARD_DATA, got " + command_name(cardData.payload[0]));
            return false;
        }

        authenticated_ = true;
        set_error({});
        return true;
    } catch (const std::exception& ex) {
        set_error(ex.what());
        return false;
    } catch (...) {
        set_error("Newcamd login failed");
        return false;
    }
}

bool NewcamdClient::send_ecm(uint16_t service_id, uint16_t caid, uint32_t provid, const std::vector<uint8_t>& ecm, uint16_t* message_id) {
    if (!authenticated_) {
        set_error("Newcamd ECM request skipped: client is not authenticated");
        return false;
    }
    if (ecm.size() < 3 || (ecm[0] != 0x80 && ecm[0] != 0x81)) {
        set_error("Newcamd ECM request is not a valid ECM section");
        return false;
    }

    // Astra-compatible transaction model: one ECM request is in flight per
    // Newcamd TCP connection. Multi-service scheduling is handled by the plugin
    // queue, which submits the next service immediately after this reply arrives.
    size_t pending = pending_ecms_.load(std::memory_order_relaxed);
    while (true) {
        if (pending >= kMaxPendingEcms) {
            set_error("Newcamd ECM transaction is already in flight");
            return false;
        }
        if (pending_ecms_.compare_exchange_weak(
                pending, pending + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            break;
        }
    }

    if (!send_message(ecm, service_id, caid, provid & 0x00FFFFFFu, true, message_id)) {
        pending_ecms_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    set_error({});
    return true;
}

void NewcamdClient::start_receiver() {
    if (running_) return;
    running_ = true;
    receiver_thread_ = std::thread(&NewcamdClient::receiver_loop, this);
}

void NewcamdClient::receiver_loop() {
    while (running_) {
        Message message;
        if (!receive_message(message, false)) break;
        if (message.payload.empty()) continue;

        KeyUpdateCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (!callback) continue;

        const uint8_t command = message.payload[0];
        if (command == 0x80 || command == 0x81) {
            const size_t pending = pending_ecms_.load(std::memory_order_relaxed);
            if (pending > 0) pending_ecms_.fetch_sub(1, std::memory_order_acq_rel);
        }
        if ((command == 0x80 || command == 0x81) && message.payload.size() >= 19) {
            callback(message.id, 0, message.payload.data() + 3);
            callback(message.id, 1, message.payload.data() + 11);
        } else if (command == 0x80 || command == 0x81) {
            set_error("Newcamd ECM response did not contain a control word");
        }
    }
    running_ = false;
}

void NewcamdClient::disconnect() {
    running_ = false;
    authenticated_ = false;
    pending_ecms_ = 0;
    if (socket_) {
        // Wake a synchronous receive before joining the receiver thread.
        // close() alone is not a reliable cross-thread interruption for an
        // already-blocked read on every Linux/socket path.
        boost::system::error_code ignored;
        socket_->cancel(ignored);
        socket_->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_->close(ignored);
    }
    if (receiver_thread_.joinable()) {
        if (receiver_thread_.get_id() == std::this_thread::get_id()) {
            receiver_thread_.detach();
        } else {
            receiver_thread_.join();
        }
    }
}
