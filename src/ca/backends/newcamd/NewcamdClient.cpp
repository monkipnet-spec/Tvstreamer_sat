#include "NewcamdClient.h"
#include <iostream>
#include <vector>
#include <arpa/inet.h>
#include <openssl/des.h>

using boost::asio::ip::tcp;

static void des_encrypt(uint8_t* data, const uint8_t* key) {
    DES_key_schedule schedule;
    DES_set_odd_parity((DES_cblock*)key);
    DES_set_key_checked((DES_cblock*)key, &schedule);
    DES_ecb_encrypt((DES_cblock*)data, (DES_cblock*)data, &schedule, DES_ENCRYPT);
}

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
        std::cerr << "Newcamd connected to " << host_ << ":" << port_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Newcamd connection error: " << e.what() << std::endl;
        return false;
    }
}

bool NewcamdClient::login() {
    if (!socket_ || !socket_->is_open()) return false;

    // 1. Получаем Server Nonce (14 байт)
    std::vector<uint8_t> server_nonce(14);
    boost::asio::read(*socket_, boost::asio::buffer(server_nonce));

    // 2. Формируем пароль для логина
    std::vector<uint8_t> login_password(20);
    std::copy(pass_.begin(), pass_.end(), login_password.begin());

    // 3. Шифруем (упрощенная реализация LOGIN64)
    uint8_t des_key[8];
    // Преобразуем строковый DES ключ (hex) в байты
    for(int i = 0; i < 8; ++i) {
        des_key[i] = std::stoi(des_.substr(i*2, 2), nullptr, 16);
    }
    
    // В реальности здесь должна быть более сложная логика LOGIN64:
    // Шифруем данные пароля с использованием nonce сервера и DES ключа
    
    // 4. Отправляем LOGIN64
    std::vector<uint8_t> login_pkt;
    login_pkt.insert(login_pkt.end(), user_.begin(), user_.end());
    login_pkt.push_back(0x00);
    login_pkt.insert(login_pkt.end(), login_password.begin(), login_password.end());
    
    uint16_t len = htons(login_pkt.size() + 2);
    std::vector<uint8_t> full_pkt;
    full_pkt.push_back((len >> 8) & 0xFF);
    full_pkt.push_back(len & 0xFF);
    full_pkt.insert(full_pkt.end(), login_pkt.begin(), login_pkt.end());
    
    boost::asio::write(*socket_, boost::asio::buffer(full_pkt));
    
    return true;
}

void NewcamdClient::disconnect() {
    if (socket_ && socket_->is_open()) {
        socket_->close();
    }
}

bool NewcamdClient::get_dcw(const std::vector<uint8_t>& ecm, std::vector<uint8_t>& dcw) {
// ... existing code ...
