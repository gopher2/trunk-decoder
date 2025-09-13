/*
 * Job Manager for trunk-decoder
 * Separates network ingestion from processing with asynchronous queue
 */

#pragma once

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include "p25_decoder.h"

struct ProcessingJob {
    std::string job_id;
    std::string p25_file_path;     // Temporary P25 file
    std::string metadata_json;     // Call metadata
    std::string output_base_path;  // Base path for outputs
    std::string stream_name;       // Which stream this belongs to
    std::string upload_script;     // Script to run after processing
    std::string audio_format;      // Target audio format
    int audio_bitrate;             // Audio bitrate
    bool delete_temp_files;        // Cleanup temp files after processing
    
    // Timing
    std::chrono::system_clock::time_point received_time;
    std::chrono::system_clock::time_point started_time;
    std::chrono::system_clock::time_point completed_time;
    
    // Status
    enum Status {
        QUEUED,
        PROCESSING,
        COMPLETED,
        FAILED
    } status;
    
    std::string error_message;
    
    ProcessingJob() : status(QUEUED), audio_bitrate(0), delete_temp_files(true) {
        received_time = std::chrono::system_clock::now();
    }
};

class JobManager {
private:
    // Job queue
    std::queue<std::shared_ptr<ProcessingJob>> job_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    
    // Worker threads
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> shutdown_requested_;
    
    // Statistics
    std::atomic<int> jobs_queued_;
    std::atomic<int> jobs_completed_;
    std::atomic<int> jobs_failed_;
    std::atomic<int> active_workers_;
    
    // Configuration
    int max_worker_threads_;
    int max_queue_size_;
    int job_timeout_ms_;
    bool verbose_;
    
    // Job tracking
    std::map<std::string, std::shared_ptr<ProcessingJob>> job_tracker_;
    std::mutex tracker_mutex_;
    
    // Per-thread decoders to avoid thread safety issues
    std::map<std::thread::id, std::unique_ptr<P25Decoder>> thread_decoders_;
    std::mutex decoder_mutex_;
    
    // Worker thread function
    void worker_thread_main();
    
    // Job processing
    bool process_job(std::shared_ptr<ProcessingJob> job);
    P25Decoder* get_thread_decoder();
    void cleanup_temp_files(const ProcessingJob& job);
    void execute_upload_script(const ProcessingJob& job, const std::string& wav_file, const std::string& json_file);
    
public:
    JobManager(int max_workers = 4, int max_queue_size = 1000, int timeout_ms = 30000, bool verbose = false);
    ~JobManager();
    
    // Lifecycle
    bool start();
    void stop();
    bool is_running() const;
    
    // Job management
    std::string queue_job(const std::string& p25_temp_file, 
                         const std::string& metadata_json,
                         const std::string& output_base_path,
                         const std::string& stream_name = "default",
                         const std::string& upload_script = "",
                         const std::string& audio_format = "wav",
                         int audio_bitrate = 0);
    
    std::shared_ptr<ProcessingJob> get_job_status(const std::string& job_id);
    void remove_completed_job(const std::string& job_id);
    
    // Statistics
    struct JobStats {
        int queued;
        int completed; 
        int failed;
        int active_workers;
        int queue_size;
        double avg_processing_time_ms;
        int total_processed;
    };
    
    JobStats get_stats();
    void reset_stats();
    
    // Configuration
    void set_verbose(bool verbose) { verbose_ = verbose; }
};