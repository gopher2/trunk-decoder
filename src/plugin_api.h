/*
 * trunk-decoder Plugin API
 * Based on trunk-recorder's plugin architecture
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 */

#pragma once

#include <vector>
#include <string>
#include <memory>
#include <map>
#include <functional>
#include <boost/dll/alias.hpp>
#include <boost/shared_ptr.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Forward declarations
struct Call_Data_t;
struct System_Info;
struct P25_TSBK_Data;

// Plugin lifecycle states
enum class Plugin_State {
    PLUGIN_UNINITIALIZED,
    PLUGIN_INITIALIZED,
    PLUGIN_RUNNING,
    PLUGIN_STOPPED,
    PLUGIN_ERROR
};

// Call data structure (simplified from trunk-recorder)
struct Call_Data_t {
    // Core call info
    long talkgroup;
    long source_id;
    long call_num;
    double freq;
    long start_time;
    long stop_time;
    bool encrypted;
    bool emergency;
    
    // System info
    std::string system_short_name;
    std::string system_name;
    uint16_t nac;
    uint32_t wacn;
    uint8_t rfss;
    uint8_t site_id;
    std::string site_name;
    
    // File paths
    char wav_filename[512];
    char json_filename[512];
    std::map<std::string, std::string> converted_files; // format -> filepath
    
    // Rich metadata
    json call_json;
    
    // Processing info
    std::string stream_name;
    int priority;
    std::chrono::system_clock::time_point processing_start;
    
    Call_Data_t() : talkgroup(0), source_id(0), call_num(0), freq(0.0),
                   start_time(0), stop_time(0), encrypted(false), emergency(false),
                   nac(0), wacn(0), rfss(0), site_id(0), priority(1) {
        wav_filename[0] = '\0';
        json_filename[0] = '\0';
        processing_start = std::chrono::system_clock::now();
    }
};

// System information
struct System_Info {
    std::string short_name;
    std::string name;
    uint16_t nac;
    uint32_t wacn;
    std::vector<double> control_channels;
    bool encrypted_calls_allowed;
    std::map<std::string, std::string> config;
};

// P25 TSBK data structure for input plugins
struct P25_TSBK_Data {
    uint32_t magic;              // 0x50323543 ("P25C")
    uint32_t version;            // Protocol version
    uint64_t timestamp_us;       // Microsecond timestamp
    uint32_t sequence_number;    // For ordering/loss detection
    uint32_t system_id;          // System identifier (NAC)
    uint32_t site_id;            // Site identifier
    double frequency;            // Control channel frequency
    uint32_t sample_rate;        // Sample rate of data
    uint16_t data_length;        // Length of P25 data
    uint16_t checksum;           // Simple integrity check
    std::vector<uint8_t> tsbk_data; // Raw P25 TSBK data
    
    // Derived/processed fields
    std::string source_name;     // Input plugin name
    uint64_t received_time;      // When received by input plugin
};

// Base plugin API class
class Plugin_Api {
public:
    virtual ~Plugin_Api() = default;
    
    // Plugin lifecycle
    virtual int init(json config_data) = 0;
    virtual int start() = 0;
    virtual int stop() = 0;
    virtual int get_state() = 0;
    
    // Core processing hooks
    virtual int call_start(Call_Data_t* call_info) = 0;
    virtual int call_end(Call_Data_t call_info) = 0;
    virtual int call_data_ready(Call_Data_t call_info) = 0; // All files ready
    
    // Audio processing (for streaming plugins)
    virtual int audio_stream(Call_Data_t* call_info, int16_t* samples, int sample_count) = 0;
    
    // System events
    virtual int system_started(System_Info system_info) = 0;
    virtual int system_stopped(System_Info system_info) = 0;
    
    // Configuration
    virtual int parse_config(json config_data) = 0;
    virtual json get_stats() = 0;
    virtual bool is_enabled() = 0;
    
    // Plugin metadata
    virtual std::string get_plugin_name() = 0;
    virtual std::string get_plugin_version() = 0;
    virtual std::string get_plugin_author() = 0;
    virtual std::string get_plugin_description() = 0;
    
protected:
    Plugin_State state_ = Plugin_State::PLUGIN_UNINITIALIZED;
    json config_;
    std::string plugin_name_;
    bool enabled_ = true;
};

// Plugin factory function typedef
typedef boost::shared_ptr<Plugin_Api> pluginapi_create_t();

// Helper macros for plugin development
#define TRUNK_DECODER_PLUGIN_FACTORY(PluginClass) \
    class PluginClass; \
    static boost::shared_ptr<Plugin_Api> create_plugin() { \
        return boost::shared_ptr<Plugin_Api>(new PluginClass()); \
    } \
    BOOST_DLL_ALIAS(create_plugin, create_plugin)

#define PLUGIN_INFO(name, version, author, description) \
    virtual std::string get_plugin_name() override { return name; } \
    virtual std::string get_plugin_version() override { return version; } \
    virtual std::string get_plugin_author() override { return author; } \
    virtual std::string get_plugin_description() override { return description; }

// Base plugin implementation with common functionality
class Base_Plugin : public Plugin_Api {
public:
    Base_Plugin() = default;
    virtual ~Base_Plugin() = default;
    
    // Default implementations
    virtual int get_state() override { return static_cast<int>(state_); }
    virtual bool is_enabled() override { return enabled_; }
    
    virtual int call_start(Call_Data_t* call_info) override { return 0; }
    virtual int audio_stream(Call_Data_t* call_info, int16_t* samples, int sample_count) override { return 0; }
    virtual int system_started(System_Info system_info) override { return 0; }
    virtual int system_stopped(System_Info system_info) override { return 0; }
    
    virtual json get_stats() override {
        json stats;
        stats["plugin_name"] = get_plugin_name();
        stats["state"] = static_cast<int>(state_);
        stats["enabled"] = enabled_;
        return stats;
    }
    
protected:
    void set_state(Plugin_State state) { state_ = state; }
};

// Input Plugin API for receiving P25 TSBK data
class Input_Plugin_Api {
public:
    virtual ~Input_Plugin_Api() = default;
    
    // Plugin lifecycle
    virtual int init(json config_data) = 0;
    virtual int start() = 0;
    virtual int stop() = 0;
    virtual int get_state() = 0;
    virtual bool is_enabled() = 0;
    
    // Configuration
    virtual int parse_config(json config_data) = 0;
    virtual json get_stats() = 0;
    
    // Plugin metadata
    virtual std::string get_plugin_name() = 0;
    virtual std::string get_plugin_version() = 0;
    virtual std::string get_plugin_author() = 0;
    virtual std::string get_plugin_description() = 0;
    
    // Input-specific methods
    virtual bool has_data() = 0;  // Check if data is available
    virtual P25_TSBK_Data get_data() = 0;  // Get next TSBK data packet
    virtual void set_data_callback(std::function<void(P25_TSBK_Data)> callback) = 0;  // Set callback for async data
    
protected:
    Plugin_State state_ = Plugin_State::PLUGIN_UNINITIALIZED;
    json config_;
    bool enabled_ = true;
};

// Base input plugin implementation
class Base_Input_Plugin : public Input_Plugin_Api {
public:
    Base_Input_Plugin() = default;
    virtual ~Base_Input_Plugin() = default;
    
    // Default implementations
    virtual int get_state() override { return static_cast<int>(state_); }
    virtual bool is_enabled() override { return enabled_; }
    
    virtual json get_stats() override {
        json stats;
        stats["plugin_name"] = get_plugin_name();
        stats["state"] = static_cast<int>(state_);
        stats["enabled"] = enabled_;
        return stats;
    }
    
protected:
    void set_state(Plugin_State state) { state_ = state; }
    std::function<void(P25_TSBK_Data)> data_callback_;
};

// Output Plugin API Interface
class Output_Plugin_Api {
public:
    virtual ~Output_Plugin_Api() = default;
    
    // Plugin lifecycle
    virtual int init(json config_data) = 0;
    virtual int start() = 0;
    virtual int stop() = 0;
    virtual int get_state() = 0;
    virtual bool is_enabled() = 0;
    
    // Configuration
    virtual int parse_config(json config_data) = 0;
    virtual json get_stats() = 0;
    
    // Plugin metadata
    virtual std::string get_plugin_name() = 0;
    virtual std::string get_plugin_version() = 0;
    virtual std::string get_plugin_author() = 0;
    virtual std::string get_plugin_description() = 0;
    
    // Output-specific methods
    virtual int process_data(const P25_TSBK_Data& data) = 0;  // Process incoming data
    virtual int flush() = 0;  // Flush any buffered data
    virtual bool is_ready() = 0;  // Check if ready to accept data
    
protected:
    Plugin_State state_ = Plugin_State::PLUGIN_UNINITIALIZED;
    json config_;
    bool enabled_ = true;
};

// Base output plugin implementation
class Base_Output_Plugin : public Output_Plugin_Api {
public:
    Base_Output_Plugin() = default;
    virtual ~Base_Output_Plugin() = default;
    
    // Default implementations
    virtual int get_state() override { return static_cast<int>(state_); }
    virtual bool is_enabled() override { return enabled_; }
    virtual int flush() override { return 0; }  // Default: no buffering
    virtual bool is_ready() override { return state_ == Plugin_State::PLUGIN_RUNNING; }
    
    virtual json get_stats() override {
        json stats;
        stats["plugin_name"] = get_plugin_name();
        stats["state"] = static_cast<int>(state_);
        stats["enabled"] = enabled_;
        return stats;
    }
    
protected:
    void set_state(Plugin_State state) { state_ = state; }
};