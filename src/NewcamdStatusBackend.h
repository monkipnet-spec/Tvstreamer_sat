// NewcamdStatusBackend.h
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

namespace ca_provider {

// Типы сообщений протокола Newcamd
#define CWS_FIRSTCMDNO 0xE0

enum net_msg_type_t {
    MSG_CLIENT_2_SERVER_LOGIN = CWS_FIRSTCMDNO,
    MSG_CLIENT_2_SERVER_LOGIN_ACK,
    MSG_CLIENT_2_SERVER_LOGIN_NAK,
    MSG_CARD_DATA_REQ,
    MSG_CARD_DATA,
    MSG_SERVER_2_CLIENT_NAME,
    MSG_SERVER_2_CLIENT_NAME_ACK,
    MSG_SERVER_2_CLIENT_NAME_NAK,
    MSG_SERVER_2_CLIENT_LOGIN,
    MSG_SERVER_2_CLIENT_LOGIN_ACK,
    MSG_SERVER_2_CLIENT_LOGIN_NAK,
    MSG_ADMIN,
    MSG_ADMIN_ACK,
    MSG_ADMIN_LOGIN,
    MSG_ADMIN_LOGIN_ACK,
    MSG_ADMIN_LOGIN_NAK,
    MSG_ADMIN_COMMAND,
    MSG_ADMIN_COMMAND_ACK,
    MSG_ADMIN_COMMAND_NAK,
    MSG_KEEPALIVE = CWS_FIRSTCMDNO + 0x1D
};

// Версии протокола
enum protocol_version_t {
    VERSION_524 = 524,
    VERSION_525 = 525
};

struct NewcamdStatusResult {
    bool configured = false;
    bool online = false;
    bool authenticated = false;
    bool card_available = false;
    std::string host;
    int port = 0;
    std::string status;
    std::string error;
    std::string server_info;
    uint16_t card_caid = 0;
    std::vector<uint16_t> card_providers;
    std::string protocol_version;
    int response_time_ms = 0;
};

// Полноценный клиент Newcamd с проверкой статуса
class NewcamdStatusBackend {
public:
    // Полная проверка с аутентификацией
    static NewcamdStatusResult probe(const std::string& endpoint, int timeoutMs = 5000);
    
    // Базовая проверка только TCP
    static NewcamdStatusResult probeTCP(const std::string& endpoint, int timeoutMs = 1200);
    
    // Парсинг endpoint URL
    static bool parseEndpoint(const std::string& endpoint, std::string& host, int& port,
                              std::string& username, std::string& password,
                              std::string& des_key, bool& emm_mode);

private:
    class NewcamdClient;
};

// Внутренний класс для работы с протоколом
class NewcamdStatusBackend::NewcamdClient {
public:
    NewcamdClient(const std::string& host, int port,
                  const std::string& username, const std::string& password,
                  const std::string& des_key_hex, int timeoutMs);
    ~NewcamdClient();

    bool connect();
    bool authenticate();
    bool getCardInfo();
    void disconnect();
    
    bool isReady() const { return state_ == STATE_READY; }
    uint16_t getCardCAID() const { return card_caid_; }
    const std::vector<uint16_t>& getCardProviders() const { return card_providers_; }
    std::string getServerInfo() const { return server_info_; }
    int getResponseTime() const { return response_time_ms_; }
    std::string getLastError() const { return last_error_; }

private:
    enum State {
        STATE_DISCONNECTED,
        STATE_CONNECTING,
        STATE_CONNECTED,
        STATE_READY,
        STATE_ERROR
    };

    // Сокет и состояние
    int sock_fd_;
    State state_;
    int timeout_ms_;
    int response_time_ms_;
    std::string last_error_;
    std::mutex mutex_;

    // Параметры соединения
    std::string host_;
    int port_;
    std::string username_;
    std::string password_;
    std::string des_key_hex_;
    bool emm_mode_;

    // Данные карты
    uint16_t card_caid_;
    std::vector<uint16_t> card_providers_;
    uint8_t user_id_;
    uint8_t cam_id_[4];
    std::string server_info_;

    // Ключи шифрования
    uint8_t des_key_[14];
    uint8_t random_key_[14];
    uint8_t login_key_[16];
    uint8_t session_key_[16];

    // Последовательности
    uint16_t sent_msg_id_;
    uint16_t received_msg_id_;
    protocol_version_t proto_version_;

    // Внутренние методы
    bool connectToServer();
    bool performLogin();
    bool requestCardData();
    
    bool sendMessage(net_msg_type_t cmd, const uint8_t* data, uint16_t len,
                     const uint8_t* custom_data, uint16_t custom_len, bool encrypt);
    net_msg_type_t receiveMessage(uint8_t* buffer, uint16_t* len,
                                  uint8_t* custom_data, uint16_t* custom_len, bool decrypt);
    
    void createLoginKey(const uint8_t* random, const uint8_t* des_key);
    void createSessionKey(const uint8_t* password_hash);
    
    bool desEncrypt(uint8_t* data, uint16_t len, const uint8_t* key);
    bool desDecrypt(uint8_t* data, uint16_t len, const uint8_t* key);
    
    void debugDump(const uint8_t* data, uint16_t len, const char* prefix);
    bool hexToBytes(const std::string& hex, uint8_t* bytes, size_t& len);
};

} // namespace ca_provider