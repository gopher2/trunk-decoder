#include "api_service.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <map>
#include <ctime>

ApiService::ApiService(int port, const std::string& output_dir, bool verbose, bool foreground,
                       int worker_threads, int queue_size, int job_timeout_ms) 
    : output_dir_(output_dir), verbose_(verbose), foreground_(foreground), audio_format_("wav"), audio_bitrate_(0),
      worker_threads_(worker_threads), queue_size_(queue_size), job_timeout_ms_(job_timeout_ms) {
    http_service_ = std::make_unique<HttpService>(port);
    job_manager_ = std::make_unique<JobManager>(worker_threads_, queue_size_, job_timeout_ms_, verbose_);
    
    // Register API endpoints
    http_service_->add_handler("/api/v1/decode", 
        [this](const HttpRequest& req, HttpResponse& resp) {
            this->handle_decode_request(req, resp);
        });
        
    http_service_->add_handler("/api/v1/status", 
        [this](const HttpRequest& req, HttpResponse& resp) {
            this->handle_status_request(req, resp);
        });
        
    http_service_->add_handler("/api/v1/jobs/", 
        [this](const HttpRequest& req, HttpResponse& resp) {
            this->handle_job_status_request(req, resp);
        });
}

ApiService::~ApiService() {
    stop();
}

bool ApiService::start() {
    // Create output directory if needed
    try {
        std::filesystem::create_directories(output_dir_);
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Failed to create output directory: " << e.what() << std::endl;
        return false;
    }
    
    // Start job manager first
    if (!job_manager_->start()) {
        std::cerr << "Failed to start job manager" << std::endl;
        return false;
    }
    
    // Configure HTTPS if enabled
    if (!ssl_cert_file_.empty() && !ssl_key_file_.empty()) {
        http_service_->enable_tls(ssl_cert_file_, ssl_key_file_);
        if (verbose_) {
            std::cout << "[API] HTTPS enabled with cert: " << ssl_cert_file_ << std::endl;
        }
    }
    
    if (foreground_) {
        // Run HTTP service on main thread (blocking)
        return http_service_->start();
    } else {
        // Start HTTP service in a separate thread (non-blocking)
        std::thread([this]() {
            this->http_service_->start();
        }).detach();
        
        // Give it a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        return http_service_->is_running();
    }
}

void ApiService::stop() {
    if (http_service_) {
        http_service_->stop();
    }
    if (job_manager_) {
        job_manager_->stop();
    }
}

bool ApiService::is_running() const {
    return http_service_ && http_service_->is_running();
}

bool ApiService::validate_auth_token(const HttpRequest& request) {
    if (auth_token_.empty()) {
        return true; // No auth required
    }
    
    // Check Authorization header
    auto auth_header = request.headers.find("Authorization");
    if (auth_header == request.headers.end()) {
        return false;
    }
    
    std::string auth_value = auth_header->second;
    
    // Support Bearer token format: "Bearer <token>"
    if (auth_value.substr(0, 7) == "Bearer ") {
        std::string token = auth_value.substr(7);
        return token == auth_token_;
    }
    
    return false;
}

void ApiService::handle_decode_request(const HttpRequest& request, HttpResponse& response) {
    try {
        if (verbose_) {
            std::cout << "[API] Received decode request from client" << std::endl;
            std::cout << "[API] Method: " << request.method << std::endl;
            std::cout << "[API] Content-Type: " << request.content_type << std::endl;
            std::cout << "[API] Files found: " << request.files.size() << std::endl;
            std::cout << "[API] Form data fields: " << request.form_data.size() << std::endl;
        }
        
        // Validate authentication first
        if (!validate_auth_token(request)) {
            if (verbose_) std::cout << "[API] Authentication failed" << std::endl;
            response.status_code = 401;
            response.headers["WWW-Authenticate"] = "Bearer realm=trunk-decoder";
            response.set_json("{\"error\": \"Authentication required\"}");
            return;
        }
        
        if (request.method != "POST") {
            response.status_code = 405;
            response.set_json("{\"error\": \"Method not allowed\"}");
            return;
        }
        
        // Get P25 file from multipart form data
        auto p25_file_it = request.files.find("p25_file");
        if (p25_file_it == request.files.end()) {
            response.status_code = 400;
            response.set_json("{\"error\": \"Missing p25_file in request\"}");
            return;
        }
        
        std::string p25_temp_file = p25_file_it->second;
        
        // Get metadata JSON
        std::string metadata_str;
        auto metadata_it = request.form_data.find("metadata");
        if (metadata_it != request.form_data.end()) {
            metadata_str = metadata_it->second;
        }
        
        // Get stream name for job routing
        std::string stream_name = "default";
        auto stream_it = request.form_data.find("stream_name");
        if (stream_it != request.form_data.end()) {
            stream_name = stream_it->second;
        }
        
        // Parse minimal information for logging (but don't do full processing)
        std::string system_name = "Unknown";
        std::string talkgroup = "Unknown"; 
        std::string call_id = "Unknown";
        std::string src_radio_id = "Unknown";
        
        if (!metadata_str.empty()) {
            try {
                auto extract_json_field = [](const std::string& json, const std::string& field) -> std::string {
                    std::string search_pattern = "\"" + field + "\": ";
                    size_t pos = json.find(search_pattern);
                    if (pos != std::string::npos) {
                        pos += search_pattern.length();
                        if (json[pos] == '"') {
                            pos++;
                            size_t end = json.find('"', pos);
                            if (end != std::string::npos) {
                                return json.substr(pos, end - pos);
                            }
                        } else {
                            size_t end = json.find_first_of(",\n}", pos);
                            if (end != std::string::npos) {
                                return json.substr(pos, end - pos);
                            }
                        }
                    }
                    return "Unknown";
                };
                
                system_name = extract_json_field(metadata_str, "short_name");
                talkgroup = extract_json_field(metadata_str, "talkgroup");
                call_id = extract_json_field(metadata_str, "call_num");
                src_radio_id = extract_json_field(metadata_str, "srcList");
                if (src_radio_id == "Unknown") {
                    src_radio_id = extract_json_field(metadata_str, "src");
                }
                
            } catch (const std::exception& e) {
                if (verbose_) {
                    std::cout << "[API] Warning: Failed to parse call metadata: " << e.what() << std::endl;
                }
            }
        }
        
        // Log ingestion (not full decode info since that happens later in workers)
        std::cout << "[INGEST] " << system_name << " | TG:" << talkgroup 
                  << " | SRC:" << src_radio_id << " | Call:" << call_id 
                  << " | Stream:" << stream_name << " | Queued" << std::endl;
        
        // Get original filename for output path generation
        std::string original_filename;
        auto upload_it = request.file_uploads.find("p25_file");
        if (upload_it != request.file_uploads.end()) {
            original_filename = upload_it->second.original_filename;
        }
        
        if (original_filename.empty()) {
            original_filename = "api_call_" + std::to_string(std::time(nullptr)) + ".p25";
        }
        
        // Remove .p25 extension if present
        std::string base_filename = original_filename;
        if (base_filename.size() > 4 && base_filename.substr(base_filename.size() - 4) == ".p25") {
            base_filename = base_filename.substr(0, base_filename.size() - 4);
        }
        
        // Create basic output path (detailed folder structure will be created by worker)
        std::string folder_path = output_dir_;
        if (!metadata_str.empty()) {
            try {
                // Quick parse for system name to create basic folder structure
                size_t short_name_pos = metadata_str.find("\"short_name\": \"");
                if (short_name_pos != std::string::npos) {
                    short_name_pos += 15;
                    size_t short_name_end = metadata_str.find("\"", short_name_pos);
                    std::string short_name = metadata_str.substr(short_name_pos, short_name_end - short_name_pos);
                    folder_path = output_dir_ + "/" + short_name;
                    std::filesystem::create_directories(folder_path);
                }
            } catch (const std::exception& e) {
                // Use default folder if parsing fails
            }
        }
        
        std::string output_base_path = folder_path + "/" + base_filename;
        
        // Queue job for asynchronous processing
        std::string job_id = job_manager_->queue_job(
            p25_temp_file,
            metadata_str, 
            output_base_path,
            stream_name,
            upload_script_,
            audio_format_,
            audio_bitrate_
        );
        
        if (job_id.empty()) {
            response.status_code = 503;
            response.set_json("{\"error\": \"Processing queue is full\"}");
            cleanup_temp_file(p25_temp_file);
            return;
        }
        
        // Return job ID for status tracking
        response.status_code = 202; // Accepted for processing
        std::ostringstream json;
        json << "{"
             << "\"job_id\": \"" << job_id << "\","
             << "\"status\": \"queued\","
             << "\"message\": \"P25 file queued for processing\","
             << "\"stream_name\": \"" << stream_name << "\""
             << "}";
        response.set_json(json.str());
        
        if (verbose_) {
            std::cout << "[API] Queued job " << job_id << " for stream " << stream_name << std::endl;
        }
        
    } catch (const std::exception& e) {
        response.status_code = 500;
        response.set_json("{\"error\": \"Internal server error\"}");
        
        if (verbose_) {
            std::cerr << "[API] Error processing request: " << e.what() << std::endl;
        }
    }
}

std::string ApiService::create_temp_file(const std::vector<uint8_t>& data, const std::string& extension) {
    std::string temp_filename = "/tmp/trunk_decoder_" + std::to_string(std::time(nullptr)) + extension;
    std::ofstream temp_file(temp_filename, std::ios::binary);
    temp_file.write(reinterpret_cast<const char*>(data.data()), data.size());
    temp_file.close();
    return temp_filename;
}

void ApiService::cleanup_temp_file(const std::string& filepath) {
    try {
        std::filesystem::remove(filepath);
    } catch (const std::filesystem::filesystem_error& e) {
        if (verbose_) {
            std::cerr << "[API] Warning: Failed to cleanup temp file " << filepath 
                     << ": " << e.what() << std::endl;
        }
    }
}

void ApiService::configure_processing(int worker_threads, int queue_size, int timeout_ms) {
    worker_threads_ = worker_threads;
    queue_size_ = queue_size;
    job_timeout_ms_ = timeout_ms;
}

JobManager::JobStats ApiService::get_processing_stats() {
    return job_manager_->get_stats();
}

void ApiService::handle_status_request(const HttpRequest& request, HttpResponse& response) {
    try {
        auto stats = job_manager_->get_stats();
        
        std::ostringstream json;
        json << "{"
             << "\"status\": \"ok\","
             << "\"service\": \"trunk-decoder\","
             << "\"version\": \"1.0\","
             << "\"processing\": {"
             << "\"jobs_queued\": " << stats.queued << ","
             << "\"jobs_completed\": " << stats.completed << ","
             << "\"jobs_failed\": " << stats.failed << ","
             << "\"active_workers\": " << stats.active_workers << ","
             << "\"queue_size\": " << stats.queue_size << ","
             << "\"avg_processing_time_ms\": " << stats.avg_processing_time_ms
             << "}"
             << "}";
             
        response.set_json(json.str());
    } catch (const std::exception& e) {
        response.status_code = 500;
        response.set_json("{\"error\": \"Failed to get status\"}");
    }
}

void ApiService::handle_job_status_request(const HttpRequest& request, HttpResponse& response) {
    try {
        // Extract job ID from URL path (e.g., /api/v1/jobs/job_123456)
        std::string path = request.path;
        size_t last_slash = path.find_last_of('/');
        if (last_slash == std::string::npos || last_slash == path.length() - 1) {
            response.status_code = 400;
            response.set_json("{\"error\": \"Job ID required\"}");
            return;
        }
        
        std::string job_id = path.substr(last_slash + 1);
        auto job = job_manager_->get_job_status(job_id);
        
        if (!job) {
            response.status_code = 404;
            response.set_json("{\"error\": \"Job not found\"}");
            return;
        }
        
        std::string status_str;
        switch (job->status) {
            case ProcessingJob::QUEUED: status_str = "queued"; break;
            case ProcessingJob::PROCESSING: status_str = "processing"; break;
            case ProcessingJob::COMPLETED: status_str = "completed"; break;
            case ProcessingJob::FAILED: status_str = "failed"; break;
        }
        
        std::ostringstream json;
        json << "{"
             << "\"job_id\": \"" << job->job_id << "\","
             << "\"status\": \"" << status_str << "\","
             << "\"stream_name\": \"" << job->stream_name << "\"";
             
        if (!job->error_message.empty()) {
            json << ",\"error\": \"" << job->error_message << "\"";
        }
        
        // Add timing information if available
        auto now = std::chrono::system_clock::now();
        auto received_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - job->received_time).count();
        json << ",\"age_ms\": " << received_ms;
        
        if (job->status == ProcessingJob::PROCESSING) {
            auto processing_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - job->started_time).count();
            json << ",\"processing_ms\": " << processing_ms;
        }
        
        if (job->status == ProcessingJob::COMPLETED || job->status == ProcessingJob::FAILED) {
            auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                job->completed_time - job->received_time).count();
            json << ",\"total_time_ms\": " << total_ms;
        }
        
        json << "}";
        response.set_json(json.str());
        
    } catch (const std::exception& e) {
        response.status_code = 500;
        response.set_json("{\"error\": \"Failed to get job status\"}");
    }
}