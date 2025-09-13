#include "api_service.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <map>
#include <ctime>

ApiService::ApiService(int port, const std::string& output_dir, bool verbose, bool foreground) 
    : output_dir_(output_dir), verbose_(verbose), foreground_(foreground), audio_format_("wav"), audio_bitrate_(0) {
    http_service_ = std::make_unique<HttpService>(port);
    
    // Register API endpoints
    http_service_->add_handler("/api/v1/decode", 
        [this](const HttpRequest& req, HttpResponse& resp) {
            this->handle_decode_request(req, resp);
        });
        
    http_service_->add_handler("/api/v1/status", 
        [this](const HttpRequest& req, HttpResponse& resp) {
            resp.set_json("{\"status\": \"ok\", \"service\": \"trunk-decoder\", \"version\": \"1.0\"}");
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
        
        // Parse and log call information for multi-system tracking
        std::string system_name = "Unknown";
        std::string short_name = "Unknown";
        std::string talkgroup = "Unknown";
        std::string frequency = "Unknown";
        std::string call_id = "Unknown";
        std::string src_radio_id = "Unknown";
        std::string encrypted_status = "Clear";
        std::string site_name = "N/A";  // Unimplemented
        std::string wacn = "N/A";       // Unimplemented  
        std::string nac = "N/A";        // Unimplemented
        std::string rfss = "N/A";       // Unimplemented
        std::string site_id = "N/A";    // Unimplemented
        
        // Debug: Show what metadata we actually received
        if (verbose_ && !metadata_str.empty()) {
            std::cout << "[DEBUG] Received metadata: " << metadata_str.substr(0, 200) << "..." << std::endl;
        }
        
        if (!metadata_str.empty()) {
            try {
                // Extract key information from JSON metadata
                auto extract_json_field = [](const std::string& json, const std::string& field) -> std::string {
                    std::string search_pattern = "\"" + field + "\": ";
                    size_t pos = json.find(search_pattern);
                    if (pos != std::string::npos) {
                        pos += search_pattern.length();
                        if (json[pos] == '"') {
                            // String value
                            pos++;
                            size_t end = json.find('"', pos);
                            if (end != std::string::npos) {
                                return json.substr(pos, end - pos);
                            }
                        } else {
                            // Numeric value
                            size_t end = json.find_first_of(",\n}", pos);
                            if (end != std::string::npos) {
                                return json.substr(pos, end - pos);
                            }
                        }
                    }
                    return "Unknown";
                };
                
                system_name = extract_json_field(metadata_str, "short_name");
                short_name = system_name; // Same field for now
                talkgroup = extract_json_field(metadata_str, "talkgroup");
                call_id = extract_json_field(metadata_str, "call_num");
                frequency = extract_json_field(metadata_str, "freq");
                // Try multiple possible source radio ID fields
                src_radio_id = extract_json_field(metadata_str, "srcList");
                if (src_radio_id == "Unknown") {
                    src_radio_id = extract_json_field(metadata_str, "src");
                }
                if (src_radio_id == "Unknown") {
                    src_radio_id = extract_json_field(metadata_str, "source");
                }
                
                // System identifiers (extract if available in metadata)
                nac = extract_json_field(metadata_str, "nac");
                wacn = extract_json_field(metadata_str, "wacn");
                rfss = extract_json_field(metadata_str, "rfss");
                site_id = extract_json_field(metadata_str, "site_id");
                site_name = extract_json_field(metadata_str, "site_name");
                
                std::string encrypted_val = extract_json_field(metadata_str, "encrypted");
                if (encrypted_val == "1" || encrypted_val == "true") {
                    encrypted_status = "Encrypted";
                }
                
                // Format frequency for display
                if (frequency != "Unknown") {
                    try {
                        double freq_hz = std::stod(frequency);
                        double freq_mhz = freq_hz / 1000000.0;
                        frequency = std::to_string(freq_mhz).substr(0, 9) + " MHz";
                    } catch (...) {
                        // Keep original if conversion fails
                    }
                }
                
            } catch (const std::exception& e) {
                if (verbose_) {
                    std::cout << "[API] Warning: Failed to parse call metadata: " << e.what() << std::endl;
                }
            }
        }
        
        // Log comprehensive call information
        std::cout << "[DECODE] " << short_name << " | TG:" << talkgroup 
                  << " | SRC:" << src_radio_id << " | " << frequency 
                  << " | Call:" << call_id << " | " << encrypted_status;
                  
        // Add system identifiers if available
        if (nac != "Unknown" && nac != "N/A") std::cout << " | NAC:" << nac;
        if (wacn != "Unknown" && wacn != "N/A") std::cout << " | WACN:" << wacn;  
        if (rfss != "Unknown" && rfss != "N/A") std::cout << " | RFSS:" << rfss;
        if (site_id != "Unknown" && site_id != "N/A") std::cout << " | Site:" << site_id;
        if (site_name != "Unknown" && site_name != "N/A") std::cout << " | " << site_name;
        
        std::cout << std::endl;
        
        if (verbose_) {
            std::cout << "[API] Processing P25 file: " << p25_temp_file << std::endl;
        }
        
        // Get original filename from upload
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
        
        // Create folder structure based on metadata if available
        std::string folder_path = output_dir_;
        if (!metadata_str.empty()) {
            try {
                // Parse metadata to extract short_name and start_time
                size_t short_name_pos = metadata_str.find("\"short_name\": \"");
                size_t start_time_pos = metadata_str.find("\"start_time\": ");
                
                if (short_name_pos != std::string::npos && start_time_pos != std::string::npos) {
                    // Extract short_name
                    short_name_pos += 15; // length of "\"short_name\": \""
                    size_t short_name_end = metadata_str.find("\"", short_name_pos);
                    std::string short_name = metadata_str.substr(short_name_pos, short_name_end - short_name_pos);
                    
                    // Extract start_time
                    start_time_pos += 14; // length of "\"start_time\": "
                    size_t start_time_end = metadata_str.find_first_of(",\n}", start_time_pos);
                    std::string start_time_str = metadata_str.substr(start_time_pos, start_time_end - start_time_pos);
                    time_t start_time = std::stoll(start_time_str);
                    
                    // Convert to date components
                    struct tm* time_info = localtime(&start_time);
                    char year[5], month[3], day[3];
                    strftime(year, sizeof(year), "%Y", time_info);
                    strftime(month, sizeof(month), "%m", time_info);
                    strftime(day, sizeof(day), "%d", time_info);
                    
                    // Create folder structure: output_dir/SHORT_NAME/YEAR/MONTH/DAY/
                    folder_path = output_dir_ + "/" + short_name + "/" + year + "/" + month + "/" + day;
                    
                    // Create directories
                    std::filesystem::create_directories(folder_path);
                }
            } catch (const std::exception& e) {
                if (verbose_) {
                    std::cout << "[API] Warning: Failed to parse metadata for folder structure: " << e.what() << std::endl;
                }
            }
        }
        
        std::string output_base = folder_path + "/" + base_filename;
        std::string wav_file = output_base + ".wav";
        std::string json_file = output_base + ".json";
        
        // Configure decoder with audio format
        decoder_.set_audio_format(audio_format_);
        decoder_.set_audio_bitrate(audio_bitrate_);
        
        // Process the P25 file using the decoder
        if (!decoder_.open_p25_file(p25_temp_file)) {
            response.status_code = 400;
            response.set_json("{\"error\": \"Failed to open P25 file\"}");
            cleanup_temp_file(p25_temp_file);
            return;
        }
        
        // Decode the file
        if (!decoder_.decode_to_audio(output_base)) {
            response.status_code = 500;
            response.set_json("{\"error\": \"Failed to decode P25 file\"}");
            cleanup_temp_file(p25_temp_file);
            return;
        }
        
        // Write the metadata JSON directly if provided
        bool json_saved = false;
        if (!metadata_str.empty()) {
            std::ofstream json_out(json_file);
            if (json_out.is_open()) {
                json_out << metadata_str;
                json_out.close();
                json_saved = true;
            }
        } else {
            // No metadata provided - warn and skip JSON generation
            std::cout << "[WARNING] No metadata provided for call - JSON not created" << std::endl;
            json_saved = false;
        }
        
        // Check if files were generated
        bool wav_exists = std::filesystem::exists(wav_file);
        
        if (wav_exists && json_saved) {
            response.status_code = 200;
            response.set_json("{\"success\": true, \"message\": \"P25 file processed successfully\"}");
            
            if (verbose_) {
                std::cout << "[API] Successfully processed P25 file" << std::endl;
            }
            
            // Execute upload script if configured
            if (!upload_script_.empty() && std::filesystem::exists(upload_script_)) {
                try {
                    std::ostringstream cmd;
                    cmd << upload_script_ << " \"" << wav_file << "\" \"" << json_file << "\" \"1\"";
                    
                    if (verbose_) {
                        std::cout << "[API] Executing upload script: " << cmd.str() << std::endl;
                    }
                    
                    int result = std::system(cmd.str().c_str());
                    if (result != 0 && verbose_) {
                        std::cout << "[API] Upload script returned non-zero exit code: " << result << std::endl;
                    }
                } catch (const std::exception& e) {
                    if (verbose_) {
                        std::cerr << "[API] Error executing upload script: " << e.what() << std::endl;
                    }
                }
            }
        } else {
            response.status_code = 500;
            response.set_json("{\"error\": \"Failed to generate output files\"}");
        }
        
        // Cleanup temporary file
        cleanup_temp_file(p25_temp_file);
        
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