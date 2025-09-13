/*
 * trunk-decoder Plugin Manager
 * Based on trunk-recorder's plugin manager
 */

#pragma once

#include "plugin_api.h"
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <boost/dll/shared_library.hpp>
#include <boost/dll/import.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Plugin information structure
struct Plugin_Info {
    std::string name;
    std::string library_path;
    bool enabled;
    json config;
    Plugin_State state;
    
    // Dynamic loading
    boost::dll::shared_library plugin_lib;
    boost::function<boost::shared_ptr<Plugin_Api>()> creator;
    boost::shared_ptr<Plugin_Api> api;
    
    // Statistics
    int calls_processed;
    int calls_failed;
    std::chrono::system_clock::time_point last_activity;
    
    Plugin_Info() : enabled(false), state(Plugin_State::PLUGIN_UNINITIALIZED),
                   calls_processed(0), calls_failed(0),
                   last_activity(std::chrono::system_clock::now()) {}
};

// Plugin Manager class
class Plugin_Manager {
private:
    std::vector<std::shared_ptr<Plugin_Info>> plugins_;
    std::map<std::string, std::shared_ptr<Plugin_Info>> plugins_by_name_;
    bool initialized_;
    
    // Plugin directories to search
    std::vector<std::string> plugin_directories_;
    
    // Retry mechanism
    struct Plugin_Retry {
        std::shared_ptr<Plugin_Info> plugin;
        Call_Data_t call_data;
        std::string operation;
        int retry_count;
        std::chrono::system_clock::time_point next_retry;
    };
    std::vector<Plugin_Retry> retry_queue_;
    
    // Helper methods
    bool load_plugin(std::shared_ptr<Plugin_Info> plugin_info);
    bool unload_plugin(std::shared_ptr<Plugin_Info> plugin_info);
    void process_retry_queue();
    std::string find_plugin_library(const std::string& library_name);
    
public:
    Plugin_Manager();
    ~Plugin_Manager();
    
    // Plugin management
    bool initialize(const json& config);
    bool start_plugins();
    bool stop_plugins();
    bool shutdown();
    
    // Plugin loading
    bool load_plugin_config(const json& plugin_config);
    bool reload_plugin(const std::string& plugin_name);
    bool enable_plugin(const std::string& plugin_name, bool enable = true);
    
    // Event dispatching
    void call_start(Call_Data_t* call_info);
    void call_end(Call_Data_t call_info);
    void call_data_ready(Call_Data_t call_info);
    void audio_stream(Call_Data_t* call_info, int16_t* samples, int sample_count);
    void system_started(System_Info system_info);
    void system_stopped(System_Info system_info);
    
    // Plugin information
    std::vector<std::string> get_plugin_names() const;
    json get_plugin_stats(const std::string& plugin_name) const;
    json get_all_plugin_stats() const;
    bool is_plugin_enabled(const std::string& plugin_name) const;
    Plugin_State get_plugin_state(const std::string& plugin_name) const;
    
    // Configuration
    void add_plugin_directory(const std::string& directory);
    std::vector<std::string> get_plugin_directories() const;
    
    // Health monitoring
    bool is_healthy() const;
    int get_active_plugin_count() const;
    int get_failed_plugin_count() const;
    
    // Utility
    void poll_plugins(); // Process retries and maintenance
};

// Global plugin manager instance
extern std::unique_ptr<Plugin_Manager> plugin_manager;