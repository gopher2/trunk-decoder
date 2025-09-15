/*
 * Test program for P25 TSBK UDP Input Plugin
 * Loads the plugin and tests receiving data from trunk-recorder
 */

#include "src/plugin_api.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <boost/dll/import.hpp>

namespace dll = boost::dll;

int main() {
    try {
        // Load the plugin
        std::cout << "Loading P25 TSBK UDP Input plugin..." << std::endl;
        
        boost::function<boost::shared_ptr<Input_Plugin_Api>()> creator;
        creator = dll::import_alias<boost::shared_ptr<Input_Plugin_Api>()>(
            "./plugins/libp25_tsbk_udp_input.so",
            "create_input_plugin",
            dll::load_mode::append_decorations
        );
        
        auto plugin = creator();
        
        std::cout << "Plugin loaded: " << plugin->get_plugin_name() 
                  << " v" << plugin->get_plugin_version() << std::endl;
        
        // Configure plugin
        json config;
        config["listen_address"] = "127.0.0.1";
        config["listen_port"] = 9999;
        config["verbose"] = true;
        config["validate_checksums"] = true;
        config["max_queue_size"] = 100;
        
        // Initialize and start
        if (plugin->init(config) != 0) {
            std::cerr << "Failed to initialize plugin" << std::endl;
            return 1;
        }
        
        if (plugin->start() != 0) {
            std::cerr << "Failed to start plugin" << std::endl;
            return 1;
        }
        
        std::cout << "Plugin started. Listening for P25 TSBK data..." << std::endl;
        std::cout << "Make sure trunk-recorder is streaming to 127.0.0.1:9999" << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
        
        // Set up data callback
        plugin->set_data_callback([](P25_TSBK_Data data) {
            std::cout << "Received P25 TSBK data:" << std::endl;
            std::cout << "  Magic: 0x" << std::hex << data.magic << std::dec << std::endl;
            std::cout << "  NAC: 0x" << std::hex << data.nac << std::dec << std::endl;
            std::cout << "  Frequency: " << data.frequency << " Hz" << std::endl;
            std::cout << "  Sequence: " << data.sequence_number << std::endl;
            std::cout << "  Data length: " << data.data_length << " bytes" << std::endl;
            std::cout << "  Source: " << data.source_name << std::endl;
            std::cout << "---" << std::endl;
        });
        
        // Run for a while to collect data
        auto start_time = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(30)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // Print stats every 5 seconds
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed.count() % 5000000000 == 0) { // Every 5 seconds (nanoseconds)
                json stats = plugin->get_stats();
                std::cout << "Stats: " << stats.dump(2) << std::endl;
            }
        }
        
        // Stop plugin
        plugin->stop();
        std::cout << "Plugin stopped" << std::endl;
        
        // Final stats
        json final_stats = plugin->get_stats();
        std::cout << "Final stats: " << final_stats.dump(2) << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}