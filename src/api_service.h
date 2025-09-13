#pragma once
#include "http_service.h"
#include "p25_decoder.h"
#include <memory>

class ApiService {
private:
    std::unique_ptr<HttpService> http_service_;
    P25Decoder decoder_;
    std::string output_dir_;
    bool verbose_;
    bool foreground_;
    std::string auth_token_;
    std::string ssl_cert_file_;
    std::string ssl_key_file_;
    std::string upload_script_;
    std::string audio_format_;
    int audio_bitrate_;
    
    void handle_decode_request(const HttpRequest& request, HttpResponse& response);
    std::string create_temp_file(const std::vector<uint8_t>& data, const std::string& extension);
    void cleanup_temp_file(const std::string& filepath);
    bool validate_auth_token(const HttpRequest& request);
    
public:
    ApiService(int port, const std::string& output_dir, bool verbose = false, bool foreground = false);
    ~ApiService();
    
    bool start();
    void stop();
    bool is_running() const;
    
    // Security configuration
    void set_auth_token(const std::string& token) { auth_token_ = token; }
    void enable_https(const std::string& cert_file, const std::string& key_file) { 
        ssl_cert_file_ = cert_file; ssl_key_file_ = key_file; 
    }
    void set_upload_script(const std::string& script) { upload_script_ = script; }
    void set_audio_format(const std::string& format) { audio_format_ = format; }
    void set_audio_bitrate(int bitrate) { audio_bitrate_ = bitrate; }
    
    // Placeholder methods for compatibility - these would be implemented in HttplibService
    void enable_decryption(bool enabled) { /* TODO: pass to decoder */ }
    void add_des_key(uint16_t key_id, const std::vector<uint8_t>& key) { /* TODO: pass to decoder */ }
    void add_aes_key(uint16_t key_id, const std::vector<uint8_t>& key) { /* TODO: pass to decoder */ }
    void add_adp_key(uint16_t key_id, const std::vector<uint8_t>& key) { /* TODO: pass to decoder */ }
};