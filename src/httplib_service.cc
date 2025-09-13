#include "httplib_service.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

HttplibService::HttplibService(int port, const std::string& output_dir, bool verbose, bool foreground) 
    : port_(port), output_dir_(output_dir), verbose_(verbose), foreground_(foreground), 
      running_(false), require_auth_(false), use_https_(false) {
    
    server_ = std::make_unique<httplib::Server>();
    
    // Register endpoints
    server_->Post("/api/v1/decode", [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_decode_request(req, res);
    });
    
    server_->Get("/api/v1/status", [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_status_request(req, res);
    });
    
    // Enable CORS if needed
    server_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        return httplib::Server::HandlerResponse::Unhandled;
    });
}

HttplibService::~HttplibService() {
    stop();
}

bool HttplibService::start() {
    // Create output directory if needed
    try {
        std::filesystem::create_directories(output_dir_);
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Failed to create output directory: " << e.what() << std::endl;
        return false;
    }
    
    running_ = true;
    
    if (foreground_) {
        // Run on main thread (blocking)
        std::cout << "Starting HTTP API service on port " << port_ << std::endl;
        bool result = server_->listen("0.0.0.0", port_);
        running_ = false;
        return result;
    } else {
        // Run in background thread (non-blocking)
        std::thread([this]() {
            std::cout << "Starting HTTP API service on port " << this->port_ << std::endl;
            this->server_->listen("0.0.0.0", this->port_);
            this->running_ = false;
        }).detach();
        
        // Give it a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        return server_->is_running();
    }
}

void HttplibService::stop() {
    if (server_ && running_) {
        server_->stop();
        running_ = false;
    }
}

bool HttplibService::is_running() const {
    return running_ && server_ && server_->is_running();
}

void HttplibService::set_auth_token(const std::string& token) {
    auth_token_ = token;
    require_auth_ = !token.empty();
}

void HttplibService::enable_tls(const std::string& cert_file, const std::string& key_file) {
    ssl_cert_file_ = cert_file;
    ssl_key_file_ = key_file;
    use_https_ = !cert_file.empty() && !key_file.empty();
}

bool HttplibService::validate_auth_token(const httplib::Request& request) {
    if (!require_auth_) {
        return true;
    }
    
    // Check Authorization header for Bearer token
    if (request.has_header("Authorization")) {
        std::string auth_value = request.get_header_value("Authorization");
        if (auth_value.substr(0, 7) == "Bearer ") {
            std::string token = auth_value.substr(7);
            return token == auth_token_;
        }
    }
    
    // Check X-API-Key header as alternative
    if (request.has_header("X-API-Key")) {
        std::string api_key = request.get_header_value("X-API-Key");
        return api_key == auth_token_;
    }
    
    return false;
}

void HttplibService::handle_status_request(const httplib::Request& req, httplib::Response& res) {
    std::ostringstream json;
    json << "{"
         << "\"status\": \"ok\","
         << "\"service\": \"trunk-decoder\","
         << "\"version\": \"1.0\""
         << "}";
    
    res.set_content(json.str(), "application/json");
}

void HttplibService::handle_decode_request(const httplib::Request& req, httplib::Response& res) {
    try {
        if (verbose_) {
            std::cout << "[API] Received decode request from client" << std::endl;
            std::cout << "[API] Method: " << req.method << std::endl;
            std::cout << "[API] Content-Type: " << req.get_header_value("Content-Type") << std::endl;
            
            // Log Content-Length if present
            if (req.has_header("Content-Length")) {
                std::cout << "[API] Content-Length: " << req.get_header_value("Content-Length") << " bytes" << std::endl;
            }
            
            // Log total body size received
            std::cout << "[API] Request body size: " << req.body.size() << " bytes" << std::endl;
            std::cout << "[API] Processing multipart request" << std::endl;
        }
        
        // Always log total bytes received (even in non-verbose mode)
        std::cout << "[API] ===== TOTAL BYTES RECEIVED: " << req.body.size() << " bytes =====" << std::endl;
        
        // Validate authentication
        if (!validate_auth_token(req)) {
            if (verbose_) std::cout << "[API] Authentication failed" << std::endl;
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer realm=\"trunk-decoder\"");
            res.set_content("{\"error\": \"Authentication required. Provide valid auth token.\"}", "application/json");
            return;
        }
        
        // Check for P25 file
        auto files = req.files;
        auto p25_file_it = files.find("p25_file");
        if (p25_file_it == files.end()) {
            res.status = 400;
            res.set_content("{\"error\": \"Missing p25_file in request\"}", "application/json");
            return;
        }
        
        const auto& p25_file = p25_file_it->second;
        
        // Get metadata if provided
        std::string metadata_str;
        if (req.has_param("metadata")) {
            metadata_str = req.get_param_value("metadata");
        }
        
        if (verbose_) {
            std::cout << "[API] Processing P25 file: " << p25_file.filename << " (" << p25_file.content.size() << " bytes)" << std::endl;
            std::cout << "[API] File content first 10 bytes: ";
            for (size_t i = 0; i < std::min((size_t)10, p25_file.content.size()); i++) {
                printf("0x%02X ", (unsigned char)p25_file.content[i]);
            }
            std::cout << std::endl;
            if (!metadata_str.empty()) {
                std::cout << "[API] Metadata: " << metadata_str << std::endl;
            }
        }
        
        // Create temporary file from uploaded content
        std::string temp_filename = "/tmp/trunk_decoder_" + std::to_string(std::time(nullptr)) + "_" + p25_file.filename;
        std::ofstream temp_file(temp_filename, std::ios::binary);
        temp_file.write(p25_file.content.c_str(), p25_file.content.size());
        temp_file.close();
        
        // Verify file size matches uploaded content
        std::ifstream check_file(temp_filename, std::ios::binary | std::ios::ate);
        size_t written_size = check_file.tellg();
        check_file.close();
        
        if (verbose_) {
            std::cout << "[API] File written: " << written_size << " bytes (expected " << p25_file.content.size() << " bytes)" << std::endl;
        }
        
        if (written_size != p25_file.content.size()) {
            std::cout << "[API] WARNING: File size mismatch! Expected " << p25_file.content.size() 
                      << " bytes, wrote " << written_size << " bytes" << std::endl;
        }
        
        // Generate output filenames
        std::string base_filename = "api_call_" + std::to_string(std::time(nullptr));
        std::string output_base = output_dir_ + "/" + base_filename;
        std::string wav_file = output_base + ".wav";
        std::string json_file = output_base + ".json";
        
        // Process the P25 file
        if (!decoder_.open_p25_file(temp_filename)) {
            res.status = 400;
            res.set_content("{\"error\": \"Failed to open P25 file\"}", "application/json");
            cleanup_temp_file(temp_filename);
            return;
        }
        
        // Decode the file
        if (!decoder_.decode_to_audio(output_base)) {
            res.status = 500;
            res.set_content("{\"error\": \"Failed to decode P25 file\"}", "application/json");
            cleanup_temp_file(temp_filename);
            return;
        }
        
        // Generate outputs and build response
        bool success = true;
        std::ostringstream json_response;
        
        json_response << "{";
        json_response << "\"success\": " << (success ? "true" : "false") << ",";
        json_response << "\"files\": {";
        
        bool first_file = true;
        if (std::filesystem::exists(wav_file)) {
            json_response << "\"wav_file\": \"" << wav_file << "\"";
            first_file = false;
        }
        
        if (decoder_.save_json_metadata(json_file) && std::filesystem::exists(json_file)) {
            if (!first_file) json_response << ",";
            json_response << "\"json_file\": \"" << json_file << "\"";
            first_file = false;
        }
        
        json_response << "},";
        
        // Add statistics
        auto stats = decoder_.get_call_metadata();
        json_response << "\"stats\": {";
        json_response << "\"frames_processed\": " << stats.total_frames << ",";
        json_response << "\"voice_frames\": " << stats.voice_frames << ",";
        json_response << "\"talkgroup\": " << stats.talkgroup << ",";
        json_response << "\"duration_seconds\": " << stats.call_length;
        json_response << "}";
        json_response << "}";
        
        res.status = 200;
        res.set_content(json_response.str(), "application/json");
        
        if (verbose_) {
            std::cout << "[API] Successfully processed P25 file" << std::endl;
        }
        
        // Cleanup
        cleanup_temp_file(temp_filename);
        
    } catch (const std::exception& e) {
        res.status = 500;
        std::ostringstream error_json;
        error_json << "{\"error\": \"Internal error: " << e.what() << "\"}";
        res.set_content(error_json.str(), "application/json");
        
        if (verbose_) {
            std::cerr << "[API] Error processing request: " << e.what() << std::endl;
        }
    }
}

void HttplibService::cleanup_temp_file(const std::string& filepath) {
    try {
        std::filesystem::remove(filepath);
    } catch (const std::filesystem::filesystem_error& e) {
        if (verbose_) {
            std::cerr << "[API] Warning: Failed to cleanup temp file " << filepath 
                     << ": " << e.what() << std::endl;
        }
    }
}