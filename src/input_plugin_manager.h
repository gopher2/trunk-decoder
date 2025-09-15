/*
 * Input Plugin Manager for trunk-decoder
 * Manages input plugins for receiving P25 TSBK data
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 */

#pragma once

#include "plugin_api.h"
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <boost/dll/import.hpp>

namespace dll = boost::dll;

class InputPluginManager {
private:
    struct InputPluginInfo {
        std::string name;
        std::string library_path;
        std::shared_ptr<Input_Plugin_Api> plugin;
        json config;
        bool enabled;
        
        InputPluginInfo(const std::string& n, const std::string& path) 
            : name(n), library_path(path), enabled(true) {}
    };
    
    std::vector<InputPluginInfo> plugins_;
    std::function<void(P25_TSBK_Data)> data_callback_;
    bool verbose_;
    
public:
    InputPluginManager(bool verbose = false) : verbose_(verbose) {}
    
    ~InputPluginManager() {
        stop_all();
    }
    
    // Add a plugin to be loaded
    int add_plugin(const std::string& name, const std::string& library_path, const json& config) {
        plugins_.emplace_back(name, library_path);
        plugins_.back().config = config;
        plugins_.back().enabled = config.value("enabled", true);
        return 0;
    }
    
    // Load and initialize all plugins
    int initialize_all() {
        for (auto& plugin_info : plugins_) {
            if (!plugin_info.enabled) {
                if (verbose_) {
                    std::cout << "[InputPluginManager] Skipping disabled plugin: " << plugin_info.name << std::endl;
                }
                continue;
            }
            
            if (load_plugin(plugin_info) != 0) {
                std::cerr << "[InputPluginManager] Failed to load plugin: " << plugin_info.name << std::endl;
                return -1;
            }
        }
        return 0;
    }
    
    // Start all loaded plugins
    int start_all() {
        for (auto& plugin_info : plugins_) {
            if (!plugin_info.enabled || !plugin_info.plugin) {
                continue;
            }
            
            if (plugin_info.plugin->start() != 0) {
                std::cerr << "[InputPluginManager] Failed to start plugin: " << plugin_info.name << std::endl;
                return -1;
            }
            
            if (verbose_) {
                std::cout << "[InputPluginManager] Started plugin: " << plugin_info.name << std::endl;
            }
        }
        return 0;
    }
    
    // Stop all plugins
    int stop_all() {
        for (auto& plugin_info : plugins_) {
            if (plugin_info.plugin) {
                plugin_info.plugin->stop();
                if (verbose_) {
                    std::cout << "[InputPluginManager] Stopped plugin: " << plugin_info.name << std::endl;
                }
            }
        }
        return 0;
    }
    
    // Set data callback for all plugins
    void set_data_callback(std::function<void(P25_TSBK_Data)> callback) {
        data_callback_ = callback;
        
        for (auto& plugin_info : plugins_) {
            if (plugin_info.plugin) {
                plugin_info.plugin->set_data_callback(callback);
            }
        }
    }
    
    // Get statistics from all plugins
    json get_all_stats() {
        json all_stats = json::array();
        
        for (const auto& plugin_info : plugins_) {
            if (plugin_info.plugin) {
                json plugin_stats = plugin_info.plugin->get_stats();
                plugin_stats["plugin_name"] = plugin_info.name;
                plugin_stats["library_path"] = plugin_info.library_path;
                all_stats.push_back(plugin_stats);
            }
        }
        
        return all_stats;
    }
    
    // Check if any plugin has data available
    bool has_data() {
        for (auto& plugin_info : plugins_) {
            if (plugin_info.plugin && plugin_info.plugin->has_data()) {
                return true;
            }
        }
        return false;
    }
    
    // Get list of active plugin names
    std::vector<std::string> get_active_plugin_names() {
        std::vector<std::string> names;
        for (const auto& plugin_info : plugins_) {
            if (plugin_info.plugin && plugin_info.enabled) {
                names.push_back(plugin_info.name);
            }
        }
        return names;
    }
    
    // Get data from the first plugin that has it
    P25_TSBK_Data get_data() {
        for (auto& plugin_info : plugins_) {
            if (plugin_info.plugin && plugin_info.plugin->has_data()) {
                return plugin_info.plugin->get_data();
            }
        }
        return P25_TSBK_Data(); // Empty data
    }
    
private:
    int load_plugin(InputPluginInfo& plugin_info) {
        try {
            if (verbose_) {
                std::cout << "[InputPluginManager] Loading plugin: " << plugin_info.name 
                          << " from " << plugin_info.library_path << std::endl;
            }
            
            // Load the plugin library
            std::function<std::shared_ptr<Input_Plugin_Api>()> creator;
            creator = dll::import_alias<std::shared_ptr<Input_Plugin_Api>()>(
                plugin_info.library_path,
                "create_input_plugin",
                dll::load_mode::append_decorations
            );
            
            // Create plugin instance
            plugin_info.plugin = creator();
            
            if (verbose_) {
                std::cout << "[InputPluginManager] Created plugin: " 
                          << plugin_info.plugin->get_plugin_name()
                          << " v" << plugin_info.plugin->get_plugin_version() << std::endl;
            }
            
            // Initialize plugin
            if (plugin_info.plugin->init(plugin_info.config) != 0) {
                std::cerr << "[InputPluginManager] Failed to initialize plugin: " << plugin_info.name << std::endl;
                plugin_info.plugin.reset();
                return -1;
            }
            
            // Set data callback if we have one
            if (data_callback_) {
                plugin_info.plugin->set_data_callback(data_callback_);
            }
            
            return 0;
            
        } catch (const std::exception& e) {
            std::cerr << "[InputPluginManager] Error loading plugin " << plugin_info.name 
                      << ": " << e.what() << std::endl;
            return -1;
        }
    }
};