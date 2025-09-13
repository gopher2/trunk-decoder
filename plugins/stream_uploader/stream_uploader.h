/*
 * Stream Uploader Plugin for trunk-decoder
 * Uploads to multiple destinations (OpenMHZ, Broadcastify, custom APIs)
 */

#pragma once

#include "../../src/plugin_api.h"
#include <curl/curl.h>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>

class Stream_Uploader : public Base_Plugin {
private:
    struct Upload_Config {
        std::string type; // "openmhz", "broadcastify", "custom"
        std::string api_url;
        std::string api_key;
        std::string system_id;
        std::map<std::string, std::string> headers;
        std::vector<std::string> supported_formats; // Which formats to upload
        int retry_count;
        int timeout_seconds;
        bool verify_ssl;
    };
    
    struct Upload_Job {
        Call_Data_t call_info;
        Upload_Config config;
        std::string format;
        std::string file_path;
        int retry_count;
        std::chrono::system_clock::time_point next_retry;
        
        Upload_Job() : retry_count(0), next_retry(std::chrono::system_clock::now()) {}
    };
    
    std::map<std::string, std::vector<Upload_Config>> stream_uploaders_; // stream -> configs
    
    // Async upload queue
    std::queue<Upload_Job> upload_queue_;
    std::vector<std::thread> upload_workers_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::atomic<bool> stop_workers_;
    
    // CURL handles
    std::map<std::thread::id, CURL*> curl_handles_;
    std::mutex curl_mutex_;
    
    // Statistics
    std::atomic<int> uploads_queued_;
    std::atomic<int> uploads_completed_;
    std::atomic<int> uploads_failed_;
    std::map<std::string, std::atomic<int>> uploads_by_type_;
    
    // Worker methods
    void upload_worker();
    bool upload_to_openmhz(const Upload_Job& job);
    bool upload_to_broadcastify(const Upload_Job& job);
    bool upload_to_custom(const Upload_Job& job);
    
    // CURL helpers
    CURL* get_curl_handle();
    static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* response);
    
public:
    Stream_Uploader();
    virtual ~Stream_Uploader();
    
    // Plugin API implementation
    virtual int init(json config_data) override;
    virtual int start() override;
    virtual int stop() override;
    virtual int parse_config(json config_data) override;
    
    virtual int call_data_ready(Call_Data_t call_info) override;
    
    virtual json get_stats() override;
    
    // Plugin metadata
    PLUGIN_INFO("Stream Uploader", "1.0.0", "trunk-decoder",
                "Upload audio files to multiple streaming services")
};

TRUNK_DECODER_PLUGIN_FACTORY(Stream_Uploader)