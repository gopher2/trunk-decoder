/*
 * Console Output Plugin
 * Outputs P25 TSBK data to console for debugging and testing
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 */

#include "../src/plugin_api.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <sstream>

class ConsoleOutput : public Base_Output_Plugin {
private:
    bool verbose_;
    bool show_hex_dump_;
    size_t max_hex_bytes_;
    uint64_t messages_processed_;
    
public:
    PLUGIN_INFO("Console Output", "1.0.0", "Dave K9DPD", "Outputs P25 TSBK data to console")
    
    ConsoleOutput() : 
        verbose_(true),
        show_hex_dump_(false),
        max_hex_bytes_(32),
        messages_processed_(0) {}
    
    virtual ~ConsoleOutput() {}
    
    virtual int init(json config_data) override {
        if (parse_config(config_data) != 0) {
            set_state(Plugin_State::PLUGIN_ERROR);
            return -1;
        }
        
        set_state(Plugin_State::PLUGIN_INITIALIZED);
        return 0;
    }
    
    virtual int start() override {
        if (state_ != Plugin_State::PLUGIN_INITIALIZED) {
            return -1;
        }
        
        set_state(Plugin_State::PLUGIN_RUNNING);
        
        if (verbose_) {
            std::cout << "[ConsoleOutput] Started console output plugin" << std::endl;
        }
        
        return 0;
    }
    
    virtual int stop() override {
        if (state_ == Plugin_State::PLUGIN_RUNNING) {
            set_state(Plugin_State::PLUGIN_STOPPED);
            
            if (verbose_) {
                std::cout << "[ConsoleOutput] Stopped console output plugin. " 
                          << "Messages processed: " << messages_processed_ << std::endl;
            }
        }
        
        return 0;
    }
    
    virtual int parse_config(json config_data) override {
        config_ = config_data;
        
        if (config_data.contains("verbose")) {
            verbose_ = config_data["verbose"];
        }
        
        if (config_data.contains("show_hex_dump")) {
            show_hex_dump_ = config_data["show_hex_dump"];
        }
        
        if (config_data.contains("max_hex_bytes")) {
            max_hex_bytes_ = config_data["max_hex_bytes"];
        }
        
        return 0;
    }
    
    virtual int process_data(const P25_TSBK_Data& data) override {
        if (state_ != Plugin_State::PLUGIN_RUNNING) {
            return -1;
        }
        
        messages_processed_++;
        
        // Format timestamp
        auto timestamp = std::chrono::microseconds(data.timestamp_us);
        auto time_t_val = std::chrono::duration_cast<std::chrono::seconds>(timestamp).count();
        auto microseconds = timestamp.count() % 1000000;
        time_t time_val = static_cast<time_t>(time_t_val);
        
        std::cout << "=== P25 TSBK Message ===" << std::endl;
        std::cout << "Timestamp: " << std::put_time(std::localtime(&time_val), "%Y-%m-%d %H:%M:%S");
        std::cout << "." << std::setfill('0') << std::setw(6) << microseconds << std::endl;
        std::cout << "Sequence:  " << data.sequence_number << std::endl;
        std::cout << "NAC:       0x" << std::hex << std::uppercase << data.nac << std::dec << std::endl;
        std::cout << "Site ID:   " << data.site_id << std::endl;
        std::cout << "Frequency: " << std::fixed << std::setprecision(6) << data.frequency << " Hz" << std::endl;
        std::cout << "Data Size: " << data.data_length << " bytes" << std::endl;
        std::cout << "Source:    " << data.source_name << std::endl;
        
        if (show_hex_dump_ && !data.tsbk_data.empty()) {
            std::cout << "Hex Data:  ";
            size_t bytes_to_show = std::min(data.tsbk_data.size(), max_hex_bytes_);
            for (size_t i = 0; i < bytes_to_show; ++i) {
                std::cout << std::hex << std::setfill('0') << std::setw(2) 
                          << static_cast<unsigned int>(data.tsbk_data[i]) << " ";
            }
            if (bytes_to_show < data.tsbk_data.size()) {
                std::cout << "... (" << (data.tsbk_data.size() - bytes_to_show) << " more bytes)";
            }
            std::cout << std::dec << std::endl;
        }
        
        std::cout << "========================" << std::endl << std::endl;
        
        return 0;
    }
    
    virtual json get_stats() override {
        json stats = Base_Output_Plugin::get_stats();
        stats["messages_processed"] = messages_processed_;
        stats["verbose"] = verbose_;
        stats["show_hex_dump"] = show_hex_dump_;
        return stats;
    }
};

// Plugin factory
extern "C" std::shared_ptr<Output_Plugin_Api> create_output_plugin() {
    return std::make_shared<ConsoleOutput>();
}