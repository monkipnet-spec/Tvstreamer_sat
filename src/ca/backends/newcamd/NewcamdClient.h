#pragma once

#include <string>
#include <vector>
#include <boost/asio.hpp>
#include <mutex>
#include <thread>

class NewcamdClient {
public:
    NewcamdClient(const std::string& host, int port, const std::string& user, const std::string& pass, const std::string& des);
    ~NewcamdClient();

    bool connect();
    bool login();
    void disconnect();
    
    // Отправка ECM и получение DCW (заглушка на данном этапе)
    bool get_dcw(const std::vector<uint8_t>& ecm, std::vector<uint8_t>& dcw);

private:
    std::string host_;
    int port_;
    std::string user_;
    std::string pass_;
    std::string des_;
    
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
    std::mutex mutex_;
};
