#pragma once

#include "../include/httplib.h"
#include "p25_decoder.h"
#include <string>
#include <functional>
#include <memory>

class HttplibService {
public:
    HttplibService(int port, const std::string& output_dir, bool verbose = false, bool foreground = false);
    ~HttplibService();
    
    bool start();
    void stop();
    bool is_running() const;
    
    void set_auth_token(const std::string& token);
    void enable_tls(const std::string& cert_file, const std::string& key_file);

private:
    void handle_decode_request(const httplib::Request& req, httplib::Response& res);
    void handle_status_request(const httplib::Request& req, httplib::Response& res);
    bool validate_auth_token(const httplib::Request& request);
    void cleanup_temp_file(const std::string& filepath);
    
    std::unique_ptr<httplib::Server> server_;
    int port_;
    std::string output_dir_;
    bool verbose_;
    bool foreground_;
    bool running_;
    
    // Authentication
    bool require_auth_;
    std::string auth_token_;
    
    // TLS
    bool use_https_;
    std::string ssl_cert_file_;
    std::string ssl_key_file_;
    
    // Decoder
    P25Decoder decoder_;
};