/*
 * Job Manager Implementation for trunk-decoder
 */

#include "job_manager.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <random>
#include <algorithm>

JobManager::JobManager(int max_workers, int max_queue_size, int timeout_ms, bool verbose)
    : shutdown_requested_(false), jobs_queued_(0), jobs_completed_(0), jobs_failed_(0), active_workers_(0),
      max_worker_threads_(max_workers), max_queue_size_(max_queue_size), job_timeout_ms_(timeout_ms), verbose_(verbose) {
}

JobManager::~JobManager() {
    stop();
}

bool JobManager::start() {
    if (!worker_threads_.empty()) {
        return true; // Already started
    }
    
    shutdown_requested_ = false;
    
    // Start worker threads
    for (int i = 0; i < max_worker_threads_; ++i) {
        worker_threads_.emplace_back(&JobManager::worker_thread_main, this);
    }
    
    if (verbose_) {
        std::cout << "[JobManager] Started with " << max_worker_threads_ << " worker threads" << std::endl;
    }
    
    return true;
}

void JobManager::stop() {
    if (worker_threads_.empty()) {
        return; // Already stopped
    }
    
    // Signal shutdown
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_requested_ = true;
    }
    queue_condition_.notify_all();
    
    // Wait for workers to finish
    for (auto& worker : worker_threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    worker_threads_.clear();
    
    // Cleanup decoders
    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        thread_decoders_.clear();
    }
    
    if (verbose_) {
        std::cout << "[JobManager] Stopped all workers" << std::endl;
    }
}

bool JobManager::is_running() const {
    return !worker_threads_.empty() && !shutdown_requested_;
}

std::string JobManager::queue_job(const std::string& p25_temp_file,
                                 const std::string& metadata_json,
                                 const std::string& output_base_path,
                                 const std::string& stream_name,
                                 const std::string& upload_script,
                                 const std::string& audio_format,
                                 int audio_bitrate) {
    
    // Generate unique job ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    std::string job_id = "job_" + std::to_string(dis(gen)) + "_" + std::to_string(std::time(nullptr));
    
    // Create job
    auto job = std::make_shared<ProcessingJob>();
    job->job_id = job_id;
    job->p25_file_path = p25_temp_file;
    job->metadata_json = metadata_json;
    job->output_base_path = output_base_path;
    job->stream_name = stream_name;
    job->upload_script = upload_script;
    job->audio_format = audio_format;
    job->audio_bitrate = audio_bitrate;
    job->status = ProcessingJob::QUEUED;
    
    // Check queue size limit
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (job_queue_.size() >= static_cast<size_t>(max_queue_size_)) {
            if (verbose_) {
                std::cerr << "[JobManager] Queue is full, rejecting job " << job_id << std::endl;
            }
            return "";  // Queue full
        }
        
        // Add to queue
        job_queue_.push(job);
        jobs_queued_++;
    }
    
    // Track job
    {
        std::lock_guard<std::mutex> lock(tracker_mutex_);
        job_tracker_[job_id] = job;
    }
    
    // Notify workers
    queue_condition_.notify_one();
    
    if (verbose_) {
        std::cout << "[JobManager] Queued job " << job_id << " for stream " << stream_name << std::endl;
    }
    
    return job_id;
}

std::shared_ptr<ProcessingJob> JobManager::get_job_status(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(tracker_mutex_);
    auto it = job_tracker_.find(job_id);
    return (it != job_tracker_.end()) ? it->second : nullptr;
}

void JobManager::remove_completed_job(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(tracker_mutex_);
    job_tracker_.erase(job_id);
}

JobManager::JobStats JobManager::get_stats() {
    JobStats stats;
    stats.queued = jobs_queued_.load();
    stats.completed = jobs_completed_.load();
    stats.failed = jobs_failed_.load();
    stats.active_workers = active_workers_.load();
    stats.total_processed = stats.completed + stats.failed;
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stats.queue_size = static_cast<int>(job_queue_.size());
    }
    
    // Calculate average processing time (simplified)
    stats.avg_processing_time_ms = stats.total_processed > 0 ? 1500.0 : 0.0;  // Placeholder
    
    return stats;
}

void JobManager::reset_stats() {
    jobs_queued_ = 0;
    jobs_completed_ = 0;
    jobs_failed_ = 0;
}

void JobManager::worker_thread_main() {
    if (verbose_) {
        std::cout << "[JobManager] Worker thread " << std::this_thread::get_id() << " started" << std::endl;
    }
    
    while (!shutdown_requested_) {
        std::shared_ptr<ProcessingJob> job;
        
        // Get job from queue
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_condition_.wait(lock, [this] {
                return shutdown_requested_ || !job_queue_.empty();
            });
            
            if (shutdown_requested_ && job_queue_.empty()) {
                break;
            }
            
            if (!job_queue_.empty()) {
                job = job_queue_.front();
                job_queue_.pop();
                job->status = ProcessingJob::PROCESSING;
                job->started_time = std::chrono::system_clock::now();
            }
        }
        
        if (job) {
            active_workers_++;
            
            if (verbose_) {
                std::cout << "[JobManager] Processing job " << job->job_id << std::endl;
            }
            
            // Process the job
            bool success = process_job(job);
            
            // Update job status
            job->completed_time = std::chrono::system_clock::now();
            if (success) {
                job->status = ProcessingJob::COMPLETED;
                jobs_completed_++;
                
                if (verbose_) {
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        job->completed_time - job->started_time);
                    std::cout << "[JobManager] Completed job " << job->job_id 
                             << " in " << duration.count() << "ms" << std::endl;
                }
            } else {
                job->status = ProcessingJob::FAILED;
                jobs_failed_++;
                
                if (verbose_) {
                    std::cout << "[JobManager] Failed job " << job->job_id 
                             << ": " << job->error_message << std::endl;
                }
            }
            
            active_workers_--;
        }
    }
    
    if (verbose_) {
        std::cout << "[JobManager] Worker thread " << std::this_thread::get_id() << " stopped" << std::endl;
    }
}

bool JobManager::process_job(std::shared_ptr<ProcessingJob> job) {
    try {
        // Get thread-local decoder
        P25Decoder* decoder = get_thread_decoder();
        if (!decoder) {
            job->error_message = "Failed to get decoder instance";
            return false;
        }
        
        // Configure decoder
        decoder->set_audio_format(job->audio_format);
        decoder->set_audio_bitrate(job->audio_bitrate);
        
        // Open P25 file
        if (!decoder->open_p25_file(job->p25_file_path)) {
            job->error_message = "Failed to open P25 file";
            cleanup_temp_files(*job);
            return false;
        }
        
        // Decode to audio
        if (!decoder->decode_to_audio(job->output_base_path)) {
            job->error_message = "Failed to decode P25 audio";
            cleanup_temp_files(*job);
            return false;
        }
        
        // Write metadata JSON if provided
        std::string json_file;
        if (!job->metadata_json.empty()) {
            json_file = job->output_base_path + ".json";
            std::ofstream json_out(json_file);
            if (json_out.is_open()) {
                json_out << job->metadata_json;
                json_out.close();
            } else {
                if (verbose_) {
                    std::cout << "[JobManager] Warning: Failed to write metadata JSON for job " 
                             << job->job_id << std::endl;
                }
            }
        }
        
        // Check if WAV file was generated
        std::string wav_file = job->output_base_path + ".wav";
        if (!std::filesystem::exists(wav_file)) {
            job->error_message = "WAV file was not generated";
            cleanup_temp_files(*job);
            return false;
        }
        
        // Execute upload script if configured
        if (!job->upload_script.empty() && std::filesystem::exists(job->upload_script)) {
            execute_upload_script(*job, wav_file, json_file);
        }
        
        // Cleanup temporary files
        cleanup_temp_files(*job);
        
        return true;
        
    } catch (const std::exception& e) {
        job->error_message = std::string("Exception during processing: ") + e.what();
        cleanup_temp_files(*job);
        return false;
    }
}

P25Decoder* JobManager::get_thread_decoder() {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    std::thread::id thread_id = std::this_thread::get_id();
    
    auto it = thread_decoders_.find(thread_id);
    if (it == thread_decoders_.end()) {
        // Create new decoder for this thread
        auto decoder = std::make_unique<P25Decoder>();
        P25Decoder* decoder_ptr = decoder.get();
        thread_decoders_[thread_id] = std::move(decoder);
        return decoder_ptr;
    }
    
    return it->second.get();
}

void JobManager::cleanup_temp_files(const ProcessingJob& job) {
    if (!job.delete_temp_files) {
        return;
    }
    
    try {
        if (std::filesystem::exists(job.p25_file_path)) {
            std::filesystem::remove(job.p25_file_path);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        if (verbose_) {
            std::cerr << "[JobManager] Warning: Failed to cleanup temp file " 
                     << job.p25_file_path << ": " << e.what() << std::endl;
        }
    }
}

void JobManager::execute_upload_script(const ProcessingJob& job, const std::string& wav_file, const std::string& json_file) {
    try {
        std::ostringstream cmd;
        cmd << job.upload_script << " \"" << wav_file << "\" \"" << json_file << "\" \"1\"";
        
        if (verbose_) {
            std::cout << "[JobManager] Executing upload script for job " << job.job_id 
                     << ": " << cmd.str() << std::endl;
        }
        
        int result = std::system(cmd.str().c_str());
        if (result != 0 && verbose_) {
            std::cout << "[JobManager] Upload script returned non-zero exit code: " << result 
                     << " for job " << job.job_id << std::endl;
        }
    } catch (const std::exception& e) {
        if (verbose_) {
            std::cerr << "[JobManager] Error executing upload script for job " << job.job_id 
                     << ": " << e.what() << std::endl;
        }
    }
}