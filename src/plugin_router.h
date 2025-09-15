/*
 * Plugin Router for trunk-decoder
 * Manages routing connections between input and output plugins
 * Supports 1:1, 1:many, many:1, and many:many routing configurations
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 */

#pragma once

#include "plugin_api.h"
#include "input_plugin_manager.h"
#include "output_plugin_manager.h"
#include <vector>
#include <map>
#include <set>
#include <string>
#include <functional>
#include <memory>

class PluginRouter {
public:
    struct RoutingRule {
        std::string input_plugin;      // Source plugin name or "*" for all
        std::vector<std::string> output_plugins; // Target plugin names
        std::function<bool(const P25_TSBK_Data&)> filter; // Optional data filter
        bool enabled;
        
        RoutingRule(const std::string& input, const std::vector<std::string>& outputs) 
            : input_plugin(input), output_plugins(outputs), enabled(true) {}
    };

private:
    std::shared_ptr<InputPluginManager> input_manager_;
    std::shared_ptr<OutputPluginManager> output_manager_;
    std::vector<RoutingRule> routing_rules_;
    bool verbose_;
    
    // Statistics
    std::map<std::string, uint64_t> messages_routed_;
    std::map<std::string, uint64_t> messages_filtered_;
    std::map<std::string, uint64_t> routing_errors_;

public:
    PluginRouter(std::shared_ptr<InputPluginManager> input_mgr, 
                 std::shared_ptr<OutputPluginManager> output_mgr,
                 bool verbose = false) 
        : input_manager_(input_mgr), output_manager_(output_mgr), verbose_(verbose) {}
    
    // Add a routing rule
    void add_route(const std::string& input_plugin, const std::vector<std::string>& output_plugins) {
        routing_rules_.emplace_back(input_plugin, output_plugins);
        
        if (verbose_) {
            std::cout << "[PluginRouter] Added route: " << input_plugin << " -> [";
            for (size_t i = 0; i < output_plugins.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << output_plugins[i];
            }
            std::cout << "]" << std::endl;
        }
    }
    
    // Add a routing rule with filter function
    void add_route_with_filter(const std::string& input_plugin, 
                              const std::vector<std::string>& output_plugins,
                              std::function<bool(const P25_TSBK_Data&)> filter) {
        routing_rules_.emplace_back(input_plugin, output_plugins);
        routing_rules_.back().filter = filter;
        
        if (verbose_) {
            std::cout << "[PluginRouter] Added filtered route: " << input_plugin << " -> [";
            for (size_t i = 0; i < output_plugins.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << output_plugins[i];
            }
            std::cout << "] (with filter)" << std::endl;
        }
    }
    
    // Load routing rules from configuration
    int load_routes_from_config(const json& config) {
        if (!config.contains("routing_rules")) {
            // Default: route all inputs to all outputs
            add_route("*", output_manager_->get_active_plugin_names());
            return 0;
        }
        
        try {
            for (const auto& rule : config["routing_rules"]) {
                std::string input = rule["input"];
                std::vector<std::string> outputs = rule["outputs"];
                bool enabled = rule.value("enabled", true);
                
                if (enabled) {
                    add_route(input, outputs);
                }
            }
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "[PluginRouter] Error loading routes from config: " << e.what() << std::endl;
            return -1;
        }
    }
    
    // Route data from input plugin to appropriate output plugins
    void route_data(const P25_TSBK_Data& data, const std::string& source_plugin) {
        for (const auto& rule : routing_rules_) {
            if (!rule.enabled) continue;
            
            // Check if this rule applies to the source plugin
            bool applies = (rule.input_plugin == "*" || rule.input_plugin == source_plugin);
            if (!applies) continue;
            
            // Apply filter if present
            if (rule.filter && !rule.filter(data)) {
                messages_filtered_[source_plugin]++;
                continue;
            }
            
            // Route to output plugins
            try {
                output_manager_->send_data_to(data, rule.output_plugins);
                messages_routed_[source_plugin]++;
                
                if (verbose_) {
                    std::cout << "[PluginRouter] Routed data from " << source_plugin 
                              << " to " << rule.output_plugins.size() << " outputs" << std::endl;
                }
            } catch (const std::exception& e) {
                routing_errors_[source_plugin]++;
                std::cerr << "[PluginRouter] Error routing data from " << source_plugin 
                          << ": " << e.what() << std::endl;
            }
        }
    }
    
    // Enable/disable specific routing rules
    void enable_route(const std::string& input_plugin, const std::vector<std::string>& output_plugins, bool enabled) {
        for (auto& rule : routing_rules_) {
            if (rule.input_plugin == input_plugin && rule.output_plugins == output_plugins) {
                rule.enabled = enabled;
                if (verbose_) {
                    std::cout << "[PluginRouter] " << (enabled ? "Enabled" : "Disabled") 
                              << " route: " << input_plugin << " -> outputs" << std::endl;
                }
            }
        }
    }
    
    // Get routing statistics
    json get_routing_stats() {
        json stats;
        stats["messages_routed"] = messages_routed_;
        stats["messages_filtered"] = messages_filtered_;
        stats["routing_errors"] = routing_errors_;
        stats["active_rules"] = 0;
        
        for (const auto& rule : routing_rules_) {
            if (rule.enabled) {
                stats["active_rules"] = stats["active_rules"].get<int>() + 1;
            }
        }
        
        return stats;
    }
    
    // Get current routing configuration
    json get_routing_config() {
        json config = json::array();
        
        for (const auto& rule : routing_rules_) {
            json rule_config;
            rule_config["input"] = rule.input_plugin;
            rule_config["outputs"] = rule.output_plugins;
            rule_config["enabled"] = rule.enabled;
            rule_config["has_filter"] = (rule.filter != nullptr);
            config.push_back(rule_config);
        }
        
        return config;
    }
    
    // Clear all routing rules
    void clear_routes() {
        routing_rules_.clear();
        if (verbose_) {
            std::cout << "[PluginRouter] Cleared all routing rules" << std::endl;
        }
    }
    
    // Validate routing configuration
    bool validate_routes() {
        auto active_inputs = input_manager_->get_active_plugin_names();
        auto active_outputs = output_manager_->get_active_plugin_names();
        
        std::set<std::string> input_set(active_inputs.begin(), active_inputs.end());
        std::set<std::string> output_set(active_outputs.begin(), active_outputs.end());
        
        bool valid = true;
        
        for (const auto& rule : routing_rules_) {
            if (!rule.enabled) continue;
            
            // Validate input plugin (except wildcard)
            if (rule.input_plugin != "*" && input_set.find(rule.input_plugin) == input_set.end()) {
                std::cerr << "[PluginRouter] Invalid input plugin in route: " << rule.input_plugin << std::endl;
                valid = false;
            }
            
            // Validate output plugins
            for (const auto& output : rule.output_plugins) {
                if (output_set.find(output) == output_set.end()) {
                    std::cerr << "[PluginRouter] Invalid output plugin in route: " << output << std::endl;
                    valid = false;
                }
            }
        }
        
        return valid;
    }
    
private:
    // Helper method to get input plugin names (need to add this to InputPluginManager)
    std::vector<std::string> get_active_input_names() {
        // This would need to be implemented in InputPluginManager
        return {}; // Placeholder
    }
};