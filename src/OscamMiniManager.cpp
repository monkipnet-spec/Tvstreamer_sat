#include "OscamMiniManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
using IniSection = std::map<std::string, std::string>;

std::string trimLocal(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());
    return value;
}

std::vector<IniSection> parseIniSections(const std::string& text, const std::string& wanted) {
    std::vector<IniSection> result;
    std::istringstream input(text);
    std::string line;
    IniSection* current = nullptr;

    while (std::getline(input, line)) {
        auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = trimLocal(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            const std::string sectionName = trimLocal(line.substr(1, line.size() - 2));
            if (sectionName == wanted) {
                result.emplace_back();
                current = &result.back();
            } else {
                current = nullptr;
            }
            continue;
        }

        if (!current) {
            continue;
        }

        auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        (*current)[trimLocal(line.substr(0, equals))] = trimLocal(line.substr(equals + 1));
    }
    return result;
}

IniSection parseFirstSection(const std::string& text, const std::string& wanted) {
    auto sections = parseIniSections(text, wanted);
    return sections.empty() ? IniSection{} : sections.front();
}

std::string getValue(const IniSection& section, const std::string& key, const std::string& fallback = {}) {
    auto it = section.find(key);
    return it == section.end() ? fallback : it->second;
}

int getInt(const IniSection& section, const std::string& key, int fallback) {
    try {
        return std::stoi(getValue(section, key, std::to_string(fallback)));
    } catch (...) {
        return fallback;
    }
}

bool isTrue(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "1" || value == "yes" || value == "true" || value == "on";
}

std::string safeIni(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
    return trimLocal(value);
}

bool isHex(const std::string& value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool isSafeName(const std::string& value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '@';
    });
}

bool validGroups(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    bool expectDigit = true;
    int current = 0;
    for (unsigned char c : value) {
        if (std::isdigit(c)) {
            current = current * 10 + (c - '0');
            if (current > 64) {
                return false;
            }
            expectDigit = false;
        } else if (c == ',') {
            if (expectDigit || current < 1) {
                return false;
            }
            current = 0;
            expectDigit = true;
        } else if (!std::isspace(c)) {
            return false;
        }
    }
    return !expectDigit && current >= 1 && current <= 64;
}

struct PortDef {
    int port = 0;
    std::string caid;
    std::string provider;
};

std::vector<PortDef> parsePortDefs(const std::string& value) {
    std::vector<PortDef> result;
    std::istringstream input(value);
    std::string item;
    while (std::getline(input, item, ';')) {
        item = trimLocal(item);
        if (item.empty()) {
            continue;
        }
        auto at = item.find('@');
        auto colon = item.find(':', at == std::string::npos ? 0 : at + 1);
        if (at == std::string::npos || colon == std::string::npos) {
            continue;
        }
        PortDef def;
        try {
            def.port = std::stoi(trimLocal(item.substr(0, at)));
        } catch (...) {
            continue;
        }
        def.caid = trimLocal(item.substr(at + 1, colon - at - 1));
        def.provider = trimLocal(item.substr(colon + 1));
        if (def.port > 0 && def.port <= 65535 && isHex(def.caid, 4) && isHex(def.provider, 6)) {
            result.push_back(def);
        }
    }
    return result;
}

std::pair<std::string, std::string> accountCaidProvider(const IniSection& account) {
    std::string caid = getValue(account, "caid", "");
    std::string provider = "";
    std::string ident = getValue(account, "ident", "");
    if (!ident.empty()) {
        auto colon = ident.find(':');
        if (colon != std::string::npos) {
            if (caid.empty()) {
                caid = trimLocal(ident.substr(0, colon));
            }
            provider = trimLocal(ident.substr(colon + 1));
            auto comma = provider.find(',');
            if (comma != std::string::npos) {
                provider.resize(comma);
            }
        }
    }
    return {caid, provider};
}
}

OscamMiniManager& OscamMiniManager::instance() {
    static OscamMiniManager instance;
    return instance;
}

std::string OscamMiniManager::trim(std::string value) {
    return trimLocal(std::move(value));
}

std::string OscamMiniManager::readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

bool OscamMiniManager::writeAtomic(const std::string& path, const std::string& content, std::string& error) {
    try {
        fs::create_directories(fs::path(path).parent_path());
        const std::string temp = path + ".tmp." + std::to_string(getpid());
        {
            std::ofstream file(temp, std::ios::trunc);
            if (!file) {
                error = "Не удалось создать временный файл " + temp;
                return false;
            }
            file << content;
            if (!file.good()) {
                error = "Ошибка записи " + temp;
                return false;
            }
        }
        fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
        fs::rename(temp, path);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::string OscamMiniManager::run(const std::string& command, int* rc) {
    std::array<char, 512> buffer{};
    std::string output;
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        if (rc) {
            *rc = -1;
        }
        return "popen failed";
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    int status = pclose(pipe);
    if (rc) {
        *rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return output;
}

std::vector<std::string> OscamMiniManager::ttyDevices() {
    std::vector<std::string> devices;
    try {
        if (fs::exists("/dev/serial/by-id")) {
            for (const auto& entry : fs::directory_iterator("/dev/serial/by-id")) {
                devices.push_back(entry.path().string());
            }
        }
        for (const auto& entry : fs::directory_iterator("/dev")) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0) {
                devices.push_back(entry.path().string());
            }
        }
    } catch (...) {
    }
    std::sort(devices.begin(), devices.end());
    devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
    return devices;
}

std::string OscamMiniManager::json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value OscamMiniManager::parse(const std::string& body, std::string& error) {
    Json::Value value;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &value, &error)) {
        return {};
    }
    return value;
}

OscamMiniManager::Settings OscamMiniManager::loadLocked() {
    Settings settings;

    const auto newcamd = parseFirstSection(readFile(std::string(kConfigDir) + "/oscam.conf"), "newcamd");
    settings.bindIp = getValue(newcamd, "serverip", "127.0.0.1");
    settings.key = getValue(newcamd, "key", settings.key);
    settings.keepalive = isTrue(getValue(newcamd, "keepalive", "1"));

    auto ports = parsePortDefs(getValue(newcamd, "port", ""));
    auto accounts = parseIniSections(readFile(std::string(kConfigDir) + "/oscam.user"), "account");
    std::vector<bool> usedPorts(ports.size(), false);

    for (std::size_t i = 0; i < accounts.size(); ++i) {
        const auto& account = accounts[i];
        NewcamdUser user;
        user.user = getValue(account, "user", "tvstreamer" + std::to_string(i + 1));
        user.password = getValue(account, "pwd", "tvstreamer");
        user.groups = getValue(account, "group", "1");
        user.au = isTrue(getValue(account, "au", "1"));

        auto [accountCaid, accountProvider] = accountCaidProvider(account);
        int selected = -1;
        for (std::size_t p = 0; p < ports.size(); ++p) {
            if (usedPorts[p]) {
                continue;
            }
            if (!accountCaid.empty() && ports[p].caid != accountCaid) {
                continue;
            }
            if (!accountProvider.empty() && ports[p].provider != accountProvider) {
                continue;
            }
            selected = static_cast<int>(p);
            break;
        }
        if (selected < 0) {
            for (std::size_t p = 0; p < ports.size(); ++p) {
                if (!usedPorts[p]) {
                    selected = static_cast<int>(p);
                    break;
                }
            }
        }
        if (selected >= 0) {
            usedPorts[static_cast<std::size_t>(selected)] = true;
            user.port = ports[static_cast<std::size_t>(selected)].port;
            user.caid = ports[static_cast<std::size_t>(selected)].caid;
            user.provider = ports[static_cast<std::size_t>(selected)].provider;
        } else {
            user.port = 11220 + static_cast<int>(i);
            user.caid = isHex(accountCaid, 4) ? accountCaid : "0652";
            user.provider = isHex(accountProvider, 6) ? accountProvider : "000000";
        }
        settings.users.push_back(user);
    }

    // Preserve port definitions even if a legacy config has fewer accounts than ports.
    for (std::size_t p = 0; p < ports.size(); ++p) {
        if (usedPorts[p]) {
            continue;
        }
        NewcamdUser user;
        user.user = "user" + std::to_string(settings.users.size() + 1);
        user.password = "change_me";
        user.port = ports[p].port;
        user.caid = ports[p].caid;
        user.provider = ports[p].provider;
        user.groups = "1";
        settings.users.push_back(user);
    }

    if (settings.users.empty()) {
        settings.users.push_back(NewcamdUser{});
    }

    for (const auto& entry : parseIniSections(readFile(std::string(kConfigDir) + "/oscam.server"), "reader")) {
        Reader reader;
        reader.label = getValue(entry, "label", "Reader");
        reader.protocol = getValue(entry, "protocol", "mouse");
        reader.device = getValue(entry, "device", "");
        reader.caid = getValue(entry, "caid", "");
        reader.detect = getValue(entry, "detect", "cd");
        reader.mhz = getInt(entry, "mhz", 600);
        reader.cardmhz = getInt(entry, "cardmhz", 600);
        reader.group = getInt(entry, "group", 1);
        reader.ident = getValue(entry, "ident", "");
        reader.emmcache = getValue(entry, "emmcache", "1,3,2");
        reader.enabled = !isTrue(getValue(entry, "disable", "0"));
        settings.readers.push_back(reader);
    }

    return settings;
}

bool OscamMiniManager::saveLocked(const Settings& settings, std::string& error) {
    if (!isHex(settings.key, 28)) {
        error = "DES key должен содержать ровно 28 HEX-символов";
        return false;
    }
    if (settings.users.empty()) {
        error = "Добавьте хотя бы одного пользователя Newcamd";
        return false;
    }

    std::set<int> ports;
    std::set<std::string> usernames;
    for (const auto& user : settings.users) {
        if (!isSafeName(user.user)) {
            error = "Некорректное имя пользователя: " + user.user;
            return false;
        }
        if (user.password.empty() || user.password.size() > 64 || user.password.find('\n') != std::string::npos || user.password.find('\r') != std::string::npos) {
            error = "Некорректный пароль пользователя " + user.user;
            return false;
        }
        if (user.port < 1 || user.port > 65535) {
            error = "Некорректный порт пользователя " + user.user;
            return false;
        }
        if (!ports.insert(user.port).second) {
            error = "Порт " + std::to_string(user.port) + " используется более одного раза";
            return false;
        }
        if (!usernames.insert(user.user).second) {
            error = "Пользователь " + user.user + " указан более одного раза";
            return false;
        }
        if (!isHex(user.caid, 4)) {
            error = "CAID пользователя " + user.user + " должен содержать 4 HEX-символа";
            return false;
        }
        if (!isHex(user.provider, 6)) {
            error = "Provider пользователя " + user.user + " должен содержать 6 HEX-символов";
            return false;
        }
        if (!validGroups(user.groups)) {
            error = "Некорректные группы пользователя " + user.user;
            return false;
        }
    }

    std::ostringstream conf;
    conf << "[global]\n"
         << "logfile = stdout\n"
         << "clienttimeout = 5000\n"
         << "clientmaxidle = 120\n"
         << "waitforcards = 1\n\n"
         << "[newcamd]\n"
         << "serverip = " << safeIni(settings.bindIp) << "\n"
         << "port = ";
    for (std::size_t i = 0; i < settings.users.size(); ++i) {
        if (i) {
            conf << ';';
        }
        const auto& user = settings.users[i];
        conf << user.port << '@' << safeIni(user.caid) << ':' << safeIni(user.provider);
    }
    conf << "\nkey = " << safeIni(settings.key) << "\n"
         << "keepalive = " << (settings.keepalive ? 1 : 0) << "\n";

    std::ostringstream users;
    for (const auto& user : settings.users) {
        users << "[account]\n"
              << "user = " << safeIni(user.user) << "\n"
              << "pwd = " << safeIni(user.password) << "\n"
              << "group = " << safeIni(user.groups) << "\n"
              << "au = " << (user.au ? 1 : 0) << "\n"
              << "caid = " << safeIni(user.caid) << "\n"
              << "ident = " << safeIni(user.caid) << ':' << safeIni(user.provider) << "\n"
              << "allowedprotocols = newcamd\n\n";
    }

    std::ostringstream server;
    for (const auto& reader : settings.readers) {
        if (reader.label.empty()) {
            error = "У ридера отсутствует имя";
            return false;
        }
        if (reader.device.rfind("/dev/", 0) != 0) {
            error = "Некорректное устройство ридера " + reader.label;
            return false;
        }
        if (!isHex(reader.caid, 4)) {
            error = "CAID ридера " + reader.label + " должен содержать 4 HEX-символа";
            return false;
        }
        if (reader.group < 1 || reader.group > 64) {
            error = "Некорректная группа ридера " + reader.label;
            return false;
        }
        if (reader.mhz < 100 || reader.mhz > 2000 || reader.cardmhz < 100 || reader.cardmhz > 2000) {
            error = "Некорректная частота ридера " + reader.label;
            return false;
        }
        if (reader.protocol != "mouse" && reader.protocol != "phoenix") {
            error = "Поддерживаются только mouse/phoenix: " + reader.label;
            return false;
        }

        server << "[reader]\n"
               << "label = " << safeIni(reader.label) << "\n"
               << "protocol = " << safeIni(reader.protocol) << "\n"
               << "device = " << safeIni(reader.device) << "\n"
               << "caid = " << safeIni(reader.caid) << "\n"
               << "detect = " << safeIni(reader.detect) << "\n"
               << "mhz = " << reader.mhz << "\n"
               << "cardmhz = " << reader.cardmhz << "\n"
               << "group = " << reader.group << "\n";
        if (!reader.ident.empty()) {
            server << "ident = " << safeIni(reader.ident) << "\n";
        }
        if (!reader.emmcache.empty()) {
            server << "emmcache = " << safeIni(reader.emmcache) << "\n";
        }
        if (!reader.enabled) {
            server << "disable = 1\n";
        }
        server << '\n';
    }

    return writeAtomic(std::string(kConfigDir) + "/oscam.conf", conf.str(), error)
        && writeAtomic(std::string(kConfigDir) + "/oscam.user", users.str(), error)
        && writeAtomic(std::string(kConfigDir) + "/oscam.server", server.str(), error);
}

Json::Value OscamMiniManager::statusLocked() {
    Json::Value result;
    result["config_dir"] = kConfigDir;
    result["binary_exists"] = fs::exists(kBinary);

    int rc = 0;
    const std::string active = trim(run("systemctl is-active " + std::string(kService), &rc));
    result["service_active"] = active == "active";
    result["service_state"] = active.empty() ? "unknown" : active;

    Json::Value devices(Json::arrayValue);
    for (const auto& device : ttyDevices()) {
        devices.append(device);
    }
    result["devices"] = devices;
    result["process"] = trim(run("ps -C oscam-mini -o pid=,rss=,vsz=,%cpu=,cmd="));

    std::string log = run("journalctl -u " + std::string(kService) + " -n 40 --no-pager -o cat");
    if (log.size() > 16000) {
        log = log.substr(log.size() - 16000);
    }
    result["log"] = log;
    return result;
}

std::string OscamMiniManager::statusJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    return json(statusLocked());
}

std::string OscamMiniManager::settingsJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    const Settings settings = loadLocked();

    Json::Value result;
    result["bind_ip"] = settings.bindIp;
    result["key"] = settings.key;
    result["keepalive"] = settings.keepalive;

    Json::Value users(Json::arrayValue);
    for (const auto& user : settings.users) {
        Json::Value value;
        value["user"] = user.user;
        value["password"] = user.password;
        value["port"] = user.port;
        value["caid"] = user.caid;
        value["provider"] = user.provider;
        value["groups"] = user.groups;
        value["au"] = user.au;
        users.append(value);
    }
    result["users"] = users;

    Json::Value readersJson(Json::arrayValue);
    for (const auto& reader : settings.readers) {
        Json::Value value;
        value["label"] = reader.label;
        value["protocol"] = reader.protocol;
        value["device"] = reader.device;
        value["caid"] = reader.caid;
        value["detect"] = reader.detect;
        value["mhz"] = reader.mhz;
        value["cardmhz"] = reader.cardmhz;
        value["group"] = reader.group;
        value["ident"] = reader.ident;
        value["emmcache"] = reader.emmcache;
        value["enabled"] = reader.enabled;
        readersJson.append(value);
    }
    result["readers"] = readersJson;
    return json(result);
}

std::string OscamMiniManager::saveSettingsJson(const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    Json::Value input = parse(body, error);
    Json::Value output;

    if (!error.empty()) {
        output["ok"] = false;
        output["error"] = error;
        return json(output);
    }

    Settings settings;
    settings.bindIp = input.get("bind_ip", "127.0.0.1").asString();
    settings.key = input.get("key", "0102030405060708091011121314").asString();
    settings.keepalive = input.get("keepalive", true).asBool();

    if (input["users"].isArray()) {
        for (const auto& item : input["users"]) {
            NewcamdUser user;
            user.user = item.get("user", "tvstreamer").asString();
            user.password = item.get("password", "tvstreamer").asString();
            user.port = item.get("port", 11220).asInt();
            user.caid = item.get("caid", "0652").asString();
            user.provider = item.get("provider", "000000").asString();
            user.groups = item.get("groups", "1").asString();
            user.au = item.get("au", true).asBool();
            settings.users.push_back(user);
        }
    }

    if (input["readers"].isArray()) {
        for (const auto& item : input["readers"]) {
            Reader reader;
            reader.label = item.get("label", "Reader").asString();
            reader.protocol = item.get("protocol", "mouse").asString();
            reader.device = item.get("device", "").asString();
            reader.caid = item.get("caid", "0652").asString();
            reader.detect = item.get("detect", "cd").asString();
            reader.mhz = item.get("mhz", 600).asInt();
            reader.cardmhz = item.get("cardmhz", 600).asInt();
            reader.group = item.get("group", 1).asInt();
            reader.ident = item.get("ident", "").asString();
            reader.emmcache = item.get("emmcache", "1,3,2").asString();
            reader.enabled = item.get("enabled", true).asBool();
            settings.readers.push_back(reader);
        }
    }

    if (!saveLocked(settings, error)) {
        output["ok"] = false;
        output["error"] = error;
        return json(output);
    }

    int rc = 0;
    const std::string restartOutput = run("systemctl restart " + std::string(kService), &rc);
    output["ok"] = rc == 0;
    if (rc != 0) {
        output["error"] = "Конфигурация сохранена, но OSCam-mini не перезапущен: " + trim(restartOutput);
    }
    output["status"] = statusLocked();
    return json(output);
}

std::string OscamMiniManager::serviceActionJson(const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    const Json::Value input = parse(body, error);
    Json::Value output;

    const std::string action = input.get("action", "").asString();
    if (action != "start" && action != "stop" && action != "restart") {
        output["ok"] = false;
        output["error"] = "unsupported action";
        return json(output);
    }

    int rc = 0;
    const std::string commandOutput = run("systemctl " + action + " " + std::string(kService), &rc);
    output["ok"] = rc == 0;
    if (rc != 0) {
        output["error"] = trim(commandOutput);
    }
    output["status"] = statusLocked();
    return json(output);
}

std::string OscamMiniManager::renderPage() {
    return R"HTML(<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OSCam-mini</title>
<style>
body{font-family:Arial,sans-serif;background:#0f1218;color:#eee;margin:0}.w{max-width:1320px;margin:auto;padding:16px}.c{background:#161b25;border:1px solid #2a3241;border-radius:18px;padding:20px;margin:14px 0}.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px}label{display:flex;flex-direction:column;gap:6px;font-size:14px;color:#c6d1e3}input,select{background:#0e131c;border:1px solid #334058;color:#fff;border-radius:10px;padding:11px;font-size:15px}button,a.btn{border:0;border-radius:10px;padding:11px 16px;background:#218cff;color:white;cursor:pointer;text-decoration:none;font-size:15px}.alt{background:#30394a!important}.danger{background:#56313a!important}.item{border-top:1px solid #30394a;padding-top:16px;margin-top:16px}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.grow{flex:1}.status-ok{color:#77e79d}.status-bad{color:#ff8b8b}.msg{margin-left:10px}.error{color:#ff8b8b}.success{color:#77e79d}pre{white-space:pre-wrap;background:#0b0f16;padding:12px;border-radius:10px;max-height:280px;overflow:auto}h2,h3{margin-top:0}.hint{color:#9ba9bd;font-size:13px;margin-top:8px}
</style>
</head>
<body><div class="w">
<div class="row"><a class="btn alt" href="/">← TVStreammerSAT5</a><h2 style="margin:0">OSCam-mini</h2><span id="state"></span></div>
<div class="c"><div class="row"><button onclick="act('start')">Старт</button><button class="alt" onclick="act('restart')">Перезапуск</button><button class="alt" onclick="act('stop')">Стоп</button><button class="alt" onclick="loadAll()">Обновить</button></div><p>Конфиги: /opt/TVStreammerSAT5/oscam-mini/config</p></div>

<div class="c">
  <h3>Newcamd</h3>
  <div class="g"><label>Bind IP<input id="bind_ip"></label><label>DES key<input id="key"></label></div>
  <div class="row" style="margin-top:16px"><h3 style="margin:0">Пользователи / порты</h3><button onclick="addUser()">+ Пользователь</button></div>
  <div class="hint">Для каждого пользователя задаются свой порт, CAID, Provider и группы. Строка port= в oscam.conf формируется автоматически.</div>
  <div id="users"></div>
</div>

<div class="c"><div class="row"><h3 style="margin:0">Phoenix / Smartmouse</h3><button onclick="addReader()">+ Ридер</button></div><div id="readers"></div></div>
<div class="c"><button onclick="save()">Сохранить и перезапустить</button><span id="msg" class="msg"></span></div>
<div class="c"><h3>Процесс / журнал</h3><pre id="proc"></pre><pre id="log"></pre></div>
</div>
<script>
let devices=[];
function esc(v){return String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function api(url,opt){const r=await fetch(url,opt);let data;try{data=await r.json()}catch(e){throw new Error(await r.text()||('HTTP '+r.status))}return data}

function userHtml(u={},i=0){return `<div class="item user"><div class="g">
<label>Пользователь<input class="user-name" value="${esc(u.user||('user'+(i+1)))}"></label>
<label>Пароль<input class="user-pass" type="password" value="${esc(u.password||'')}"></label>
<label>Порт<input class="user-port" type="number" min="1" max="65535" value="${esc(u.port||11220+i)}"></label>
<label>CAID<input class="user-caid" maxlength="4" value="${esc(u.caid||'0652')}"></label>
<label>Provider<input class="user-provider" maxlength="6" value="${esc(u.provider||'000000')}"></label>
<label>Группы<input class="user-groups" value="${esc(u.groups||'1')}" placeholder="6,12"></label>
</div><div class="row" style="margin-top:10px"><label style="flex-direction:row;align-items:center"><input class="user-au" type="checkbox" ${u.au!==false?'checked':''}> AU/EMM</label><button class="danger" onclick="this.closest('.user').remove()">Удалить пользователя</button></div></div>`}
function addUser(u={}){const box=document.createElement('div');box.innerHTML=userHtml(u,document.querySelectorAll('.user').length);users.append(...box.childNodes)}

function readerHtml(r={},i=0){const opts=['',...devices];if(r.device&&!opts.includes(r.device))opts.push(r.device);const options=opts.map(d=>`<option value="${esc(d)}" ${d===r.device?'selected':''}>${esc(d)}</option>`).join('');return `<div class="item reader"><div class="g">
<label>Имя<input class="reader-label" value="${esc(r.label||('Reader'+(i+1)))}"></label>
<label>Устройство<select class="reader-device">${options}</select></label>
<label>CAID<input class="reader-caid" maxlength="4" value="${esc(r.caid||'0652')}"></label>
<label>Протокол<select class="reader-protocol"><option value="mouse" ${r.protocol==='mouse'?'selected':''}>mouse</option><option value="phoenix" ${r.protocol==='phoenix'?'selected':''}>phoenix</option></select></label>
<label>MHz<input class="reader-mhz" type="number" value="${esc(r.mhz||600)}"></label>
<label>Card MHz<input class="reader-cardmhz" type="number" value="${esc(r.cardmhz||600)}"></label>
<label>Group<input class="reader-group" type="number" min="1" max="64" value="${esc(r.group||1)}"></label>
<label>Ident<input class="reader-ident" value="${esc(r.ident||'')}"></label>
</div><div class="row" style="margin-top:10px"><button class="danger" onclick="this.closest('.reader').remove()">Удалить ридер</button></div></div>`}
function addReader(r={}){const box=document.createElement('div');box.innerHTML=readerHtml(r,document.querySelectorAll('.reader').length);readers.append(...box.childNodes)}

async function loadAll(){try{const st=await api('/api/oscam-mini/status');devices=st.devices||[];state.textContent=st.service_active?'● работает':'● '+(st.service_state||'остановлен');state.className=st.service_active?'status-ok':'status-bad';proc.textContent=st.process||'процесс не запущен';log.textContent=st.log||'';const s=await api('/api/oscam-mini/settings');bind_ip.value=s.bind_ip||'127.0.0.1';key.value=s.key||'';users.innerHTML='';(s.users||[]).forEach(addUser);readers.innerHTML='';(s.readers||[]).forEach(addReader)}catch(e){msg.textContent=e.message;msg.className='msg error'}}

async function save(){const userRows=[...document.querySelectorAll('.user')].map(e=>({user:e.querySelector('.user-name').value,password:e.querySelector('.user-pass').value,port:+e.querySelector('.user-port').value,caid:e.querySelector('.user-caid').value.trim(),provider:e.querySelector('.user-provider').value.trim(),groups:e.querySelector('.user-groups').value.trim(),au:e.querySelector('.user-au').checked}));const readerRows=[...document.querySelectorAll('.reader')].map(e=>({label:e.querySelector('.reader-label').value,device:e.querySelector('.reader-device').value,caid:e.querySelector('.reader-caid').value.trim(),protocol:e.querySelector('.reader-protocol').value,detect:'cd',mhz:+e.querySelector('.reader-mhz').value,cardmhz:+e.querySelector('.reader-cardmhz').value,group:+e.querySelector('.reader-group').value,ident:e.querySelector('.reader-ident').value.trim(),emmcache:'1,3,2',enabled:true}));const body={bind_ip:bind_ip.value.trim(),key:key.value.trim(),keepalive:true,users:userRows,readers:readerRows};msg.textContent='Сохранение…';msg.className='msg';try{const x=await api('/api/oscam-mini/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});msg.textContent=x.ok?'Сохранено и перезапущено':(x.error||'Ошибка');msg.className=x.ok?'msg success':'msg error';await loadAll()}catch(e){msg.textContent=e.message;msg.className='msg error'}}

async function act(action){try{const x=await api('/api/oscam-mini/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action})});if(!x.ok){msg.textContent=x.error||'Ошибка';msg.className='msg error'}await loadAll()}catch(e){msg.textContent=e.message;msg.className='msg error'}}
loadAll();
</script>
</body></html>)HTML";
}
