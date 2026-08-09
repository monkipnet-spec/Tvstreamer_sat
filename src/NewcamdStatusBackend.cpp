// NewcamdStatusBackend.cpp
#include "NewcamdStatusBackend.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/des.h>
#include <openssl/md5.h>
#include <openssl/evp.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace ca_provider {

// ---------------------------------------------------------------------------
// Парсинг endpoint с поддержкой полного URL
// ---------------------------------------------------------------------------
bool NewcamdStatusBackend::parseEndpoint(const std::string& endpoint, std::string& host, int& port,
                                         std::string& username, std::string& password,
                                         std::string& des_key, bool& emm_mode) {
    host.clear();
    port = 0;
    username.clear();
    password.clear();
    des_key.clear();
    emm_mode = false;
    
    if (endpoint.empty()) return false;

    std::string value = endpoint;
    const std::string prefix = "newcamd://";
    if (value.rfind(prefix, 0) == 0) value.erase(0, prefix.size());

    // Парсим user:pass@
    size_t at_pos = value.find('@');
    if (at_pos != std::string::npos) {
        std::string userpass = value.substr(0, at_pos);
        size_t colon = userpass.find(':');
        if (colon != std::string::npos) {
            username = userpass.substr(0, colon);
            password = userpass.substr(colon + 1);
        }
        value.erase(0, at_pos + 1);
    }

    // Парсим host:port/key/EMM
    size_t slash1 = value.find('/');
    std::string hostport = (slash1 != std::string::npos) ? value.substr(0, slash1) : value;
    std::string rest = (slash1 != std::string::npos) ? value.substr(slash1 + 1) : "";

    // Парсим host:port
    if (!hostport.empty() && hostport.front() == '[') {
        size_t close = hostport.find(']');
        if (close == std::string::npos || close + 2 > hostport.size() || hostport[close + 1] != ':')
            return false;
        host = hostport.substr(1, close - 1);
        try { port = std::stoi(hostport.substr(close + 2)); } catch (...) { return false; }
    } else {
        size_t colon = hostport.rfind(':');
        if (colon == std::string::npos) return false;
        host = hostport.substr(0, colon);
        try { port = std::stoi(hostport.substr(colon + 1)); } catch (...) { return false; }
    }

    // Парсим ключ и EMM
    if (!rest.empty()) {
        size_t slash2 = rest.find('/');
        if (slash2 != std::string::npos) {
            des_key = rest.substr(0, slash2);
            emm_mode = (rest.substr(slash2 + 1) == "EMM");
        } else {
            des_key = rest;
        }
    }

    return !host.empty() && port > 0 && port <= 65535;
}

// ---------------------------------------------------------------------------
// Полная проверка с аутентификацией
// ---------------------------------------------------------------------------
NewcamdStatusResult NewcamdStatusBackend::probe(const std::string& endpoint, int timeoutMs) {
    NewcamdStatusResult result;
    
    std::string host, username, password, des_key;
    int port = 0;
    bool emm_mode = false;
    
    if (!parseEndpoint(endpoint, host, port, username, password, des_key, emm_mode)) {
        result.status = "NOT_CONFIGURED";
        result.error = endpoint.empty() ? "endpoint is empty" : "invalid endpoint format";
        return result;
    }
    
    result.host = host;
    result.port = port;
    result.configured = true;

    // Создаем клиент
    NewcamdClient client(host, port, username, password, des_key, timeoutMs);
    
    auto start = std::chrono::steady_clock::now();
    
    if (!client.connect()) {
        result.status = "TCP_OFFLINE";
        result.error = client.getLastError();
        return result;
    }
    
    result.online = true;
    
    if (!client.authenticate()) {
        result.status = "AUTH_FAILED";
        result.error = client.getLastError();
        client.disconnect();
        return result;
    }
    
    result.authenticated = true;
    
    if (!client.getCardInfo()) {
        result.status = "CARD_ERROR";
        result.error = client.getLastError();
        client.disconnect();
        return result;
    }
    
    result.card_available = true;
    result.card_caid = client.getCardCAID();
    result.card_providers = client.getCardProviders();
    result.server_info = client.getServerInfo();
    result.response_time_ms = client.getResponseTime();
    
    auto end = std::chrono::steady_clock::now();
    result.response_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    result.status = "ONLINE";
    result.protocol_version = "Newcamd " + std::to_string(VERSION_525);
    
    client.disconnect();
    return result;
}

// ---------------------------------------------------------------------------
// Базовая проверка только TCP (оригинальная функциональность)
// ---------------------------------------------------------------------------
NewcamdStatusResult NewcamdStatusBackend::probeTCP(const std::string& endpoint, int timeoutMs) {
    NewcamdStatusResult result;
    
    std::string host;
    int port = 0;
    std::string username, password, des_key;
    bool emm_mode = false;
    
    if (!parseEndpoint(endpoint, host, port, username, password, des_key, emm_mode)) {
        result.status = "NOT_CONFIGURED";
        result.error = endpoint.empty() ? "endpoint is empty" : "expected host:port";
        return result;
    }
    
    result.host = host;
    result.port = port;
    result.configured = true;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(port);
    const int gai = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (gai != 0) {
        result.status = "DNS_ERROR";
        result.error = ::gai_strerror(gai);
        return result;
    }

    std::string lastError = "connection failed";
    for (addrinfo* p = resolved; p; p = p->ai_next) {
        const int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        const int oldFlags = ::fcntl(fd, F_GETFL, 0);
        if (oldFlags >= 0) ::fcntl(fd, F_SETFL, oldFlags | O_NONBLOCK);
        int rc = ::connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc == 0) {
            result.online = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            rc = ::poll(&pfd, 1, timeoutMs);
            if (rc > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                    result.online = true;
                } else if (error != 0) {
                    lastError = std::strerror(error);
                }
            } else if (rc == 0) {
                lastError = "connect timeout";
            }
        } else {
            lastError = std::strerror(errno);
        }
        ::close(fd);
        if (result.online) break;
    }
    ::freeaddrinfo(resolved);

    result.status = result.online ? "TCP_ONLINE" : "TCP_OFFLINE";
    if (!result.online) result.error = lastError;
    return result;
}

// ---------------------------------------------------------------------------
// Реализация NewcamdClient
// ---------------------------------------------------------------------------
NewcamdStatusBackend::NewcamdClient::NewcamdClient(const std::string& host, int port,
                                                    const std::string& username,
                                                    const std::string& password,
                                                    const std::string& des_key_hex,
                                                    int timeoutMs)
    : sock_fd_(-1), state_(STATE_DISCONNECTED), timeout_ms_(timeoutMs),
      response_time_ms_(0), host_(host), port_(port), username_(username),
      password_(password), des_key_hex_(des_key_hex), emm_mode_(false),
      card_caid_(0), user_id_(0), sent_msg_id_(0), received_msg_id_(0),
      proto_version_(VERSION_525)
{
    memset(des_key_, 0, sizeof(des_key_));
    memset(random_key_, 0, sizeof(random_key_));
    memset(login_key_, 0, sizeof(login_key_));
    memset(session_key_, 0, sizeof(session_key_));
    memset(cam_id_, 0, sizeof(cam_id_));
    
    // Парсим hex-ключ
    size_t key_len = 0;
    if (!hexToBytes(des_key_hex_, des_key_, key_len) || key_len != 14) {
        last_error_ = "Invalid DES key length (must be 14 bytes)";
        state_ = STATE_ERROR;
    }
}

NewcamdStatusBackend::NewcamdClient::~NewcamdClient() {
    disconnect();
}

// ---------------------------------------------------------------------------
// Подключение к серверу
// ---------------------------------------------------------------------------
bool NewcamdStatusBackend::NewcamdClient::connect() {
    if (state_ == STATE_ERROR) return false;
    
    std::lock_guard<std::mutex> lock(mutex_);
    return connectToServer();
}

bool NewcamdStatusBackend::NewcamdClient::connectToServer() {
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(port_);
    
    if (::getaddrinfo(host_.c_str(), service.c_str(), &hints, &resolved) != 0) {
        last_error_ = "DNS resolution failed";
        return false;
    }

    bool connected = false;
    for (addrinfo* p = resolved; p; p = p->ai_next) {
        sock_fd_ = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock_fd_ < 0) continue;

        int flags = ::fcntl(sock_fd_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(sock_fd_, F_SETFL, flags | O_NONBLOCK);

        if (::connect(sock_fd_, p->ai_addr, p->ai_addrlen) == 0) {
            connected = true;
            break;
        }

        if (errno == EINPROGRESS) {
            pollfd pfd{sock_fd_, POLLOUT, 0};
            int rc = ::poll(&pfd, 1, timeout_ms_);
            if (rc > 0 && (pfd.revents & POLLOUT)) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (::getsockopt(sock_fd_, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                    connected = true;
                    break;
                } else if (error != 0) {
                    last_error_ = std::strerror(error);
                }
            } else if (rc == 0) {
                last_error_ = "connect timeout";
            }
        } else {
            last_error_ = std::strerror(errno);
        }

        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    ::freeaddrinfo(resolved);
    
    if (connected) {
        // Возвращаем блокирующий режим
        int flags = ::fcntl(sock_fd_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(sock_fd_, F_SETFL, flags & ~O_NONBLOCK);
        state_ = STATE_CONNECTED;
        return true;
    }
    
    return false;
}

// ---------------------------------------------------------------------------
// Аутентификация
// ---------------------------------------------------------------------------
bool NewcamdStatusBackend::NewcamdClient::authenticate() {
    if (state_ != STATE_CONNECTED) {
        last_error_ = "Not connected";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return performLogin();
}

bool NewcamdStatusBackend::NewcamdClient::performLogin() {
    auto start = std::chrono::steady_clock::now();
    
    // Шаг 1: Получаем 14 байт от сервера
    uint8_t buffer[256];
    ssize_t recv_len = ::recv(sock_fd_, buffer, 14, 0);
    if (recv_len != 14) {
        last_error_ = "Failed to receive random key";
        return false;
    }

    memcpy(random_key_, buffer, 14);

    // Шаг 2: Вычисляем login_key = XOR(random_key, des_key)
    createLoginKey(random_key_, des_key_);

    // Шаг 3: Хешируем пароль
    uint8_t password_hash[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(password_.c_str()),
        password_.length(), password_hash);

    // Шаг 4: Формируем логин-пакет
    uint8_t login_data[256];
    uint16_t login_len = 0;
    memcpy(login_data, username_.c_str(), username_.length() + 1);
    login_len += username_.length() + 1;
    memcpy(login_data + login_len, password_hash, MD5_DIGEST_LENGTH);
    login_len += MD5_DIGEST_LENGTH;

    // Шаг 5: Отправляем зашифрованным login_key
    if (!sendMessage(MSG_CLIENT_2_SERVER_LOGIN, login_data, login_len,
                     nullptr, 0, true)) {
        last_error_ = "Failed to send login packet";
        return false;
    }

    // Шаг 6: Получаем ответ
    uint8_t response[256];
    uint16_t resp_len = 0;
    uint8_t custom[8];
    uint16_t custom_len = 0;

    net_msg_type_t msg = receiveMessage(response, &resp_len, custom, &custom_len, true);
    if (msg != MSG_CLIENT_2_SERVER_LOGIN_ACK) {
        last_error_ = "Login NAK received";
        return false;
    }

    // Извлекаем ID пользователя и информацию о сервере
    if (resp_len >= 6) {
        user_id_ = response[0];
        if (resp_len > 1) {
            server_info_ = std::string(reinterpret_cast<char*>(response + 1), resp_len - 1);
        }
    }

    auto end = std::chrono::steady_clock::now();
    response_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    state_ = STATE_READY;
    return true;
}

// ---------------------------------------------------------------------------
// Получение информации о карте
// ---------------------------------------------------------------------------
bool NewcamdStatusBackend::NewcamdClient::getCardInfo() {
    if (state_ != STATE_READY) {
        last_error_ = "Not authenticated";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Отправляем запрос данных карты
    if (!sendMessage(MSG_CARD_DATA_REQ, nullptr, 0, nullptr, 0, true)) {
        last_error_ = "Failed to request card data";
        return false;
    }

    // Получаем ответ
    uint8_t buffer[256];
    uint16_t len = 0;
    uint8_t custom[8];
    uint16_t custom_len = 0;

    net_msg_type_t msg = receiveMessage(buffer, &len, custom, &custom_len, true);
    if (msg != MSG_CARD_DATA) {
        last_error_ = "Card data request failed";
        return false;
    }

    // Парсим данные карты
    if (len >= 8) {
        card_caid_ = (buffer[0] << 8) | buffer[1];
        uint16_t prov_count = (buffer[2] << 8) | buffer[3];

        // Извлекаем провайдеров
        for (int i = 0; i < prov_count && i < 4; i++) {
            uint16_t prov = (buffer[4 + i*2] << 8) | buffer[5 + i*2];
            card_providers_.push_back(prov);
        }

        memcpy(cam_id_, buffer + 4 + prov_count*2, 4);
        return true;
    }

    last_error_ = "Invalid card data response";
    return false;
}

// ---------------------------------------------------------------------------
// Отключение
// ---------------------------------------------------------------------------
void NewcamdStatusBackend::NewcamdClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
    state_ = STATE_DISCONNECTED;
    sent_msg_id_ = 0;
    received_msg_id_ = 0;
}

// ---------------------------------------------------------------------------
// Генерация ключей
// ---------------------------------------------------------------------------
void NewcamdStatusBackend::NewcamdClient::createLoginKey(const uint8_t* random,
                                                          const uint8_t* des_key) {
    for (int i = 0; i < 14; i++) {
        login_key_[i] = random[i] ^ des_key[i];
    }
    // DES ключ должен быть 16 байт, расширяем для Triple DES
    login_key_[14] = login_key_[0];
    login_key_[15] = login_key_[1];

    // Копируем в сессионный ключ
    memcpy(session_key_, login_key_, 16);
}

// ---------------------------------------------------------------------------
// Отправка сообщения
// ---------------------------------------------------------------------------
bool NewcamdStatusBackend::NewcamdClient::sendMessage(net_msg_type_t cmd,
                                                       const uint8_t* data, uint16_t len,
                                                       const uint8_t* custom_data,
                                                       uint16_t custom_len, bool encrypt) {
    uint8_t packet[1024];
    uint16_t header_len = (proto_version_ == VERSION_524) ? 8 : 12;
    uint16_t total_len = header_len + len;

    // Заголовок
    packet[0] = (total_len >> 8) & 0xFF;
    packet[1] = total_len & 0xFF;
    packet[2] = (sent_msg_id_ >> 8) & 0xFF;
    packet[3] = sent_msg_id_ & 0xFF;
    sent_msg_id_++;

    if (proto_version_ == VERSION_525) {
        if (custom_data && custom_len >= 2) {
            packet[4] = custom_data[0];
            packet[5] = custom_data[1];
            packet[6] = (custom_len >= 4) ? custom_data[2] : 0;
            packet[7] = (custom_len >= 6) ? custom_data[4] : 0;
        } else {
            packet[4] = packet[5] = packet[6] = packet[7] = 0;
        }
        packet[8] = static_cast<uint8_t>(cmd);
        packet[9] = (len >> 8) & 0x0F;  // 12-bit length
        packet[10] = len & 0xFF;
        packet[11] = 0;
    } else {
        packet[4] = static_cast<uint8_t>(cmd);
        packet[5] = (len >> 8) & 0xFF;
        packet[6] = len & 0xFF;
        packet[7] = 0;
        if (custom_data && custom_len >= 2) {
            packet[6] = custom_data[0];
            packet[7] = custom_data[1];
        }
    }

    if (data && len > 0) {
        memcpy(packet + header_len, data, len);
    }

    if (encrypt) {
        uint16_t encrypt_len = header_len + len;
        uint16_t padded_len = ((encrypt_len + 7) / 8) * 8;
        if (padded_len > encrypt_len) {
            memset(packet + encrypt_len, 0, padded_len - encrypt_len);
        }
        if (!desEncrypt(packet, padded_len, session_key_)) {
            return false;
        }
        total_len = padded_len;
    }

    ssize_t sent = ::send(sock_fd_, packet, total_len, 0);
    return sent == total_len;
}

// ---------------------------------------------------------------------------
// Приём сообщения
// ---------------------------------------------------------------------------
net_msg_type_t NewcamdStatusBackend::NewcamdClient::receiveMessage(uint8_t* buffer,
                                                                    uint16_t* len,
                                                                    uint8_t* custom_data,
                                                                    uint16_t* custom_len,
                                                                    bool decrypt) {
    uint16_t header_len = (proto_version_ == VERSION_524) ? 8 : 12;
    uint8_t header[16] = {0};

    ssize_t recv_len = ::recv(sock_fd_, header, header_len, 0);
    if (recv_len != header_len) {
        return MSG_CLIENT_2_SERVER_LOGIN_NAK;
    }

    uint16_t total_len = (header[0] << 8) | header[1];
    *len = 0;

    uint16_t data_len = total_len - header_len;
    if (data_len > 0) {
        recv_len = ::recv(sock_fd_, buffer, data_len, 0);
        if (recv_len != data_len) {
            return MSG_CLIENT_2_SERVER_LOGIN_NAK;
        }

        if (decrypt) {
            if (!desDecrypt(buffer, data_len, session_key_)) {
                return MSG_CLIENT_2_SERVER_LOGIN_NAK;
            }
        }

        *len = recv_len;
    }

    uint8_t cmd;
    if (proto_version_ == VERSION_525) {
        cmd = header[8];
        if (custom_data && custom_len) {
            custom_data[0] = header[4];
            custom_data[1] = header[5];
            custom_data[2] = header[6];
            custom_data[3] = header[7];
            *custom_len = 4;
        }
    } else {
        cmd = header[4];
        if (custom_data && custom_len) {
            custom_data[0] = header[6];
            custom_data[1] = header[7];
            *custom_len = 2;
        }
    }

    return static_cast<net_msg_type_t>(cmd);
}

// ---------------------------------------------------------------------------
// Шифрование/дешифрование Triple DES
// ---------------------------------------------------------------------------
bool NewcamdStatusBackend::NewcamdClient::desEncrypt(uint8_t* data, uint16_t len,
                                                      const uint8_t* key) {
    DES_key_schedule ks1, ks2;
    DES_cblock key1, key2;
    memcpy(key1, key, 8);
    memcpy(key2, key + 8, 8);
    
    if (DES_set_key_checked(&key1, &ks1) != 0) return false;
    if (DES_set_key_checked(&key2, &ks2) != 0) return false;

    unsigned char iv[8] = {0};  // Нулевой IV для Newcamd
    DES_ede2_cbc_encrypt(data, data, len, &ks1, &ks2, &ks1, DES_ENCRYPT);
    return true;
}

bool NewcamdStatusBackend::NewcamdClient::desDecrypt(uint8_t* data, uint16_t len,
                                                      const uint8_t* key) {
    DES_key_schedule ks1, ks2;
    DES_cblock key1, key2;
    memcpy(key1, key, 8);
    memcpy(key2, key + 8, 8);
    
    if (DES_set_key_checked(&key1, &ks1) != 0) return false;
    if (DES_set_key_checked(&key2, &ks2) != 0) return false;

    unsigned char iv[8] = {0};  // Нулевой IV для Newcamd
    DES_ede2_cbc_encrypt(data, data, len, &ks1, &ks2, &ks1, DES_DECRYPT);
    return true;
}

// ---------------------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------------------
bool NewcamdStatusBackend::NewcamdClient::hexToBytes(const std::string& hex,
                                                      uint8_t* bytes, size_t& len) {
    len = 0;
    if (hex.length() % 2 != 0) return false;
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        try {
            bytes[len++] = static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16));
        } catch (...) {
            return false;
        }
    }
    return true;
}

void NewcamdStatusBackend::NewcamdClient::debugDump(const uint8_t* data, uint16_t len,
                                                     const char* prefix) {
    // Отключено для production, можно включить при необходимости
    (void)data;
    (void)len;
    (void)prefix;
}

} // namespace ca_provider