#pragma once
#include <string>
#include <functional>
#include <memory>
#include <map>
#include <vector>

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

struct FileUpload {
    std::string temp_path;
    std::string original_filename;
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string content_type;
    std::vector<uint8_t> body;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> files; // For multipart form data (temp paths)
    std::map<std::string, FileUpload> file_uploads; // For detailed file info
    std::map<std::string, std::string> form_data;
};

struct HttpResponse {
    int status_code = 200;
    std::string content_type = "application/json";
    std::vector<uint8_t> body;
    std::map<std::string, std::string> headers;
    
    void set_json(const std::string& json_str) {
        content_type = "application/json";
        body = std::vector<uint8_t>(json_str.begin(), json_str.end());
    }
    
    void set_text(const std::string& text) {
        content_type = "text/plain";
        body = std::vector<uint8_t>(text.begin(), text.end());
    }
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

class HttpService {
private:
    int port_;
    bool running_;
    bool use_tls_;
    bool debug_enabled_;
    std::string cert_file_;
    std::string key_file_;
    std::map<std::string, HttpHandler> handlers_;
    
#ifdef HAVE_OPENSSL
    SSL_CTX *ssl_ctx_;
    void init_ssl();
    void cleanup_ssl();
    void handle_client_ssl(SSL* ssl);
#endif
    
    void handle_client(int client_socket);
    HttpRequest parse_request(const std::string& request_data);
    std::string create_response(const HttpResponse& response);
    bool parse_multipart_form_data(const std::string& content_type, 
                                  const std::vector<uint8_t>& body, 
                                  HttpRequest& request);
    
public:
    HttpService(int port) : port_(port), running_(false), use_tls_(false), debug_enabled_(false) {
#ifdef HAVE_OPENSSL
        ssl_ctx_ = nullptr;
#endif
    }
    
    void add_handler(const std::string& path, HttpHandler handler);
    bool start();
    void stop();
    bool is_running() const { return running_; }
    
    // TLS configuration
    void enable_tls(const std::string& cert_file, const std::string& key_file) {
        cert_file_ = cert_file;
        key_file_ = key_file;
        use_tls_ = true;
    }
    
    // Debug configuration
    void enable_debug(bool enable = true) {
        debug_enabled_ = enable;
    }
};