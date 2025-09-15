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
        
        char buffer[8192];
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_read <= 0) {
            requests_rejected_++;
            return;
        }
        
        buffer[bytes_read] = '\0';
        std::string request(buffer);
        
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
        }
        
        // TODO: Parse the JSON request body to extract call data
        // TODO: Extract audio file data and metadata
        // TODO: Create P25_TSBK_Data objects from the call information
        
        // For now, acknowledge the request
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
};

// Plugin factory
extern "C" std::shared_ptr<Input_Plugin_Api> create_input_plugin() {
    return std::make_shared<API_Input>();
}