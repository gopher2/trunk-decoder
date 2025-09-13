/*
 * trunk-player API Plugin Implementation
 */

#include "trunk_player_api.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>

Trunk_Player_Api::Trunk_Player_Api() 
    : stop_workers_(false), transfers_queued_(0), transfers_completed_(0), 
      transfers_failed_(0), audio_files_transferred_(0), metadata_records_transferred_(0) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

Trunk_Player_Api::~Trunk_Player_Api() {
    stop();
    curl_global_cleanup();
}

int Trunk_Player_Api::init(json config_data) {
    try {
        config_ = config_data;
        set_state(Plugin_State::PLUGIN_INITIALIZED);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[trunk-player API] Init failed: " << e.what() << std::endl;
        set_state(Plugin_State::PLUGIN_ERROR);
        return -1;
    }
}

int Trunk_Player_Api::start() {
    if (state_ != Plugin_State::PLUGIN_INITIALIZED) {
        return -1;
    }
    
    // Start transfer workers
    int worker_count = config_.value("worker_threads", 2);
    for (int i = 0; i < worker_count; i++) {
        transfer_workers_.emplace_back(&Trunk_Player_Api::transfer_worker, this);
    }
    
    set_state(Plugin_State::PLUGIN_RUNNING);
    std::cout << "[trunk-player API] Started with " << worker_count << " workers" << std::endl;
    return 0;
}

int Trunk_Player_Api::stop() {
    if (state_ != Plugin_State::PLUGIN_RUNNING) {
        return 0;
    }
    
    // Stop workers
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_workers_ = true;
    }
    queue_condition_.notify_all();
    
    for (auto& worker : transfer_workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    transfer_workers_.clear();
    
    // Cleanup CURL handles
    std::lock_guard<std::mutex> curl_lock(curl_mutex_);
    for (auto& handle_pair : curl_handles_) {
        curl_easy_cleanup(handle_pair.second);
    }
    curl_handles_.clear();
    
    set_state(Plugin_State::PLUGIN_STOPPED);
    std::cout << "[trunk-player API] Stopped" << std::endl;
    return 0;
}

int Trunk_Player_Api::parse_config(json config_data) {
    try {
        // Parse stream configurations
        if (config_data.contains("streams")) {
            for (const auto& stream_config : config_data["streams"]) {
                Transfer_Config config;
                config.api_base_url = stream_config.value("api_base_url", "http://localhost:8000/api/v1");
                config.api_key = stream_config.value("api_key", "");
                config.system_short_name = stream_config.value("system_short_name", "");
                config.transfer_audio = stream_config.value("transfer_audio", true);
                config.transfer_metadata = stream_config.value("transfer_metadata", true);
                config.delete_after_transfer = stream_config.value("delete_after_transfer", false);
                config.retry_count = stream_config.value("retry_count", 3);
                config.timeout_seconds = stream_config.value("timeout_seconds", 30);
                config.verify_ssl = stream_config.value("verify_ssl", true);
                
                // Parse supported audio formats
                if (stream_config.contains("audio_formats")) {
                    for (const auto& format : stream_config["audio_formats"]) {
                        config.audio_formats.push_back(format);
                    }
                } else {
                    config.audio_formats = {"wav", "m4a"}; // Default formats
                }
                
                std::string stream_name = stream_config.value("name", "default");
                stream_configs_[stream_name] = config;
                
                std::cout << "[trunk-player API] Configured stream: " << stream_name 
                         << " -> " << config.api_base_url << std::endl;
            }
        }
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[trunk-player API] Config parse failed: " << e.what() << std::endl;
        return -1;
    }
}

int Trunk_Player_Api::call_data_ready(Call_Data_t call_info) {
    // Find matching stream configuration
    auto config_it = stream_configs_.find(call_info.stream_name);
    if (config_it == stream_configs_.end()) {
        // No configuration for this stream, skip
        return 0;
    }
    
    Transfer_Job job;
    job.call_info = call_info;
    job.config = config_it->second;
    
    // Collect audio files that match supported formats
    for (const std::string& format : job.config.audio_formats) {
        auto file_it = call_info.converted_files.find(format);
        if (file_it != call_info.converted_files.end()) {
            if (std::filesystem::exists(file_it->second)) {
                job.audio_files.push_back(file_it->second);
            }
        }
        
        // Also check for WAV file if format is wav
        if (format == "wav" && strlen(call_info.wav_filename) > 0) {
            if (std::filesystem::exists(call_info.wav_filename)) {
                job.audio_files.push_back(call_info.wav_filename);
            }
        }
    }
    
    // Queue the transfer job
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        transfer_queue_.push(job);
        transfers_queued_++;
    }
    queue_condition_.notify_one();
    
    return 0;
}

void Trunk_Player_Api::transfer_worker() {
    while (!stop_workers_) {
        Transfer_Job job;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_condition_.wait(lock, [this] { 
                return stop_workers_ || !transfer_queue_.empty(); 
            });
            
            if (stop_workers_ && transfer_queue_.empty()) {
                break;
            }
            
            if (!transfer_queue_.empty()) {
                job = transfer_queue_.front();
                transfer_queue_.pop();
            }
        }
        
        bool success = true;
        
        // Transfer call metadata
        if (job.config.transfer_metadata) {
            if (!transfer_call_metadata(job)) {
                success = false;
            }
        }
        
        // Transfer audio files
        if (job.config.transfer_audio && success) {
            for (const std::string& audio_file : job.audio_files) {
                if (!transfer_audio_file(job, audio_file)) {
                    success = false;
                    break;
                }
                audio_files_transferred_++;
            }
        }
        
        // Update statistics
        if (success) {
            transfers_completed_++;
            
            // Cleanup files if requested
            if (job.config.delete_after_transfer) {
                for (const std::string& audio_file : job.audio_files) {
                    std::filesystem::remove(audio_file);
                }
                if (strlen(job.call_info.json_filename) > 0) {
                    std::filesystem::remove(job.call_info.json_filename);
                }
            }
        } else {
            transfers_failed_++;
            
            // Retry logic could go here
            std::cerr << "[trunk-player API] Transfer failed for call " 
                      << job.call_info.call_num << std::endl;
        }
    }
}

bool Trunk_Player_Api::transfer_call_metadata(const Transfer_Job& job) {
    CURL* curl = get_curl_handle();
    if (!curl) return false;
    
    // Build API endpoint
    std::string url = build_call_endpoint(job.config, job.call_info);
    json call_json = build_call_json(job.call_info);
    std::string json_data = call_json.dump();
    
    // Setup headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    if (!job.config.api_key.empty()) {
        std::string auth_header = "Authorization: Bearer " + job.config.api_key;
        headers = curl_slist_append(headers, auth_header.c_str());
    }
    
    std::string response_data;
    
    // Configure CURL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, job.config.timeout_seconds);
    
    if (!job.config.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    
    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK || response_code >= 400) {
        std::cerr << "[trunk-player API] Metadata transfer failed: " 
                  << "CURL:" << res << " HTTP:" << response_code << std::endl;
        return false;
    }
    
    metadata_records_transferred_++;
    return true;
}

json Trunk_Player_Api::build_call_json(const Call_Data_t& call_info) {
    json call_json;
    
    // Core call data
    call_json["talkgroup"] = call_info.talkgroup;
    call_json["source_id"] = call_info.source_id;
    call_json["call_num"] = call_info.call_num;
    call_json["freq"] = call_info.freq;
    call_json["start_time"] = call_info.start_time;
    call_json["stop_time"] = call_info.stop_time;
    call_json["encrypted"] = call_info.encrypted;
    call_json["emergency"] = call_info.emergency;
    
    // System information
    call_json["system_short_name"] = call_info.system_short_name;
    call_json["nac"] = call_info.nac;
    call_json["wacn"] = call_info.wacn;
    call_json["rfss"] = call_info.rfss;
    call_json["site_id"] = call_info.site_id;
    
    if (!call_info.site_name.empty()) {
        call_json["site_name"] = call_info.site_name;
    }
    
    // Merge any existing JSON metadata
    if (!call_info.call_json.empty()) {
        call_json.merge_patch(call_info.call_json);
    }
    
    return call_json;
}

std::string Trunk_Player_Api::build_call_endpoint(const Transfer_Config& config, const Call_Data_t& call_info) {
    return config.api_base_url + "/calls/";
}

json Trunk_Player_Api::get_stats() {
    json stats = Base_Plugin::get_stats();
    stats["transfers_queued"] = transfers_queued_.load();
    stats["transfers_completed"] = transfers_completed_.load();
    stats["transfers_failed"] = transfers_failed_.load();
    stats["audio_files_transferred"] = audio_files_transferred_.load();
    stats["metadata_records_transferred"] = metadata_records_transferred_.load();
    stats["queue_size"] = transfer_queue_.size();
    return stats;
}

CURL* Trunk_Player_Api::get_curl_handle() {
    std::lock_guard<std::mutex> lock(curl_mutex_);
    std::thread::id thread_id = std::this_thread::get_id();
    
    auto it = curl_handles_.find(thread_id);
    if (it == curl_handles_.end()) {
        CURL* curl = curl_easy_init();
        curl_handles_[thread_id] = curl;
        return curl;
    }
    
    // Reset handle for reuse
    curl_easy_reset(it->second);
    return it->second;
}

size_t Trunk_Player_Api::write_callback(void* contents, size_t size, size_t nmemb, std::string* response) {
    size_t total_size = size * nmemb;
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// Placeholder implementations - would need full multipart upload logic
bool Trunk_Player_Api::transfer_audio_file(const Transfer_Job& job, const std::string& audio_file) {
    // Implementation would do multipart file upload to trunk-player API
    // For now, just return success
    std::cout << "[trunk-player API] Would transfer: " << audio_file << std::endl;
    return true;
}

std::string Trunk_Player_Api::build_audio_endpoint(const Transfer_Config& config, const Call_Data_t& call_info) {
    return config.api_base_url + "/calls/" + std::to_string(call_info.call_num) + "/audio/";
}