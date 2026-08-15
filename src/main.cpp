#include <iostream>
#include <boost/asio.hpp>
#include <gst/gst.h>

#include "ConfigManager.h"
#include "CardManager.h"
#include "TelegramNotifier.h"
#include "StreamManager.h"
#include "HttpServer.h"
#include "AppVersion.h"

int main() {
    std::cerr << tvs::app::kProductName
              << " " << tvs::app::kProgramRelease
              << " / " << tvs::app::kProgramVersion
              << " | support=" << tvs::app::kSupportEmail << std::endl;
    std::cerr << "main() entered" << std::endl;

    // Initialize GStreamer before HttpServer is created. The web UI queries
    // transcoder capabilities during startup, and GstElementFactory lookups
    // return no factories until the GStreamer registry has been initialized.
    GError* gstError = nullptr;
    if (!gst_init_check(nullptr, nullptr, &gstError)) {
        std::cerr << "GStreamer initialization failed";
        if (gstError && gstError->message) {
            std::cerr << ": " << gstError->message;
        }
        std::cerr << std::endl;
        if (gstError) g_error_free(gstError);
        return 1;
    }
    std::cerr << "GStreamer initialized" << std::endl;

    ConfigManager configManager;
    if (!configManager.load()) {
        std::cerr << "Unable to load or create configuration." << std::endl;
        return 1;
    }

    std::cerr << "Config loaded: http_port=" << configManager.config.httpPort
              << " login=" << configManager.config.login << std::endl;

    CardManager::instance().configure(configManager.config.camClients);

    TelegramNotifier notifier(configManager);
    StreamManager streamManager(configManager, notifier);

    boost::asio::io_context ioc;
    HttpServer server(ioc, configManager, streamManager);

    if (!server.start()) {
        std::cerr << "HTTP server start failed" << std::endl;
        return 1;
    }

    std::cerr << "HTTP server started" << std::endl;
    for (const auto& stream : configManager.config.streams) {
        if (stream.autoStart) {
            std::cerr << "Auto-starting stream: " << stream.id << std::endl;
            streamManager.startStream(stream);
        }
    }
    std::cout << "TVStreammerSAT5 running on port " << configManager.config.httpPort << std::endl;
    std::cerr << "Calling ioc.run()" << std::endl;

    ioc.run();
    return 0;
}
