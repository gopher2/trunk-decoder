/*
 * P25 TSBK UDP Input Plugin
 * Receives P25C packets from trunk-recorder via UDP
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 */

#include "../src/plugin_api.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

class P25_TSBK_UDP_Input : public Base_Input_Plugin {
private:
    // Configuration
    std::string listen_address_;
    int listen_port_;
    size_t buffer_size_;
    bool validate_checksums_;
    bool verbose_;
    
    // UDP socket
    int socket_fd_;
    struct sockaddr_in server_addr_;
    
    // Threading
    std::thread receiver_thread_;
    std::atomic<bool> running_;
    
    // Data queue
    std::queue<P25_TSBK_Data> data_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    size_t max_queue_size_;
    
    // Statistics
    std::atomic<uint64_t> packets_received_;
    std::atomic<uint64_t> packets_dropped_;
    std::atomic<uint64_t> bytes_received_;
    std::atomic<uint64_t> checksum_errors_;
    std::atomic<uint64_t> sequence_errors_;
    uint32_t last_sequence_;
    
public:
    PLUGIN_INFO("P25 TSBK UDP Input", "1.0.0", "Dave K9DPD", "Receives P25 TSBK control data from trunk-recorder via UDP")
    
    P25_TSBK_UDP_Input() : 
        listen_address_("127.0.0.1"), 
        listen_port_(9999), 
        buffer_size_(8192),
        validate_checksums_(true),
        verbose_(false),
        socket_fd_(-1),
        running_(false),
        max_queue_size_(1000),
        packets_received_(0),
        packets_dropped_(0),
        bytes_received_(0),
        checksum_errors_(0),
        sequence_errors_(0),
        last_sequence_(0) {}
    
    virtual ~P25_TSBK_UDP_Input() {
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
        receiver_thread_ = std::thread(&P25_TSBK_UDP_Input::receiver_worker, this);
        
        set_state(Plugin_State::PLUGIN_RUNNING);
        
        if (verbose_) {
            std::cout << "[P25_TSBK_UDP_Input] Started listening on " 
                      << listen_address_ << ":" << listen_port_ << std::endl;
        }
        
        return 0;
    }
    
    virtual int stop() override {
        if (running_) {
            running_ = false;
            
            if (receiver_thread_.joinable()) {
                receiver_thread_.join();
            }
            
            if (socket_fd_ >= 0) {
                close(socket_fd_);
                socket_fd_ = -1;
            }
            
            set_state(Plugin_State::PLUGIN_STOPPED);
            
            if (verbose_) {
                std::cout << "[P25_TSBK_UDP_Input] Stopped. Stats: " 
                          << packets_received_ << " received, " 
                          << packets_dropped_ << " dropped" << std::endl;
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
        
        if (config_data.contains("buffer_size")) {
            buffer_size_ = config_data["buffer_size"];
        }
        
        if (config_data.contains("max_queue_size")) {
            max_queue_size_ = config_data["max_queue_size"];
        }
        
        if (config_data.contains("validate_checksums")) {
            validate_checksums_ = config_data["validate_checksums"];
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
        stats["packets_received"] = packets_received_.load();
        stats["packets_dropped"] = packets_dropped_.load();
        stats["bytes_received"] = bytes_received_.load();
        stats["checksum_errors"] = checksum_errors_.load();
        stats["sequence_errors"] = sequence_errors_.load();
        stats["queue_size"] = data_queue_.size();
        return stats;
    }
    
private:
    int initialize_socket() {
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            std::cerr << "[P25_TSBK_UDP_Input] Failed to create socket: " << strerror(errno) << std::endl;
            return -1;
        }
        
        // Allow address reuse
        int reuse = 1;
        if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            std::cerr << "[P25_TSBK_UDP_Input] Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
        
        // Bind to address
        memset(&server_addr_, 0, sizeof(server_addr_));
        server_addr_.sin_family = AF_INET;
        server_addr_.sin_port = htons(listen_port_);
        
        if (inet_pton(AF_INET, listen_address_.c_str(), &server_addr_.sin_addr) <= 0) {
            std::cerr << "[P25_TSBK_UDP_Input] Invalid IP address: " << listen_address_ << std::endl;
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
        
        if (bind(socket_fd_, (struct sockaddr*)&server_addr_, sizeof(server_addr_)) < 0) {
            std::cerr << "[P25_TSBK_UDP_Input] Failed to bind to " << listen_address_ 
                      << ":" << listen_port_ << ": " << strerror(errno) << std::endl;
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
        
        return 0;
    }
    
    void receiver_worker() {
        std::vector<uint8_t> buffer(buffer_size_);
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        while (running_) {
            ssize_t bytes_received = recvfrom(socket_fd_, buffer.data(), buffer.size(), 0,
                                            (struct sockaddr*)&client_addr, &client_len);
            
            if (bytes_received < 0) {
                if (running_) {
                    std::cerr << "[P25_TSBK_UDP_Input] recvfrom error: " << strerror(errno) << std::endl;
                }
                continue;
            }
            
            if (bytes_received == 0) {
                continue;
            }
            
            bytes_received_ += bytes_received;
            
            // Parse P25C packet
            P25_TSBK_Data tsbk_data;
            if (parse_p25c_packet(buffer.data(), bytes_received, tsbk_data)) {
                packets_received_++;
                
                // Check queue size
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    if (data_queue_.size() >= max_queue_size_) {
                        packets_dropped_++;
                        if (verbose_) {
                            std::cout << "[P25_TSBK_UDP_Input] Queue full, dropping packet" << std::endl;
                        }
                        continue;
                    }
                    
                    data_queue_.push(tsbk_data);
                }
                
                queue_cv_.notify_one();
                
                // Call callback if set
                if (data_callback_) {
                    data_callback_(tsbk_data);
                }
            }
        }
    }
    
    bool parse_p25c_packet(const uint8_t* data, size_t length, P25_TSBK_Data& tsbk_data) {
        // Minimum packet size check
        if (length < sizeof(uint32_t) * 8 + sizeof(uint16_t) * 2 + sizeof(uint64_t) + sizeof(double)) {
            if (verbose_) {
                std::cout << "[P25_TSBK_UDP_Input] Packet too small: " << length << " bytes" << std::endl;
            }
            return false;
        }
        
        size_t offset = 0;
        
        // Parse header
        memcpy(&tsbk_data.magic, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        
        // Check magic number
        if (tsbk_data.magic != 0x50323543) { // "P25C"
            if (verbose_) {
                std::cout << "[P25_TSBK_UDP_Input] Invalid magic: 0x" << std::hex << tsbk_data.magic << std::endl;
            }
            return false;
        }
        
        memcpy(&tsbk_data.version, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        
        memcpy(&tsbk_data.timestamp_us, data + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        
        memcpy(&tsbk_data.sequence_number, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        
        memcpy(&tsbk_data.system_id, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        
        memcpy(&tsbk_data.site_id, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        
        memcpy(&tsbk_data.frequency, data + offset, sizeof(double));
        offset += sizeof(double);
        
        // DEBUG: Print raw bytes and parsed values (disabled to reduce output noise)
        // static int debug_count = 0;
        // if (debug_count < 3) { // Only show first 3 packets to avoid spam
        //     std::cout << "[DEBUG] Packet length: " << length << " bytes" << std::endl;
        //     std::cout << "[DEBUG] Magic: 0x" << std::hex << tsbk_data.magic << std::dec << std::endl;
        //     std::cout << "[DEBUG] System ID: 0x" << std::hex << tsbk_data.system_id << std::dec << std::endl;
        //     std::cout << "[DEBUG] Frequency: " << tsbk_data.frequency << " Hz" << std::endl;
        //     std::cout << "[DEBUG] Raw frequency bytes: ";
        //     for (int i = 0; i < 8; i++) {
        //         std::cout << "0x" << std::hex << (int)data[offset - 8 + i] << " ";
        //     }
        //     std::cout << std::dec << std::endl;
        //     debug_count++;
        // }
        
        memcpy(&tsbk_data.sample_rate, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        
        memcpy(&tsbk_data.data_length, data + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
        
        memcpy(&tsbk_data.checksum, data + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
        
        // Validate data length
        if (offset + tsbk_data.data_length > length) {
            if (verbose_) {
                std::cout << "[P25_TSBK_UDP_Input] Invalid data length: " << tsbk_data.data_length << std::endl;
            }
            return false;
        }
        
        // Extract TSBK data
        tsbk_data.tsbk_data.resize(tsbk_data.data_length);
        memcpy(tsbk_data.tsbk_data.data(), data + offset, tsbk_data.data_length);
        
        // Validate checksum if enabled
        if (validate_checksums_ && tsbk_data.checksum != 0) {
            uint16_t calculated_checksum = calculate_checksum(tsbk_data.tsbk_data.data(), tsbk_data.data_length);
            if (calculated_checksum != tsbk_data.checksum) {
                checksum_errors_++;
                if (verbose_) {
                    std::cout << "[P25_TSBK_UDP_Input] Checksum mismatch: got 0x" << std::hex 
                              << tsbk_data.checksum << ", expected 0x" << calculated_checksum << std::endl;
                }
                return false;
            }
        }
        
        // Check sequence number
        if (last_sequence_ != 0 && tsbk_data.sequence_number != 0) {
            uint32_t expected_seq = last_sequence_ + 1;
            if (tsbk_data.sequence_number != expected_seq) {
                sequence_errors_++;
                if (verbose_) {
                    std::cout << "[P25_TSBK_UDP_Input] Sequence error: got " << tsbk_data.sequence_number 
                              << ", expected " << expected_seq << std::endl;
                }
            }
        }
        last_sequence_ = tsbk_data.sequence_number;
        
        // Set metadata
        tsbk_data.source_name = get_plugin_name();
        tsbk_data.received_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        return true;
    }
    
    uint16_t calculate_checksum(const uint8_t* data, size_t length) {
        uint16_t checksum = 0;
        for (size_t i = 0; i < length; i++) {
            checksum ^= data[i];
        }
        return checksum;
    }
};

// Plugin factory
extern "C" std::shared_ptr<Input_Plugin_Api> create_input_plugin() {
    return std::make_shared<P25_TSBK_UDP_Input>();
}