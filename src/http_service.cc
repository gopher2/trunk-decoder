#include "http_service.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <regex>

#ifdef HAVE_OPENSSL
void HttpService::init_ssl() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    
    const SSL_METHOD *method = TLS_server_method();
    ssl_ctx_ = SSL_CTX_new(method);
    
    if (!ssl_ctx_) {
        std::cerr << "Failed to create SSL context" << std::endl;
        return;
    }
    
    // Load certificate and private key
    if (SSL_CTX_use_certificate_file(ssl_ctx_, cert_file_.c_str(), SSL_FILETYPE_PEM) <= 0) {
        std::cerr << "Failed to load certificate file: " << cert_file_ << std::endl;
        ERR_print_errors_fp(stderr);
        return;
    }
    
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, key_file_.c_str(), SSL_FILETYPE_PEM) <= 0) {
        std::cerr << "Failed to load private key file: " << key_file_ << std::endl;
        ERR_print_errors_fp(stderr);
        return;
    }
    
    // Verify private key matches certificate
    if (!SSL_CTX_check_private_key(ssl_ctx_)) {
        std::cerr << "Private key does not match the certificate" << std::endl;
        return;
    }
    
    std::cout << "SSL context initialized successfully" << std::endl;
}

void HttpService::cleanup_ssl() {
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
    EVP_cleanup();
}

void HttpService::handle_client_ssl(SSL* ssl) {
    char buffer[65536];
    int bytes_read = SSL_read(ssl, buffer, sizeof(buffer) - 1);
    
    if (bytes_read <= 0) {
        int ssl_error = SSL_get_error(ssl, bytes_read);
        if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
            std::cerr << "SSL_read error: " << ssl_error << std::endl;
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        return;
    }
    
    buffer[bytes_read] = '\0';
    std::string request_data(buffer);
    
    try {
        HttpRequest request = parse_request(request_data);
        HttpResponse response;
        
        // Find handler for this path
        auto handler_it = handlers_.find(request.path);
        if (handler_it != handlers_.end()) {
            handler_it->second(request, response);
        } else {
            response.status_code = 404;
            response.set_json("{\"error\": \"Not found\"}");
        }
        
        std::string response_str = create_response(response);
        SSL_write(ssl, response_str.c_str(), response_str.length());
        
    } catch (const std::exception& e) {
        std::string error_response = 
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 32\r\n"
            "\r\n"
            "{\"error\": \"Internal server error\"}";
        SSL_write(ssl, error_response.c_str(), error_response.length());
    }
    
    SSL_shutdown(ssl);
    SSL_free(ssl);
}
#endif

void HttpService::add_handler(const std::string& path, HttpHandler handler) {
    handlers_[path] = handler;
}

bool HttpService::start() {
#ifdef HAVE_OPENSSL
    if (use_tls_) {
        init_ssl();
        if (!ssl_ctx_) {
            std::cerr << "Failed to initialize SSL context" << std::endl;
            return false;
        }
    }
#endif

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return false;
    }

    // Set socket options to allow address reuse
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        std::cerr << "Failed to set socket options" << std::endl;
        close(server_fd);
        return false;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        std::cerr << "Failed to bind to port " << port_ << std::endl;
        close(server_fd);
        return false;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "Failed to listen on socket" << std::endl;
        close(server_fd);
        return false;
    }

    running_ = true;
    std::cout << (use_tls_ ? "HTTPS" : "HTTP") << " service started on port " << port_ << std::endl;

    // Accept connections in a loop
    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) {
            if (running_) {
                std::cerr << "Failed to accept connection" << std::endl;
            }
            continue;
        }

#ifdef HAVE_OPENSSL
        if (use_tls_) {
            // Handle SSL connection
            std::thread([this, client_socket]() {
                SSL *ssl = SSL_new(this->ssl_ctx_);
                SSL_set_fd(ssl, client_socket);
                
                if (SSL_accept(ssl) <= 0) {
                    ERR_print_errors_fp(stderr);
                    SSL_free(ssl);
                    close(client_socket);
                    return;
                }
                
                this->handle_client_ssl(ssl);
                close(client_socket);
            }).detach();
        } else {
#endif
            // Handle regular HTTP connection
            std::thread([this, client_socket]() {
                this->handle_client(client_socket);
            }).detach();
#ifdef HAVE_OPENSSL
        }
#endif
    }

    close(server_fd);
    
#ifdef HAVE_OPENSSL
    if (use_tls_) {
        cleanup_ssl();
    }
#endif
    
    return true;
}

void HttpService::stop() {
    running_ = false;
}

void HttpService::handle_client(int client_socket) {
    if (debug_enabled_) std::cout << "[DEBUG] Client connected" << std::endl;
    std::string request_data;
    std::string headers;
    size_t content_length = 0;
    bool headers_complete = false;
    
    // First, read headers to determine Content-Length
    char buffer[1024];
    while (!headers_complete) {
        if (debug_enabled_) std::cout << "[DEBUG] Reading headers..." << std::endl;
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            if (debug_enabled_) std::cout << "[DEBUG] Connection closed or error: " << bytes_read << std::endl;
            close(client_socket);
            return;
        }
        
        if (debug_enabled_) std::cout << "[DEBUG] Read " << bytes_read << " bytes" << std::endl;
        buffer[bytes_read] = '\0';
        request_data += buffer;
        
        // Check if we have complete headers (ending with \r\n\r\n)
        size_t header_end = request_data.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            headers = request_data.substr(0, header_end + 4);
            headers_complete = true;
            if (debug_enabled_) std::cout << "[DEBUG] Headers complete, length: " << headers.length() << std::endl;
            
            // Extract Content-Length from headers
            size_t cl_pos = headers.find("Content-Length:");
            if (cl_pos != std::string::npos) {
                size_t cl_start = cl_pos + 15; // strlen("Content-Length:")
                size_t cl_end = headers.find("\r\n", cl_start);
                if (cl_end != std::string::npos) {
                    std::string cl_str = headers.substr(cl_start, cl_end - cl_start);
                    // Trim whitespace
                    cl_str.erase(0, cl_str.find_first_not_of(" \t"));
                    cl_str.erase(cl_str.find_last_not_of(" \t") + 1);
                    content_length = std::stoul(cl_str);
                    if (debug_enabled_) {
                        std::cout << "[DEBUG] Processing " << content_length << " bytes" << std::endl;
                    }
                }
            }
        }
    }
    
    // Now read the body if Content-Length is specified
    if (content_length > 0) {
        if (debug_enabled_) std::cout << "[DEBUG] Reading body, Content-Length: " << content_length << std::endl;
        if (debug_enabled_) std::cout << "[DEBUG] Total request_data length: " << request_data.length() << std::endl;
        // Find the actual body start position (after \r\n\r\n)
        size_t header_end_pos = request_data.find("\r\n\r\n");
        if (header_end_pos == std::string::npos) {
            if (debug_enabled_) std::cout << "[DEBUG] Header end not found!" << std::endl;
            close(client_socket);
            return;
        }
        if (debug_enabled_) std::cout << "[DEBUG] Header end position: " << header_end_pos << std::endl;
        size_t body_start = header_end_pos + 4; // Skip past \r\n\r\n
        if (debug_enabled_) std::cout << "[DEBUG] Body start position: " << body_start << std::endl;
        size_t body_already_read = request_data.length() - body_start;
        if (debug_enabled_) std::cout << "[DEBUG] Body already read calculation: " << request_data.length() << " - " << body_start << " = " << body_already_read << std::endl;
        
        // Sanity check: if we've already read all the body data, don't try to read more
        if (body_already_read >= content_length) {
            if (debug_enabled_) std::cout << "[DEBUG] All body data already read, skipping additional reads" << std::endl;
        } else {
            size_t body_remaining = content_length - body_already_read;
        if (debug_enabled_) std::cout << "[DEBUG] Body already read: " << body_already_read << ", remaining: " << body_remaining << std::endl;
        
        // Read remaining body data with timeout
        struct timeval timeout;
        timeout.tv_sec = 1;  // 1 second timeout
        timeout.tv_usec = 0;
        
        while (body_remaining > 0) {
            size_t to_read = std::min(body_remaining, sizeof(buffer));
            if (debug_enabled_) std::cout << "[DEBUG] Reading " << to_read << " bytes of body..." << std::endl;
            
            // Set socket timeout
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client_socket, &readfds);
            
            int ready = select(client_socket + 1, &readfds, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                if (debug_enabled_) std::cout << "[DEBUG] Socket timeout or error, assuming all data received" << std::endl;
                break;  // Timeout or error, assume we have all the data
            }
            
            ssize_t bytes_read = recv(client_socket, buffer, to_read, 0);
            if (bytes_read <= 0) {
                if (debug_enabled_) std::cout << "[DEBUG] Body read error: " << bytes_read << std::endl;
                break;  // Connection closed or error, assume we have all the data
            }
            
            if (debug_enabled_) std::cout << "[DEBUG] Read " << bytes_read << " bytes of body" << std::endl;
            request_data.append(buffer, bytes_read);
            body_remaining -= bytes_read;
        }
        if (debug_enabled_) std::cout << "[DEBUG] Body reading complete" << std::endl;
        }
    }
    
    // Only log size mismatches in debug mode
    if (debug_enabled_ && content_length > 0 && request_data.size() != content_length) {
        std::cout << "[DEBUG] Size mismatch: received " << request_data.size() 
                  << " bytes, expected " << content_length << std::endl;
    }
    
    try {
        HttpRequest request = parse_request(request_data);
        HttpResponse response;
        
        // Find handler for this path
        auto handler_it = handlers_.find(request.path);
        if (handler_it != handlers_.end()) {
            handler_it->second(request, response);
        } else {
            response.status_code = 404;
            response.set_json("{\"error\": \"Not found\"}");
        }
        
        std::string response_str = create_response(response);
        send(client_socket, response_str.c_str(), response_str.length(), 0);
        
    } catch (const std::exception& e) {
        std::string error_response = 
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 23\r\n"
            "\r\n"
            "{\"error\": \"Internal server error\"}";
        send(client_socket, error_response.c_str(), error_response.length(), 0);
    }
    
    close(client_socket);
}

HttpRequest HttpService::parse_request(const std::string& request_data) {
    HttpRequest request;
    
    std::istringstream iss(request_data);
    std::string line;
    
    // Parse request line
    if (std::getline(iss, line)) {
        std::istringstream line_iss(line);
        line_iss >> request.method >> request.path;
    }
    
    // Parse headers
    size_t content_length = 0;
    while (std::getline(iss, line) && line != "\r") {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 2);
            
            // Remove trailing \r if present
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
            
            request.headers[key] = value;
            
            if (key == "Content-Type") {
                request.content_type = value;
            } else if (key == "Content-Length") {
                content_length = std::stoul(value);
            }
        }
    }
    
    // Read body if present
    if (content_length > 0) {
        // Find the body start position (after \r\n\r\n)
        size_t body_start = request_data.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            body_start += 4; // Skip past \r\n\r\n
            
            // Extract body data directly from request_data to preserve binary integrity
            if (body_start < request_data.length()) {
                std::string body_data = request_data.substr(body_start);
                request.body = std::vector<uint8_t>(body_data.begin(), body_data.end());
            }
        }
        
        // Parse multipart form data if applicable
        if (request.content_type.find("multipart/form-data") != std::string::npos) {
            parse_multipart_form_data(request.content_type, request.body, request);
        }
    }
    
    return request;
}

bool HttpService::parse_multipart_form_data(const std::string& content_type, 
                                           const std::vector<uint8_t>& body, 
                                           HttpRequest& request) {
    if (debug_enabled_) std::cout << "[HTTP] Parsing multipart data, content-type: " << content_type << std::endl;
    if (debug_enabled_) std::cout << "[HTTP] Body size: " << body.size() << " bytes" << std::endl;
    
    // Extract boundary from content type
    std::regex boundary_regex("boundary=([^;\\s]+)");
    std::smatch match;
    if (!std::regex_search(content_type, match, boundary_regex)) {
        if (debug_enabled_) std::cout << "[HTTP] Failed to extract boundary from content-type" << std::endl;
        return false;
    }
    
    std::string boundary = "--" + match[1].str();
    if (debug_enabled_) std::cout << "[HTTP] Extracted boundary: " << boundary << std::endl;
    std::string body_str(body.begin(), body.end());
    
    // Debug: show raw body content
    if (debug_enabled_) std::cout << "[HTTP] Raw body content (" << body_str.size() << " bytes):" << std::endl;
    if (body_str.size() < 500) {
        if (debug_enabled_) std::cout << "[HTTP] Body: " << body_str << std::endl;
    } else {
        if (debug_enabled_) std::cout << "[HTTP] Body (first 500 chars): " << body_str.substr(0, 500) << std::endl;
    }
    
    // Split by boundary - need to process parts between boundaries
    size_t start = body_str.find(boundary);
    if (start == std::string::npos) {
        if (debug_enabled_) std::cout << "[HTTP] No boundary found in body" << std::endl;
        return false;
    }
    
    if (debug_enabled_) std::cout << "[HTTP] Found boundary at position: " << start << std::endl;
    if (debug_enabled_) std::cout << "[HTTP] Processing multipart sections..." << std::endl;
    int part_count = 0;
    
    while (start != std::string::npos) {
        // Find next boundary
        size_t next_boundary = body_str.find(boundary, start + boundary.length());
        if (next_boundary == std::string::npos) {
            // Check for final boundary with --
            next_boundary = body_str.find(boundary + "--", start + boundary.length());
        }
        
        if (next_boundary != std::string::npos) {
            part_count++;
            if (debug_enabled_) std::cout << "[HTTP] Processing part " << part_count << std::endl;
            
            // Extract the part content between boundaries
            std::string part = body_str.substr(start + boundary.length(), next_boundary - start - boundary.length());
            
            if (debug_enabled_) std::cout << "[HTTP] Part " << part_count << " size: " << part.size() << " bytes" << std::endl;
            if (part.size() > 0 && part.size() < 200) {
                if (debug_enabled_) std::cout << "[HTTP] Part content (first 200 chars): " << part.substr(0, 200) << std::endl;
            }
            
            if (!part.empty()) {
                // Remove leading newlines from part content
                size_t start_pos = part.find_first_not_of("\r\n");
                if (start_pos != std::string::npos) {
                    part = part.substr(start_pos);
                }
                
                // Find headers/content boundary manually to preserve binary data
                size_t headers_end = part.find("\r\n\r\n");
                if (headers_end == std::string::npos) {
                    headers_end = part.find("\n\n");
                    if (headers_end != std::string::npos) {
                        headers_end += 2; // Skip \n\n
                    }
                } else {
                    headers_end += 4; // Skip \r\n\r\n
                }
                
                if (headers_end == std::string::npos) {
                    continue; // Invalid part format
                }
                
                std::string headers_section = part.substr(0, headers_end - 4);
                std::string content = part.substr(headers_end);
                
                std::string name;
                std::string filename;
                bool is_file = false;
                
                // Parse headers manually to avoid stringstream corruption
                std::istringstream headers_stream(headers_section);
                std::string line;
                while (std::getline(headers_stream, line)) {
                    // Remove trailing \r if present
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    
                    if (debug_enabled_) std::cout << "[HTTP] Header line: '" << line << "'" << std::endl;
                    if (line.find("Content-Disposition:") != std::string::npos) {
                        if (debug_enabled_) std::cout << "[HTTP] Found Content-Disposition: " << line << std::endl;
                        // Extract name and filename
                        std::smatch name_match;
                        std::smatch filename_match;
                        std::regex name_regex("name=\"([^\"]+)\"");
                        if (std::regex_search(line, name_match, name_regex)) {
                            name = name_match[1].str();
                            if (debug_enabled_) std::cout << "[HTTP] Extracted name: " << name << std::endl;
                        }
                        
                        std::regex filename_regex("filename=\"([^\"]+)\"");
                        if (std::regex_search(line, filename_match, filename_regex)) {
                            filename = filename_match[1].str();
                            is_file = true;
                            if (debug_enabled_) std::cout << "[HTTP] Extracted filename: " << filename << std::endl;
                        }
                    }
                }
                
                if (debug_enabled_) std::cout << "[HTTP] Found field: " << name << (is_file ? " (file)" : " (text)") << std::endl;
                
                if (is_file) {
                    // For file uploads, we'll write to a temporary file
                    std::string temp_filename = "/tmp/trunk_decoder_upload_" + std::to_string(time(nullptr)) + "_" + filename;
                    std::ofstream temp_file(temp_filename, std::ios::binary);
                    temp_file.write(content.c_str(), content.length());
                    temp_file.close();
                    
                    // Store both temp path and original filename
                    request.files[name] = temp_filename;
                    FileUpload upload;
                    upload.temp_path = temp_filename;
                    upload.original_filename = filename;
                    request.file_uploads[name] = upload;
                } else {
                    request.form_data[name] = content;
                }
            }
        }
        
        // Move to next boundary
        start = next_boundary;
    }
    
    return true;
}

std::string HttpService::create_response(const HttpResponse& response) {
    std::ostringstream oss;
    
    // Status line
    oss << "HTTP/1.1 " << response.status_code;
    switch (response.status_code) {
        case 200: oss << " OK"; break;
        case 400: oss << " Bad Request"; break;
        case 404: oss << " Not Found"; break;
        case 500: oss << " Internal Server Error"; break;
        default: oss << " Unknown"; break;
    }
    oss << "\r\n";
    
    // Headers
    oss << "Content-Type: " << response.content_type << "\r\n";
    oss << "Content-Length: " << response.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    
    for (const auto& header : response.headers) {
        oss << header.first << ": " << header.second << "\r\n";
    }
    
    oss << "\r\n";
    
    // Body
    std::string response_str = oss.str();
    response_str.append(response.body.begin(), response.body.end());
    
    return response_str;
}