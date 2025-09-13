/*
 * Worker Pool Header for Scalable P25 Processing
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 */

#pragma once

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <functional>
#include <memory>

// Job types for the worker pool
enum class JobType {
    DECODE,      // P25 → WAV decode
    CONVERT,     // WAV → modern format conversion  
    UPLOAD       // Upload script execution
};

// Job data structure
struct ProcessingJob {
    JobType type;
    std::string stream_name;
    std::string system_name;
    int priority;
    
    // Input data
    std::string input_file;
    std::string metadata_json;
    
    // Output configuration per stream
    std::string output_dir;
    std::map<std::string, bool> output_formats;
    std::map<std::string, int> format_bitrates;
    std::string upload_script;
    
    // Processing state
    std::string job_id;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point started_at;
    
    ProcessingJob() : type(JobType::DECODE), priority(1), created_at(std::chrono::system_clock::now()) {}
};

// Worker pool class for async P25 processing
class WorkerPool {
private:
    std::queue<std::shared_ptr<ProcessingJob>> job_queue_;
    std::vector<std::thread> workers_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::atomic<bool> stop_workers_;
    
    // Configuration
    size_t max_queue_size_;
    size_t batch_size_;
    std::chrono::milliseconds timeout_;
    
    // Worker statistics
    std::atomic<int> active_jobs_;
    std::atomic<int> completed_jobs_;
    std::atomic<int> failed_jobs_;
    
    // Worker thread function
    void worker_thread();
    
    // Job processing functions
    bool process_decode_job(std::shared_ptr<ProcessingJob> job);
    bool process_convert_job(std::shared_ptr<ProcessingJob> job);
    bool process_upload_job(std::shared_ptr<ProcessingJob> job);
    
public:
    WorkerPool(size_t num_workers, size_t max_queue_size, size_t batch_size, 
               std::chrono::milliseconds timeout);
    ~WorkerPool();
    
    // Job management
    bool enqueue_job(std::shared_ptr<ProcessingJob> job);
    bool is_queue_full() const;
    size_t queue_size() const;
    
    // Worker management
    void start();
    void stop();
    bool is_running() const;
    
    // Statistics
    struct Stats {
        int active_jobs;
        int completed_jobs;
        int failed_jobs;
        size_t queue_depth;
        double avg_processing_time_ms;
    };
    Stats get_stats() const;
    
    // Health check
    bool is_healthy() const;
};

// Stream-aware API service that uses worker pool
class StreamApiService {
private:
    std::unique_ptr<WorkerPool> worker_pool_;
    std::map<std::string, ProcessingJob> stream_configs_; // stream_name -> config template
    
public:
    StreamApiService(const std::vector<ProcessingJob>& stream_configs, 
                    size_t num_workers, size_t queue_size);
    ~StreamApiService();
    
    // HTTP request handling
    void handle_decode_request(const std::string& stream_name, 
                              const std::string& p25_data,
                              const std::string& metadata_json);
    
    // Stream management
    bool add_stream(const ProcessingJob& stream_config);
    bool remove_stream(const std::string& stream_name);
    std::vector<std::string> get_stream_names() const;
    
    // Monitoring
    WorkerPool::Stats get_worker_stats() const;
    bool is_healthy() const;
};