/*
 * API Input Plugin - HTTP API Input for trunk-decoder
 * Provides HTTP API endpoint to receive call data uploads
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 */

#include "../src/plugin_api.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <map>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <vector>

class API_Input : public Base_Input_Plugin {
private:
    // Configuration
    std::string listen_address_;
    int listen_port_;
    std::string auth_token_;
    bool verbose_;
    
    // HTTP server
    int socket_fd_;
    struct sockaddr_in server_addr_;
    
    // Threading
    std::thread server_thread_;
    std::atomic<bool> running_;
    
    // Data queue for call data
    std::queue<P25_TSBK_Data> data_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    size_t max_queue_size_;
    
    // Statistics
    std::atomic<uint64_t> requests_received_;
    std::atomic<uint64_t> requests_processed_;
    std::atomic<uint64_t> requests_rejected_;
    
public:
    PLUGIN_INFO("API Input", "1.0.0", "Dave K9DPD", "HTTP API input for receiving call data uploads")
    
    API_Input() : 
        listen_address_("0.0.0.0"), 
        listen_port_(3000),
        verbose_(false),
        socket_fd_(-1),
        running_(false),
        max_queue_size_(1000),
        requests_received_(0),
        requests_processed_(0),
        requests_rejected_(0) {}
    
    virtual ~API_Input() {
        stop();
    }
    
    virtual int init(json config_data) override {
        if (parse_config(config_data) != 0) {
            set_state(Plugin_State::PLUGIN_ERROR);
            return -1;
        }
        
        if (initialize_socket() != 0) {
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
        
        running_ = true;
        server_thread_ = std::thread(&API_Input::server_worker, this);
        
        set_state(Plugin_State::PLUGIN_RUNNING);
        
        if (verbose_) {
            std::cout << "[API_Input] HTTP server started on " 
                      << listen_address_ << ":" << listen_port_ << std::endl;
        }
        
        return 0;
    }
    
    virtual int stop() override {
        if (running_) {
            running_ = false;
            
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
            
            if (socket_fd_ >= 0) {
                close(socket_fd_);
                socket_fd_ = -1;
            }
            
            set_state(Plugin_State::PLUGIN_STOPPED);
            
            if (verbose_) {
                std::cout << "[API_Input] HTTP server stopped. Stats: " 
                          << requests_received_ << " received, " 
                          << requests_processed_ << " processed, "
                          << requests_rejected_ << " rejected" << std::endl;
            }
        }
        
        return 0;
    }
    
    virtual int parse_config(json config_data) override {
        config_ = config_data;
        
        if (config_data.contains("listen_address")) {
            listen_address_ = config_data["listen_address"];
        }
        
        if (config_data.contains("listen_port")) {
            listen_port_ = config_data["listen_port"];
        }
        
        if (config_data.contains("auth_token")) {
            auth_token_ = config_data["auth_token"];
        }
        
        if (config_data.contains("max_queue_size")) {
            max_queue_size_ = config_data["max_queue_size"];
        }
        
        if (config_data.contains("verbose")) {
            verbose_ = config_data["verbose"];
        }
        
        return 0;
    }
    
    virtual bool has_data() override {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return !data_queue_.empty();
    }
    
    virtual P25_TSBK_Data get_data() override {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !data_queue_.empty() || !running_; });
        
        if (!data_queue_.empty()) {
            P25_TSBK_Data data = data_queue_.front();
            data_queue_.pop();
            return data;
        }
        
        // Return empty data if stopped
        return P25_TSBK_Data();
    }
    
    virtual void set_data_callback(std::function<void(P25_TSBK_Data)> callback) override {
        data_callback_ = callback;
    }
    
    virtual json get_stats() override {
        json stats = Base_Input_Plugin::get_stats();
        stats["listen_address"] = listen_address_;
        stats["listen_port"] = listen_port_;
        stats["requests_received"] = requests_received_.load();
        stats["requests_processed"] = requests_processed_.load();
        stats["requests_rejected"] = requests_rejected_.load();
        stats["queue_size"] = data_queue_.size();
        stats["auth_enabled"] = !auth_token_.empty();
        return stats;
    }
    
private:
    int initialize_socket() {
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            std::cerr << "[API_Input] Failed to create socket: " << strerror(errno) << std::endl;
            return -1;
        }
        
        // Allow address reuse
        int reuse = 1;
        if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            std::cerr << "[API_Input] Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
        
        // Bind to address
        memset(&server_addr_, 0, sizeof(server_addr_));
        server_addr_.sin_family = AF_INET;
        server_addr_.sin_port = htons(listen_port_);
        
        if (inet_pton(AF_INET, listen_address_.c_str(), &server_addr_.sin_addr) <= 0) {
            std::cerr << "[API_Input] Invalid IP address: " << listen_address_ << std::endl;
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
        
        if (bind(socket_fd_, (struct sockaddr*)&server_addr_, sizeof(server_addr_)) < 0) {
            std::cerr << "[API_Input] Failed to bind to " << listen_address_ 
                      << ":" << listen_port_ << ": " << strerror(errno) << std::endl;
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
        
        if (listen(socket_fd_, 10) < 0) {
            std::cerr << "[API_Input] Failed to listen: " << strerror(errno) << std::endl;
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
        
        return 0;
    }
    
    void server_worker() {
        if (verbose_) {
            std::cout << "[API_Input] HTTP server listening on " 
                      << listen_address_ << ":" << listen_port_ << std::endl;
        }
        
        while (running_) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(socket_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (running_) {
                    std::cerr << "[API_Input] Accept error: " << strerror(errno) << std::endl;
                }
                continue;
            }
            
            // Handle request in separate thread (for now, handle synchronously)
            handle_request(client_fd);
            close(client_fd);
        }
    }
    
    void handle_request(int client_fd) {
        requests_received_++;
        
        // Use larger buffer for multipart file uploads
        std::vector<char> buffer(65536); // 64KB buffer
        ssize_t bytes_read = recv(client_fd, buffer.data(), buffer.size() - 1, 0);
        
        if (bytes_read <= 0) {
            requests_rejected_++;
            return;
        }
        
        buffer[bytes_read] = '\0';
        std::string request(buffer.data(), bytes_read);
        
        if (verbose_) {
            std::cout << "[API_Input] Received HTTP request: " << bytes_read << " bytes" << std::endl;
        }
        
        // Parse HTTP request
        std::istringstream request_stream(request);
        std::string method, path, version;
        request_stream >> method >> path >> version;
        
        if (verbose_) {
            std::cout << "[API_Input] Request: " << method << " " << path << std::endl;
        }
        
        // Handle different endpoints
        if (method == "POST" && path == "/api/call-upload") {
            handle_call_upload(client_fd, request);
        } else if (method == "POST" && path == "/api/v1/decode") {
            handle_decode_request(client_fd, request);
        } else if (method == "GET" && path == "/api/status") {
            handle_status_request(client_fd);
        } else if (method == "GET" && path == "/") {
            // Root endpoint - show service info
            json response;
            response["service"] = "trunk-decoder API Input Plugin";
            response["version"] = "1.0.0";
            response["endpoints"] = {"/api/status", "/api/call-upload", "/api/v1/decode"};
            std::string response_body = response.dump(2);
            send_http_response(client_fd, 200, "OK", "application/json", response_body);
            requests_processed_++;
        } else {
            send_http_response(client_fd, 404, "Not Found", "text/plain", "Not Found");
            requests_rejected_++;
        }
    }
    
    void handle_call_upload(int client_fd, const std::string& request) {
        // For now, just acknowledge the upload
        // In a real implementation, we would parse the JSON call data
        // and create P25_TSBK_Data objects from it
        
        json response;
        response["status"] = "success";
        response["message"] = "Call data received";
        response["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        std::string response_body = response.dump();
        send_http_response(client_fd, 200, "OK", "application/json", response_body);
        
        requests_processed_++;
        
        if (verbose_) {
            std::cout << "[API_Input] Processed call upload request" << std::endl;
        }
    }
    
    void handle_decode_request(int client_fd, const std::string& request) {
        // Handle trunk-recorder's /api/v1/decode requests
        // This is the main endpoint for receiving voice recordings from trunk-recorder
        
        if (verbose_) {
            std::cout << "[API_Input] Processing decode request from trunk-recorder" << std::endl;
            
            // Debug: Show first 1000 chars of request to understand structure
            size_t debug_len = std::min(request.size(), size_t(1000));
            std::cout << "[API_Input] Request structure (first " << debug_len << " chars):" << std::endl;
            std::cout << request.substr(0, debug_len) << std::endl;
            std::cout << "[API_Input] --- End request debug ---" << std::endl;
        }
        
        // Parse multipart form data to extract .p25 files
        std::vector<uint8_t> p25_data;
        std::string filename;
        std::string json_data;
        
        if (parse_multipart_data(request, p25_data, filename, json_data)) {
            // Process the .p25 file
            process_p25_file(p25_data, filename, json_data);
        }
        
        json response;
        response["status"] = "success";
        response["message"] = "Call decode request received";
        response["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        std::string response_body = response.dump();
        send_http_response(client_fd, 200, "OK", "application/json", response_body);
        
        requests_processed_++;
        
        if (verbose_) {
            std::cout << "[API_Input] Processed decode request" << std::endl;
        }
    }
    
    void handle_status_request(int client_fd) {
        json status = get_stats();
        std::string response_body = status.dump();
        send_http_response(client_fd, 200, "OK", "application/json", response_body);
        
        requests_processed_++;
    }
    
    void send_http_response(int client_fd, int status_code, const std::string& status_text,
                           const std::string& content_type, const std::string& body) {
        std::ostringstream response;
        response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
        response << "Content-Type: " << content_type << "\r\n";
        response << "Content-Length: " << body.length() << "\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";
        response << body;
        
        std::string response_str = response.str();
        send(client_fd, response_str.c_str(), response_str.length(), 0);
    }
    
    bool parse_multipart_data(const std::string& request, std::vector<uint8_t>& p25_data, std::string& filename, std::string& json_data) {
        // Find the boundary
        size_t boundary_pos = request.find("boundary=");
        if (boundary_pos == std::string::npos) {
            if (verbose_) {
                std::cout << "[API_Input] No boundary found in multipart data" << std::endl;
            }
            return false;
        }
        
        size_t boundary_start = boundary_pos + 9; // length of "boundary="
        size_t boundary_end = request.find_first_of("\r\n; ", boundary_start);
        if (boundary_end == std::string::npos) {
            boundary_end = request.length();
        }
        
        std::string raw_boundary = request.substr(boundary_start, boundary_end - boundary_start);
        // Remove any trailing whitespace or quotes
        while (!raw_boundary.empty() && (raw_boundary.back() == ' ' || raw_boundary.back() == '\t' || raw_boundary.back() == '"')) {
            raw_boundary.pop_back();
        }
        
        std::string boundary = "--" + raw_boundary;
        
        if (verbose_) {
            std::cout << "[API_Input] Extracted boundary: '" << boundary << "'" << std::endl;
            
            // Debug: show all form field names and data sizes
            std::cout << "[API_Input] Form fields found:" << std::endl;
            size_t name_pos = 0;
            while ((name_pos = request.find("name=\"", name_pos)) != std::string::npos) {
                name_pos += 6; // length of "name=\""
                size_t name_end = request.find('"', name_pos);
                if (name_end != std::string::npos) {
                    std::string field_name = request.substr(name_pos, name_end - name_pos);
                    
                    // Find the data for this field
                    size_t field_data_start = request.find("\r\n\r\n", name_pos);
                    if (field_data_start != std::string::npos) {
                        field_data_start += 4; // Skip \r\n\r\n
                        size_t field_data_end = request.find(boundary, field_data_start);
                        if (field_data_end == std::string::npos) {
                            field_data_end = request.find(boundary + "--", field_data_start);
                        }
                        if (field_data_end != std::string::npos) {
                            size_t data_size = field_data_end - field_data_start;
                            if (data_size >= 2 && request.substr(field_data_end - 2, 2) == "\r\n") {
                                data_size -= 2;
                            }
                            std::cout << "[API_Input]   - " << field_name << " (" << data_size << " bytes)" << std::endl;
                            
                            // If it's JSON-like field, show a preview
                            if (field_name == "metadata" || field_name == "json" || field_name == "call_data") {
                                std::string preview = request.substr(field_data_start, std::min(data_size, size_t(200)));
                                std::cout << "[API_Input]     Preview: " << preview << std::endl;
                            }
                            
                            // Continue searching after this field's data
                            name_pos = field_data_end;
                        } else {
                            std::cout << "[API_Input]   - " << field_name << " (unknown size)" << std::endl;
                            name_pos = name_end + 1;
                        }
                    } else {
                        std::cout << "[API_Input]   - " << field_name << " (no data found)" << std::endl;
                        name_pos = name_end + 1;
                    }
                } else {
                    break; // No more field names found
                }
            }
        }
        
        // First, look for JSON metadata field (trunk-recorder sends this as "metadata")
        size_t json_field_start = request.find("name=\"metadata\"");
        if (json_field_start == std::string::npos) {
            json_field_start = request.find("name=\"json\"");
        }
        if (json_field_start == std::string::npos) {
            json_field_start = request.find("name=\"call_data\"");
        }
        
        if (json_field_start != std::string::npos) {
            // Find the start of the JSON data (after headers)
            size_t json_data_start = request.find("\r\n\r\n", json_field_start);
            if (json_data_start != std::string::npos) {
                json_data_start += 4; // Skip the \r\n\r\n
                
                // Find the end of the JSON data (next boundary)
                size_t json_data_end = request.find(boundary, json_data_start);
                if (json_data_end != std::string::npos) {
                    // Adjust for \r\n before boundary
                    if (json_data_end >= 2 && request.substr(json_data_end - 2, 2) == "\r\n") {
                        json_data_end -= 2;
                    }
                    
                    json_data = request.substr(json_data_start, json_data_end - json_data_start);
                    
                    if (verbose_) {
                        std::cout << "[API_Input] Extracted JSON data: " << json_data.length() << " chars" << std::endl;
                    }
                }
            }
        }
        
        // Find the file data
        size_t file_start = request.find("name=\"p25_file\"");
        if (file_start == std::string::npos) {
            if (verbose_) {
                std::cout << "[API_Input] No p25_file field found" << std::endl;
            }
            return false;
        }
        
        // Extract filename
        size_t filename_start = request.find("filename=\"", file_start);
        if (filename_start != std::string::npos) {
            filename_start += 10; // length of "filename=\""
            size_t filename_end = request.find('"', filename_start);
            if (filename_end != std::string::npos) {
                filename = request.substr(filename_start, filename_end - filename_start);
            }
        }
        
        // Find the start of the file data (after the headers)
        size_t data_start = request.find("\r\n\r\n", file_start);
        if (data_start == std::string::npos) {
            if (verbose_) {
                std::cout << "[API_Input] No data section found" << std::endl;
            }
            return false;
        }
        data_start += 4; // Skip the \r\n\r\n
        
        // Find the end of the file data (next boundary)
        size_t data_end = request.find(boundary, data_start);
        if (data_end == std::string::npos) {
            // Try looking for the closing boundary (with --)
            std::string close_boundary = boundary + "--";
            data_end = request.find(close_boundary, data_start);
            if (data_end == std::string::npos) {
                if (verbose_) {
                    std::cout << "[API_Input] No end boundary found. Request size: " << request.size() 
                              << ", data_start: " << data_start << std::endl;
                    std::cout << "[API_Input] Looking for boundary: '" << boundary << "'" << std::endl;
                    // Show a portion of the data after data_start for debugging
                    if (request.size() > data_start + 100) {
                        std::cout << "[API_Input] Data snippet: '" << request.substr(data_start, 100) << "...'" << std::endl;
                    } else if (request.size() > data_start) {
                        std::cout << "[API_Input] Remaining data: '" << request.substr(data_start) << "'" << std::endl;
                    }
                }
                return false;
            }
        }
        
        // Adjust for the \r\n before the boundary
        if (data_end >= 2 && request.substr(data_end - 2, 2) == "\r\n") {
            data_end -= 2;
        }
        
        // Extract the binary data
        size_t data_length = data_end - data_start;
        p25_data.resize(data_length);
        std::memcpy(p25_data.data(), request.data() + data_start, data_length);
        
        if (verbose_) {
            std::cout << "[API_Input] Extracted " << data_length << " bytes of P25 data from file: " << filename << std::endl;
        }
        
        return true;
    }
    
    void process_p25_file(const std::vector<uint8_t>& p25_data, const std::string& filename, const std::string& json_data) {
        if (verbose_) {
            std::cout << "[API_Input] Processing P25 file: " << filename << " (" << p25_data.size() << " bytes)" << std::endl;
        }
        
        // Create Call_Data_t object for the plugin router
        Call_Data_t call_data;
        
        // Parse filename to extract metadata (format: talkgroup-timestamp_frequency-call_number.p25)
        std::string base_filename = filename;
        if (base_filename.length() >= 4 && base_filename.substr(base_filename.length() - 4) == ".p25") {
            base_filename = base_filename.substr(0, base_filename.length() - 4);
        }
        
        // Extract talkgroup and other info from filename
        size_t dash_pos = base_filename.find('-');
        if (dash_pos != std::string::npos) {
            std::string tg_str = base_filename.substr(0, dash_pos);
            try {
                call_data.talkgroup = std::stoul(tg_str);
            } catch (...) {
                call_data.talkgroup = 0;
            }
        }
        
        // Generate unique filenames
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S_")
           << std::setfill('0') << std::setw(3) << ms.count();
        std::string timestamp = ss.str();
        
        // Create temporary file paths for the plugin router to process
        std::string temp_dir = "/tmp/trunk-decoder-" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
        
        std::string p25_filepath = temp_dir + "/" + timestamp + ".p25";
        std::string wav_path = temp_dir + "/" + timestamp + ".wav";
        std::string json_path = temp_dir + "/" + timestamp + ".json";
        
        // Save temporary P25 file
        std::ofstream p25_file(p25_filepath, std::ios::binary);
        if (p25_file.is_open()) {
            p25_file.write(reinterpret_cast<const char*>(p25_data.data()), p25_data.size());
            p25_file.close();
            
            if (verbose_) {
                std::cout << "[API_Input] Created temporary P25 file: " << p25_filepath << std::endl;
            }
        }
        
        // Copy P25 data as WAV for compatibility (will be processed by output plugins)
        std::ofstream wav_file(wav_path, std::ios::binary);
        if (wav_file.is_open()) {
            wav_file.write(reinterpret_cast<const char*>(p25_data.data()), p25_data.size());
            wav_file.close();
        }
        
        // Set filenames for the plugin router
        strncpy(call_data.wav_filename, wav_path.c_str(), sizeof(call_data.wav_filename) - 1);
        strncpy(call_data.json_filename, json_path.c_str(), sizeof(call_data.json_filename) - 1);
        call_data.wav_filename[sizeof(call_data.wav_filename) - 1] = '\0';
        call_data.json_filename[sizeof(call_data.json_filename) - 1] = '\0';
        
        // Use original JSON data if available, otherwise create basic metadata
        json metadata;
        if (!json_data.empty()) {
            try {
                metadata = json::parse(json_data);
                if (verbose_) {
                    std::cout << "[API_Input] Using original JSON metadata from trunk-recorder" << std::endl;
                }
            } catch (const std::exception& e) {
                if (verbose_) {
                    std::cout << "[API_Input] Failed to parse JSON data, creating basic metadata: " << e.what() << std::endl;
                }
                // Fallback to basic metadata
                metadata["filename"] = filename;
                metadata["talkgroup"] = call_data.talkgroup;
                metadata["timestamp"] = timestamp;
                metadata["size"] = p25_data.size();
                metadata["format"] = "p25";
            }
        } else {
            if (verbose_) {
                std::cout << "[API_Input] No JSON data received, parsing metadata from filename" << std::endl;
            }
            // Parse rich metadata from trunk-recorder filename format
            // Expected format: talkgroup-timestamp_frequency-call_number.p25
            // Example: 8040-1757933398_853687500.0-call_832.p25
            // Parse rich metadata from trunk-recorder filename format
            // Expected format: talkgroup-timestamp_frequency-call_number.p25
            // Example: 8040-1757933398_853687500.0-call_832.p25
            
            // Basic fallback values
            metadata["filename"] = filename;
            metadata["format"] = "p25";
            metadata["size"] = static_cast<int>(p25_data.size());
            metadata["timestamp"] = timestamp;
            
            // Parse trunk-recorder filename format
            // Remove .p25 extension first
            std::string basename = filename;
            if (basename.size() > 4 && basename.substr(basename.size() - 4) == ".p25") {
                basename = basename.substr(0, basename.size() - 4);
            }
            
            // Split by dashes: talkgroup-timestamp_frequency-call_number
            size_t first_dash = basename.find('-');
            if (first_dash != std::string::npos) {
                // Extract talkgroup
                std::string talkgroup_str = basename.substr(0, first_dash);
                try {
                    long tg = std::stol(talkgroup_str);
                    metadata["talkgroup"] = tg;
                    call_data.talkgroup = tg;  // Update call_data too
                } catch (...) {
                    metadata["talkgroup"] = call_data.talkgroup;
                }
                
                // Find the last dash (before call_number)
                size_t last_dash = basename.rfind('-');
                if (last_dash != std::string::npos && last_dash != first_dash) {
                    // Extract call number (after last dash)
                    std::string call_part = basename.substr(last_dash + 1);
                    if (call_part.substr(0, 5) == "call_") {
                        try {
                            metadata["call_num"] = std::stol(call_part.substr(5));
                        } catch (...) {
                            metadata["call_num"] = call_data.call_num;
                        }
                    } else {
                        metadata["call_num"] = call_data.call_num;
                    }
                    
                    // Extract timestamp_frequency part (between first and last dash)
                    std::string time_freq_part = basename.substr(first_dash + 1, last_dash - first_dash - 1);
                    
                    // Split by underscore: timestamp_frequency
                    size_t underscore_pos = time_freq_part.find('_');
                    if (underscore_pos != std::string::npos) {
                        // Extract timestamp
                        std::string timestamp_part = time_freq_part.substr(0, underscore_pos);
                        try {
                            long start_time = std::stol(timestamp_part);
                            metadata["start_time"] = start_time;
                            call_data.start_time = start_time;
                        } catch (...) {
                            metadata["start_time"] = call_data.start_time;
                        }
                        
                        // Extract frequency
                        std::string freq_part = time_freq_part.substr(underscore_pos + 1);
                        try {
                            double freq = std::stod(freq_part);
                            metadata["freq"] = freq;
                            call_data.freq = freq;
                        } catch (...) {
                            metadata["freq"] = call_data.freq;
                        }
                    } else {
                        metadata["start_time"] = call_data.start_time;
                        metadata["freq"] = call_data.freq;
                    }
                } else {
                    // Only one dash found, set defaults
                    metadata["call_num"] = call_data.call_num;
                    metadata["start_time"] = call_data.start_time;
                    metadata["freq"] = call_data.freq;
                }
            } else {
                // No dashes found, use defaults
                metadata["talkgroup"] = call_data.talkgroup;
                metadata["call_num"] = call_data.call_num;
                metadata["start_time"] = call_data.start_time;
                metadata["freq"] = call_data.freq;
            }
            
            // Add additional trunk-recorder compatible fields
            metadata["stop_time"] = metadata["start_time"];  // We don't have stop time from filename
            metadata["emergency"] = false;
            metadata["encrypted"] = false;
            metadata["priority"] = 1;
            metadata["source_id"] = 0;  // Not available in filename
            metadata["phase2_tdma"] = false;
            metadata["tdma_slot"] = 0;
        }
        
        // Create temporary JSON file for the plugin router
        std::ofstream json_file(json_path, std::ios::binary);
        if (json_file.is_open()) {
            json_file << metadata.dump(2);
            json_file.close();
        }
        
        // Set other call data from original JSON if available
        if (metadata.contains("source_id")) {
            call_data.source_id = metadata["source_id"];
        } else {
            call_data.source_id = 0;
        }
        
        if (metadata.contains("short_name")) {
            call_data.system_short_name = metadata["short_name"];
        } else {
            call_data.system_short_name = "unknown";
        }
        
        call_data.call_json = metadata;
        
        // Send to call processing plugins if callback is set
        if (call_callback_) {
            if (verbose_) {
                std::cout << "[API_Input] Routing call data to call processing plugins" << std::endl;
            }
            call_callback_(call_data);
        } else {
            if (verbose_) {
                std::cout << "[API_Input] No call callback set - call data not routed" << std::endl;
            }
        }
        
        if (verbose_) {
            std::cout << "[API_Input] Successfully processed P25 file" << std::endl;
        }
    }
};

// Plugin factory
extern "C" std::shared_ptr<Input_Plugin_Api> create_input_plugin() {
    return std::make_shared<API_Input>();
}