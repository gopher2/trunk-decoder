/*
 * trunk-player API Plugin for trunk-decoder
 * Transfers decoded audio and metadata to trunk-player via REST API
 */

#pragma once

#include "../../src/plugin_api.h"
#include <curl/curl.h>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>

class Trunk_Player_Api : public Base_Plugin {
private:
    struct Transfer_Config {
        std::string api_base_url;          // e.g. "http://localhost:8000/api/v1"
        std::string api_key;               // Authentication token
        std::string system_short_name;     // Maps to trunk-player system
        bool transfer_audio;               // Transfer audio files
        bool transfer_metadata;            // Transfer call metadata
        std::vector<std::string> audio_formats; // Which formats to transfer
        bool delete_after_transfer;       // Cleanup local files
        int retry_count;
        int timeout_seconds;
        bool verify_ssl;
    };
    
    struct Transfer_Job {
        Call_Data_t call_info;
        Transfer_Config config;
        std::vector<std::string> audio_files; // Files to transfer
        int retry_count;
        std::chrono::system_clock::time_point next_retry;
        
        Transfer_Job() : retry_count(0), next_retry(std::chrono::system_clock::now()) {}
    };
    
    std::map<std::string, Transfer_Config> stream_configs_; // stream_name -> config
    
    // Async transfer queue
    std::queue<Transfer_Job> transfer_queue_;
    std::vector<std::thread> transfer_workers_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::atomic<bool> stop_workers_;
    
    // CURL handles for API calls
    std::map<std::thread::id, CURL*> curl_handles_;
    std::mutex curl_mutex_;
    
    // Statistics
    std::atomic<int> transfers_queued_;
    std::atomic<int> transfers_completed_;
    std::atomic<int> transfers_failed_;
    std::atomic<int> audio_files_transferred_;
    std::atomic<int> metadata_records_transferred_;
    
    // API endpoints
    std::string build_call_endpoint(const Transfer_Config& config, const Call_Data_t& call_info);
    std::string build_audio_endpoint(const Transfer_Config& config, const Call_Data_t& call_info);
    std::string build_transmission_endpoint(const Transfer_Config& config, const Call_Data_t& call_info);
    
    // Worker methods
    void transfer_worker();
    bool transfer_call_metadata(const Transfer_Job& job);
    bool transfer_audio_file(const Transfer_Job& job, const std::string& audio_file);
    bool transfer_transmissions(const Transfer_Job& job);
    
    // API helpers
    CURL* get_curl_handle();
    static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* response);
    json build_call_json(const Call_Data_t& call_info);
    json build_transmission_json(const Call_Data_t& call_info);
    
    // Multipart upload for audio files
    bool upload_multipart_file(CURL* curl, const std::string& url, 
                              const std::string& file_path,
                              const json& metadata,
                              const std::map<std::string, std::string>& headers);
    
public:
    Trunk_Player_Api();
    virtual ~Trunk_Player_Api();
    
    // Plugin API implementation
    virtual int init(json config_data) override;
    virtual int start() override;
    virtual int stop() override;
    virtual int parse_config(json config_data) override;
    
    virtual int call_data_ready(Call_Data_t call_info) override;
    
    virtual json get_stats() override;
    
    // Plugin metadata
    PLUGIN_INFO("trunk-player API", "1.0.0", "trunk-decoder",
                "Transfer decoded audio and metadata to trunk-player via REST API")
};

TRUNK_DECODER_PLUGIN_FACTORY(Trunk_Player_Api)