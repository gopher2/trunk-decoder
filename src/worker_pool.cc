/*
 * Worker Pool Implementation for Scalable P25 Processing
 */

#include "worker_pool.h"
#include "p25_decoder.h"
#include <iostream>
#include <sstream>
#include <random>
#include <iomanip>

WorkerPool::WorkerPool(size_t num_workers, size_t max_queue_size, size_t batch_size, 
                       std::chrono::milliseconds timeout)
    : max_queue_size_(max_queue_size), batch_size_(batch_size), timeout_(timeout),
      stop_workers_(false), active_jobs_(0), completed_jobs_(0), failed_jobs_(0) {
    
    // Start worker threads
    for (size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back(&WorkerPool::worker_thread, this);
    }
}

WorkerPool::~WorkerPool() {
    stop();
}

void WorkerPool::start() {
    stop_workers_ = false;
}

void WorkerPool::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_workers_ = true;
    }
    queue_condition_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool WorkerPool::enqueue_job(std::shared_ptr<ProcessingJob> job) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    if (job_queue_.size() >= max_queue_size_) {
        return false; // Queue full
    }
    
    // Generate unique job ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    
    std::ostringstream job_id;
    job_id << job->stream_name << "-" << dis(gen);
    job->job_id = job_id.str();
    
    job_queue_.push(job);
    queue_condition_.notify_one();
    return true;
}

void WorkerPool::worker_thread() {
    while (!stop_workers_) {
        std::shared_ptr<ProcessingJob> job;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_condition_.wait(lock, [this] { 
                return stop_workers_ || !job_queue_.empty(); 
            });
            
            if (stop_workers_ && job_queue_.empty()) {
                break;
            }
            
            if (!job_queue_.empty()) {
                job = job_queue_.front();
                job_queue_.pop();
                job->started_at = std::chrono::system_clock::now();
                active_jobs_++;
            }
        }
        
        if (job) {
            bool success = false;
            
            // Process based on job type
            switch (job->type) {
                case JobType::DECODE:
                    success = process_decode_job(job);
                    break;
                case JobType::CONVERT:
                    success = process_convert_job(job);
                    break;
                case JobType::UPLOAD:
                    success = process_upload_job(job);
                    break;
            }
            
            // Update statistics
            active_jobs_--;
            if (success) {
                completed_jobs_++;
            } else {
                failed_jobs_++;
            }
            
            std::cout << "[WORKER] Processed job " << job->job_id 
                      << " for stream " << job->stream_name 
                      << " - " << (success ? "SUCCESS" : "FAILED") << std::endl;
        }
    }
}

bool WorkerPool::process_decode_job(std::shared_ptr<ProcessingJob> job) {
    try {
        P25Decoder decoder;
        
        // Configure decoder for each enabled format
        for (const auto& format_pair : job->output_formats) {
            if (!format_pair.second) continue; // Skip disabled formats
            
            std::string format = format_pair.first;
            int bitrate = 0;
            
            auto bitrate_it = job->format_bitrates.find(format);
            if (bitrate_it != job->format_bitrates.end()) {
                bitrate = bitrate_it->second;
            }
            
            decoder.set_audio_format(format);
            decoder.set_audio_bitrate(bitrate);
            
            if (!decoder.open_p25_file(job->input_file)) {
                std::cerr << "[WORKER] Failed to open P25 file: " << job->input_file << std::endl;
                return false;
            }
            
            // Generate output filename
            std::string output_base = job->output_dir + "/" + job->job_id;
            
            if (!decoder.decode_to_audio(output_base)) {
                std::cerr << "[WORKER] Failed to decode P25 file" << std::endl;
                return false;
            }
            
            // Execute upload script if configured
            if (!job->upload_script.empty()) {
                std::ostringstream cmd;
                cmd << job->upload_script << " \"" << output_base << "." << format << "\" "
                    << "\"" << output_base << ".json\" \"1\"";
                
                int result = std::system(cmd.str().c_str());
                if (result != 0) {
                    std::cerr << "[WORKER] Upload script failed: " << result << std::endl;
                }
            }
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[WORKER] Decode job exception: " << e.what() << std::endl;
        return false;
    }
}

bool WorkerPool::process_convert_job(std::shared_ptr<ProcessingJob> job) {
    // Placeholder for future async conversion jobs
    return true;
}

bool WorkerPool::process_upload_job(std::shared_ptr<ProcessingJob> job) {
    // Placeholder for future async upload jobs
    return true;
}

WorkerPool::Stats WorkerPool::get_stats() const {
    Stats stats;
    stats.active_jobs = active_jobs_.load();
    stats.completed_jobs = completed_jobs_.load();
    stats.failed_jobs = failed_jobs_.load();
    stats.queue_depth = queue_size();
    stats.avg_processing_time_ms = 0.0; // TODO: Implement timing
    return stats;
}

size_t WorkerPool::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return job_queue_.size();
}

bool WorkerPool::is_queue_full() const {
    return queue_size() >= max_queue_size_;
}

bool WorkerPool::is_running() const {
    return !stop_workers_;
}

bool WorkerPool::is_healthy() const {
    return is_running() && !is_queue_full();
}