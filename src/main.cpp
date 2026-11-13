#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include "config/config_manager.hpp"
#include "config/config_watcher.hpp"
#include "orchestrator/orchestrator.hpp"

namespace {
    std::atomic<bool> g_shutdown_requested{false};

    void signal_handler(int signal) {
        if (signal == SIGINT || signal == SIGTERM) {
            std::cout << "\n[MAIN] Caught termination signal (" << signal << "). Signaling shutdown..." << std::endl;
            g_shutdown_requested.store(true);
        }
    }
}

int main(int argc, char* argv[]) {
    std::string config_file = "config.json";
    if (argc > 1) {
        config_file = argv[1];
    }

    std::cout << "===============================================================\n"
              << "  RELIANCE AI VISION - INDUSTRIAL C++ VIDEO ANALYTICS ENGINE   \n"
              << "  Granular Multi-Threaded Architecture with Zero-Loss Reload  \n"
              << "===============================================================\n" << std::endl;

    // 1. Register OS Signal Handlers (SIGINT / SIGTERM)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 2. Load Initial Configuration
    auto config_mgr = std::make_shared<engine::config::ConfigManager>();
    if (!config_mgr->load(config_file)) {
        std::cerr << "[MAIN] Fatal: Failed to load config from " << config_file << std::endl;
        return 1;
    }

    // 3. Instantiate the Orchestrator
    auto orchestrator = std::make_shared<engine::orchestrator::Orchestrator>();
    orchestrator->initialize(config_mgr->get_config());

    // 4. Start the Dedicated Config Watcher Thread
    auto watcher = std::make_unique<engine::config::ConfigWatcher>(
        config_mgr, 
        config_file, 
        [orchestrator](const engine::GlobalConfig& updated_config) {
            orchestrator->reload(updated_config);
        }
    );
    watcher->start();

    std::cout << "[MAIN] Engine is live! Edit '" << config_file << "' at any time to test live Granular Hot-Reloading.\n"
              << "[MAIN] Press Ctrl+C to terminate cleanly.\n" << std::endl;

    // 5. Main Control Loop: waits for OS shutdown signal
    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 6. Orderly Shutdown
    std::cout << "[MAIN] Shutting down Config Watcher..." << std::endl;
    watcher->stop();

    std::cout << "[MAIN] Shutting down Orchestrator..." << std::endl;
    orchestrator->shutdown();

    std::cout << "[MAIN] Engine exited cleanly. All resources destructed." << std::endl;
    return 0;
}

