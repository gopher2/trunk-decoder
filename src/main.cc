/*
 * trunk-decoder - Main Application
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 * 
 * This application builds upon the trunk-recorder project's P25 implementation.
 * Special thanks to the trunk-recorder maintainers for their excellent work.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <map>
#include "p25_decoder.h"
#include "api_service.h"

// Simple JSON parsing for config (avoiding external dependencies)
#include <map>

namespace fs = std::filesystem;

// Configuration structure for service mode
struct DecoderConfig {
    std::string input_path;
    std::string output_dir = ".";
    bool enable_json = false;
    bool enable_wav = false;
    bool enable_text = false;
    bool recursive = false;
    bool verbose = false;
    bool quiet = false;
    bool foreground = false;
    
    // Service-specific settings
    std::string service_mode = "file"; // "file" or "api"
    std::string api_endpoint;
    int api_port = 3000;
    std::string auth_token;
    std::string ssl_cert;
    std::string ssl_key;
    std::map<std::string, std::string> metadata_fields;
    
    // Output settings
    int audio_sample_rate = 8000;
    std::string audio_format = "wav"; // "wav", "mp3", "m4a", "opus", "webm"
    int audio_bitrate = 0; // 0 = auto, otherwise kbps (e.g. 64, 128)
    bool include_frame_analysis = true;
    
    // Worker pool settings
    int worker_threads = 4;
    int queue_size = 1000;
    int batch_size = 10;
    int timeout_ms = 30000;
    
    // Ingest stream configurations
    struct IngestStream {
        std::string name;
        std::string system_name;
        int priority;
        int dedicated_workers;
        std::map<std::string, bool> output_formats; // format -> enabled
        std::map<std::string, int> format_bitrates; // format -> bitrate
        std::string upload_script;
        std::string output_dir;
    };
    std::vector<IngestStream> ingest_streams;
    
    // Processing options
    bool process_encrypted = true;
    bool skip_empty_frames = false;
    
    // Post-processing
    std::string upload_script;
    
    // Decryption keys
    struct KeyInfo {
        uint16_t keyid;
        std::vector<uint8_t> key;
        std::string description;
        std::string algorithm;
    };
    std::vector<KeyInfo> decryption_keys;
};

// Simple JSON implementation for parsing (from api_service.cc)
class SimpleJson {
public:
    std::map<std::string, std::string> values;
    std::map<std::string, std::map<std::string, std::string>> objects;
    
    bool parse_file(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;
        
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return parse_string(content);
    }
    
    bool parse_string(const std::string& json_str) {
        // Very basic JSON parser - handles simple key-value pairs and booleans
        std::string clean_str = json_str;
        
        // Remove braces, quotes, and whitespace
        size_t start = clean_str.find('{');
        size_t end = clean_str.rfind('}');
        if (start == std::string::npos || end == std::string::npos) return false;
        
        clean_str = clean_str.substr(start + 1, end - start - 1);
        
        // Split by commas and parse key-value pairs
        std::istringstream ss(clean_str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            
            std::string key = item.substr(0, colon);
            std::string value = item.substr(colon + 1);
            
            // Remove quotes and trim whitespace (including newlines)
            key.erase(std::remove(key.begin(), key.end(), '"'), key.end());
            key.erase(0, key.find_first_not_of(" \t\n\r"));
            key.erase(key.find_last_not_of(" \t\n\r") + 1);
            
            value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
            value.erase(0, value.find_first_not_of(" \t\n\r"));
            value.erase(value.find_last_not_of(" \t\n\r") + 1);
            
            values[key] = value;
        }
        return true;
    }
    
    std::string get(const std::string& key, const std::string& default_value = "") const {
        auto it = values.find(key);
        return (it != values.end()) ? it->second : default_value;
    }
    
    bool get_bool(const std::string& key, bool default_value = false) const {
        std::string val = get(key);
        return val == "true" || val == "1";
    }
};

// JSON config parser
bool parse_config_file(const std::string& config_path, DecoderConfig& config) {
    SimpleJson json;
    if (!json.parse_file(config_path)) {
        std::cerr << "Error: Cannot parse JSON config file: " << config_path << std::endl;
        return false;
    }
    
    
    // Parse configuration values from JSON
    config.input_path = json.get("input_path", config.input_path);
    config.output_dir = json.get("output_dir", config.output_dir);
    config.enable_json = json.get_bool("enable_json", config.enable_json);
    config.enable_wav = json.get_bool("enable_wav", config.enable_wav);
    config.enable_text = json.get_bool("enable_text", config.enable_text);
    config.recursive = json.get_bool("recursive", config.recursive);
    config.verbose = json.get_bool("verbose", config.verbose);
    config.quiet = json.get_bool("quiet", config.quiet);
    config.foreground = json.get_bool("foreground", config.foreground);
    config.service_mode = json.get("service_mode", config.service_mode);
    config.api_endpoint = json.get("api_endpoint", config.api_endpoint);
    config.api_port = std::stoi(json.get("api_port", std::to_string(config.api_port)));
    config.auth_token = json.get("auth_token", config.auth_token);
    config.ssl_cert = json.get("ssl_cert", config.ssl_cert);
    config.ssl_key = json.get("ssl_key", config.ssl_key);
    config.audio_format = json.get("audio_format", config.audio_format);
    config.process_encrypted = json.get_bool("process_encrypted", config.process_encrypted);
    config.skip_empty_frames = json.get_bool("skip_empty_frames", config.skip_empty_frames);
    config.include_frame_analysis = json.get_bool("include_frame_analysis", config.include_frame_analysis);
    config.upload_script = json.get("upload_script", config.upload_script);
    config.audio_bitrate = std::stoi(json.get("audio_bitrate", std::to_string(config.audio_bitrate)));
    
    // TODO: Parse decryption_keys array from JSON config
    // For now, use command line -k options for key specification
    
    return true;
}

void print_usage(const std::string& program_name) {
    std::cout << "Usage: " << program_name << " [options] [p25_file_or_directory]\n";
    std::cout << "       " << program_name << " -c <config.json>\n\n";
    std::cout << "trunk-decoder - Decode P25 files to audio and metadata\n";
    std::cout << "Audio codec support requires FFmpeg with appropriate codecs installed.\n";
    std::cout << "MP3 encoding may require patent licensing in some jurisdictions.\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -c, --config FILE       Use JSON config file for all settings\n";
    std::cout << "  -i, --input PATH        Input P25 file or directory\n";
    std::cout << "  -o, --output DIR        Output directory (default: current directory)\n";
    std::cout << "  -v, --verbose           Enable verbose output\n";
    std::cout << "  -q, --quiet             Quiet mode (minimal output)\n";
    std::cout << "  -r, --recursive         Process subdirectories recursively\n";
    std::cout << "  -f, --foreground        Run API service in foreground (don't fork)\n";
    std::cout << "  -k, --key KEYID:KEY     Add decryption key (hex format)\n";
    std::cout << "                          Key length determines algorithm:\n";
    std::cout << "                          5 bytes = ADP/RC4, 8 bytes = DES-OFB, 32 bytes = AES-256\n";
    std::cout << "  -b, --bitrate RATE      Audio bitrate in kbps (default: auto per format)\n\n";
    std::cout << "Output format options (must specify at least one):\n";
    std::cout << "  --json                  Generate JSON metadata files\n";
    std::cout << "  --wav                   Generate WAV audio files\n";
    std::cout << "  --text                  Generate text dump files\n";
    std::cout << "  --csv                   Generate CSV frame analysis files\n\n";
    std::cout << "Additional format options:\n";
    std::cout << "  --mp3                   Generate MP3 audio files (legacy compatibility)\n";
    std::cout << "  --m4a                   Generate M4A/AAC audio files (web-optimized)\n";
    std::cout << "  --opus                  Generate Opus audio files (best compression)\n";
    std::cout << "  --webm                  Generate WebM/Opus audio files (web native)\n";
    std::cout << "  --transcript            Generate voice transcription (unimplemented)\n\n";
    std::cout << "Input:\n";
    std::cout << "  Single file:            Process one .p25 file\n";
    std::cout << "  Directory:              Process all .p25 files in directory\n\n";
    std::cout << "Output files:\n";
    std::cout << "  FILENAME.wav            WAV audio file (16-bit, 8kHz, mono)\n";
    std::cout << "  FILENAME.json           Call metadata in JSON format\n";
    std::cout << "  FILENAME.txt            Text dump of P25 frame analysis\n";
    std::cout << "  FILENAME.csv            CSV frame data for spreadsheet analysis\n\n";
}

// Helper function to parse hex key string with algorithm detection
bool parse_encryption_key(const std::string& key_spec, uint16_t& keyid, std::vector<uint8_t>& key, std::string& algorithm) {
    // Format: KEYID:KEY where both are in hex
    size_t colon_pos = key_spec.find(':');
    if (colon_pos == std::string::npos) {
        std::cerr << "Error: Key format should be KEYID:KEY (hex format)" << std::endl;
        return false;
    }
    
    std::string keyid_str = key_spec.substr(0, colon_pos);
    std::string key_str = key_spec.substr(colon_pos + 1);
    
    // Parse key ID
    try {
        keyid = std::stoul(keyid_str, nullptr, 16);
    } catch (const std::exception& e) {
        std::cerr << "Error: Invalid key ID format: " << keyid_str << std::endl;
        return false;
    }
    
    // Parse key (should be even number of hex digits)
    if (key_str.length() % 2 != 0) {
        std::cerr << "Error: Key must have even number of hex digits" << std::endl;
        return false;
    }
    
    key.clear();
    for (size_t i = 0; i < key_str.length(); i += 2) {
        try {
            uint8_t byte = std::stoul(key_str.substr(i, 2), nullptr, 16);
            key.push_back(byte);
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid hex digit in key: " << key_str.substr(i, 2) << std::endl;
            return false;
        }
    }
    
    // Determine algorithm based on key length
    if (key.size() == 5) {
        algorithm = "ADP/RC4";
    } else if (key.size() == 8) {
        algorithm = "DES-OFB";
    } else if (key.size() == 32) {
        algorithm = "AES-256";
    } else {
        algorithm = "UNKNOWN";
    }
    
    return true;
}

std::string get_default_output_prefix(const std::string& input_filename) {
    // Remove .p25 extension and path to create default output prefix
    std::string basename = input_filename;
    
    // Remove path
    size_t last_slash = basename.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        basename = basename.substr(last_slash + 1);
    }
    
    // Remove .p25 extension
    if (basename.length() > 4 && basename.substr(basename.length() - 4) == ".p25") {
        basename = basename.substr(0, basename.length() - 4);
    }
    
    return basename;
}

std::vector<std::string> find_p25_files(const std::string& directory_path, bool recursive = false) {
    std::vector<std::string> p25_files;
    
    try {
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(directory_path)) {
                if (entry.is_regular_file() && 
                    entry.path().extension() == ".p25" &&
                    entry.file_size() > 0) {  // Skip empty files
                    p25_files.push_back(entry.path().string());
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(directory_path)) {
                if (entry.is_regular_file() && 
                    entry.path().extension() == ".p25" &&
                    entry.file_size() > 0) {  // Skip empty files
                    p25_files.push_back(entry.path().string());
                }
            }
        }
        
        // Sort files for consistent processing order
        std::sort(p25_files.begin(), p25_files.end());
        
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error accessing directory " << directory_path << ": " << e.what() << std::endl;
    }
    
    return p25_files;
}

bool process_single_file(const std::string& input_file, const std::string& output_dir, 
                        bool verbose, bool quiet, bool enable_json, bool enable_wav, bool enable_text, bool enable_csv,
                        const std::string& audio_format, int audio_bitrate, P25Decoder& decoder) {
    
    // Open P25 file
    if (!decoder.open_p25_file(input_file)) {
        if (!quiet) {
            std::cerr << "Error: Failed to open P25 file: " << input_file << std::endl;
        }
        return false;
    }

    // Create output path
    std::string filename = fs::path(input_file).stem().string();
    std::string output_prefix = fs::path(output_dir) / filename;

    if (!quiet) {
        std::cout << "Processing: " << fs::path(input_file).filename().string();
        if (verbose) {
            std::cout << " -> " << output_prefix;
        }
        std::cout << std::endl;
    }

    // Configure decoder
    decoder.enable_text_dump(enable_text || verbose); // Enable text dump if requested or verbose
    decoder.set_audio_format(audio_format);
    decoder.set_audio_bitrate(audio_bitrate);

    // Decode based on enabled formats
    bool success = true;
    
    if (enable_wav) {
        if (!decoder.decode_to_audio(output_prefix)) {
            if (!quiet) {
                std::cerr << "Error: Failed to decode P25 audio: " << input_file << std::endl;
            }
            success = false;
        }
    } else {
        // Still need to process P25 frames for metadata, just don't save audio
        if (!decoder.process_frames_only()) {
            if (!quiet) {
                std::cerr << "Error: Failed to process P25 frames: " << input_file << std::endl;
            }
            success = false;
        }
    }
    
    if (!success) {
        return false;
    }
    
    // Generate JSON metadata if requested
    if (enable_json) {
        std::string json_filename = output_prefix + ".json";
        if (!decoder.save_json_metadata(json_filename)) {
            if (!quiet) {
                std::cerr << "Warning: Failed to save JSON metadata: " << json_filename << std::endl;
            }
        }
    }
    
    // Generate text dump if requested
    if (enable_text) {
        std::string text_filename = output_prefix + ".txt";
        if (!decoder.save_text_dump(text_filename)) {
            if (!quiet) {
                std::cerr << "Warning: Failed to save text dump: " << text_filename << std::endl;
            }
        }
    }
    
    // Generate CSV dump if requested
    if (enable_csv) {
        std::string csv_filename = output_prefix + ".csv";
        if (!decoder.save_csv_dump(csv_filename)) {
            if (!quiet) {
                std::cerr << "Warning: Failed to save CSV dump: " << csv_filename << std::endl;
            }
        }
    }

    if (verbose) {
        const CallMetadata& metadata = decoder.get_call_metadata();
        std::cout << "  NAC: 0x" << std::hex << metadata.nac << std::dec;
        std::cout << ", Frames: " << metadata.total_frames;
        std::cout << ", Voice: " << metadata.voice_frames;
        std::cout << ", Duration: " << std::fixed << std::setprecision(2) << metadata.call_length << "s";
        std::cout << std::endl;
    }

    return true;
}

int main(int argc, char* argv[]) {
    try {
        std::string input_path;
        std::string output_dir = "."; // Default to current directory
        std::string config_file;
        std::string audio_format = "wav";
        int audio_bitrate = 0;
        bool verbose = false;
        bool quiet = false;
        bool recursive = false;
        bool foreground = false;
        bool show_help = false;
        bool enable_json = false;
        bool enable_wav = false;
        bool enable_text = false;
        bool enable_csv = false;
        bool use_config_file = false;
        std::map<uint16_t, std::vector<uint8_t>> des_keys;
        std::map<uint16_t, std::vector<uint8_t>> aes_keys;
        std::map<uint16_t, std::vector<uint8_t>> adp_keys;
        
        // Simple argument parsing
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            
            if (arg == "-h" || arg == "--help") {
                show_help = true;
            } else if (arg == "-v" || arg == "--verbose") {
                verbose = true;
            } else if (arg == "-q" || arg == "--quiet") {
                quiet = true;
            } else if (arg == "-r" || arg == "--recursive") {
                recursive = true;
            } else if (arg == "-f" || arg == "--foreground") {
                foreground = true;
            } else if (arg == "-c" || arg == "--config") {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    config_file = argv[++i];
                    use_config_file = true;
                } else {
                    std::cerr << "Error: -c requires an argument\n";
                    return 1;
                }
            } else if (arg == "-i" || arg == "--input") {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    input_path = argv[++i];
                } else {
                    std::cerr << "Error: -i requires an argument\n";
                    return 1;
                }
            } else if (arg == "-o" || arg == "--output") {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    output_dir = argv[++i];
                } else {
                    std::cerr << "Error: -o requires an argument\n";
                    return 1;
                }
            } else if (arg == "-b" || arg == "--bitrate") {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    audio_bitrate = std::stoi(argv[++i]);
                } else {
                    std::cerr << "Error: -b requires an argument\n";
                    return 1;
                }
            } else if (arg == "--json") {
                enable_json = true;
            } else if (arg == "--wav") {
                enable_wav = true;
            } else if (arg == "--text") {
                enable_text = true;
            } else if (arg == "--csv") {
                enable_csv = true;
            } else if (arg == "-k" || arg == "--key") {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    std::string key_spec = argv[++i];
                    uint16_t keyid;
                    std::vector<uint8_t> key;
                    std::string algorithm;
                    if (parse_encryption_key(key_spec, keyid, key, algorithm)) {
                        // Store the key in appropriate map based on algorithm
                        if (algorithm == "DES-OFB") {
                            des_keys[keyid] = key;
                        } else if (algorithm == "AES-256") {
                            aes_keys[keyid] = key;
                        } else if (algorithm == "ADP/RC4") {
                            adp_keys[keyid] = key;
                        } else {
                            std::cerr << "Error: Unsupported key length for algorithm detection. Supported: 5 bytes (ADP/RC4), 8 bytes (DES-OFB), 32 bytes (AES-256)" << std::endl;
                            return 1;
                        }
                        std::cout << "Added " << algorithm << " key ID 0x" << std::hex << keyid << std::dec << " (" << key.size() << " bytes)" << std::endl;
                    } else {
                        return 1; // parse_encryption_key already printed error
                    }
                } else {
                    std::cerr << "Error: -k requires a key specification\n";
                    return 1;
                }
            } else if (arg == "--mp3") {
                audio_format = "mp3";
                enable_wav = true;
            } else if (arg == "--m4a") {
                audio_format = "m4a";
                enable_wav = true;
            } else if (arg == "--opus") {
                audio_format = "opus";
                enable_wav = true;
            } else if (arg == "--webm") {
                audio_format = "webm";
                enable_wav = true;
            } else if (arg == "--transcript") {
                std::cerr << "Error: Voice transcription is not yet implemented\n";
                return 1;
            } else if (!arg.empty() && arg[0] != '-') {
                // This should be the input file or directory
                input_path = arg;
            } else {
                std::cerr << "Error: Unknown option: " << arg << "\n";
                return 1;
            }
        }
        
        // Initialize config
        DecoderConfig config;
        
        // Handle config file mode
        if (use_config_file) {
            if (show_help) {
                print_usage(argv[0]);
                return 0;
            }
            
            if (!parse_config_file(config_file, config)) {
                return 1;
            }
            
            // Override config with command-line arguments if provided
            if (!input_path.empty()) config.input_path = input_path;
            if (output_dir != ".") config.output_dir = output_dir;
            if (verbose) config.verbose = true;
            if (quiet) config.quiet = true;
            if (recursive) config.recursive = true;
            if (foreground) config.foreground = true;
            if (enable_json) config.enable_json = true;
            if (enable_wav) config.enable_wav = true;
            if (enable_text) config.enable_text = true;
            
            // Use config values
            input_path = config.input_path;
            output_dir = config.output_dir;
            verbose = config.verbose;
            quiet = config.quiet;
            recursive = config.recursive;
            enable_json = config.enable_json;
            enable_wav = config.enable_wav;
            enable_text = config.enable_text;
            if (audio_format == "wav") { // Only override if not set by command line
                audio_format = config.audio_format;
            }
            if (audio_bitrate == 0) { // Only override if not set by command line
                audio_bitrate = config.audio_bitrate;
            }
            
            if (!quiet) {
                std::cout << "Using config file: " << config_file << std::endl;
                if (config.service_mode == "api") {
                    std::cout << "Service mode: API (endpoint: " << config.api_endpoint << ")" << std::endl;
                }
            }
        }
        
        // Show help or check for required arguments
        if (show_help || (input_path.empty() && config.service_mode != "api")) {
            print_usage(argv[0]);
            return show_help ? 0 : 1;
        }
        
        // Require at least one output format (unless using config file with service mode)
        if (config.service_mode != "api" && !enable_json && !enable_wav && !enable_text && !enable_csv) {
            std::cerr << "Error: Must specify at least one output format (--json, --wav, --text, or --csv)\n";
            std::cerr << "Use -h for help or -c for config file mode\n";
            return 1;
        }

        // Create output directory if it doesn't exist
        if (!fs::exists(output_dir)) {
            try {
                fs::create_directories(output_dir);
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Error: Failed to create output directory " << output_dir << ": " << e.what() << std::endl;
                return 1;
            }
        }

        if (!quiet && verbose) {
            std::cout << "trunk-decoder v1.0\n";
            if (config.service_mode != "api") {
                std::cout << "Input: " << input_path << "\n";
            }
            std::cout << "Output directory: " << output_dir << "\n";
            std::cout << std::endl;
        }

        // Handle service mode (API mode) - skip file processing
        if (config.service_mode == "api") {
            
            // Use configured port
            int port = config.api_port;
            
            ApiService api_service(port, output_dir, verbose, config.foreground);
            
            // Configure authentication
            if (!config.auth_token.empty()) {
                api_service.set_auth_token(config.auth_token);
                if (!quiet) {
                    std::cout << "API authentication enabled" << std::endl;
                }
            }
            
            // Configure HTTPS/SSL
            if (!config.ssl_cert.empty() && !config.ssl_key.empty()) {
                api_service.enable_https(config.ssl_cert, config.ssl_key);
                if (!quiet) {
                    std::cout << "HTTPS/SSL enabled with cert: " << config.ssl_cert << std::endl;
                }
            }
            
            // Configure upload script
            if (!config.upload_script.empty()) {
                api_service.set_upload_script(config.upload_script);
                if (!quiet) {
                    std::cout << "Upload script configured: " << config.upload_script << std::endl;
                }
            }
            
            // Configure audio format
            api_service.set_audio_format(config.audio_format);
            api_service.set_audio_bitrate(config.audio_bitrate);
            if (!quiet && config.audio_format != "wav") {
                std::cout << "Audio format: " << config.audio_format;
                if (config.audio_bitrate > 0) {
                    std::cout << " @ " << config.audio_bitrate << "k";
                }
                std::cout << std::endl;
            }
            
            // Configure decryption keys
            if (!des_keys.empty() || !aes_keys.empty() || !adp_keys.empty()) {
                api_service.enable_decryption(true);
                
                for (const auto& key_pair : des_keys) {
                    api_service.add_des_key(key_pair.first, key_pair.second);
                }
                for (const auto& key_pair : aes_keys) {
                    api_service.add_aes_key(key_pair.first, key_pair.second);
                }
                for (const auto& key_pair : adp_keys) {
                    api_service.add_adp_key(key_pair.first, key_pair.second);
                }
                
                if (!quiet) {
                    int total_keys = des_keys.size() + aes_keys.size() + adp_keys.size();
                    std::cout << "API service configured with " << total_keys << " encryption key(s)" << std::endl;
                }
            }
            
            if (!quiet) {
                std::cout << "Starting trunk-decoder API service on port " << port << std::endl;
                std::cout << "Output directory: " << output_dir << std::endl;
                if (config.audio_format != "wav") {
                    std::cout << "Audio codec disclaimer: Codec usage subject to patent/licensing requirements" << std::endl;
                }
                std::cout << "Press Ctrl+C to stop the service" << std::endl;
            }
            
            if (!api_service.start()) {
                std::cerr << "Failed to start API service on port " << port << std::endl;
                return 1;
            }
            
            return 0;
        }

        std::vector<std::string> files_to_process;
        
        // Check if input is a file or directory
        if (fs::is_regular_file(input_path)) {
            // Single file
            if (fs::path(input_path).extension() != ".p25") {
                std::cerr << "Error: Input file must have .p25 extension" << std::endl;
                return 1;
            }
            files_to_process.push_back(input_path);
        } else if (fs::is_directory(input_path)) {
            // Directory - find all .p25 files
            files_to_process = find_p25_files(input_path, recursive);
            if (files_to_process.empty()) {
                std::cout << "No .p25 files found in " << input_path << std::endl;
                return 0;
            }
        } else {
            std::cerr << "Error: Input path does not exist or is not accessible: " << input_path << std::endl;
            return 1;
        }

        if (!quiet) {
            std::cout << "Found " << files_to_process.size() << " P25 file(s) to process" << std::endl;
        }

        // Create and configure decoder
        P25Decoder decoder;
        
        // Add encryption keys to decoder if any were provided
        if (!des_keys.empty() || !aes_keys.empty() || !adp_keys.empty()) {
            decoder.enable_decryption(true);
            
            // Add DES keys
            for (const auto& key_pair : des_keys) {
                decoder.add_des_key(key_pair.first, key_pair.second);
            }
            
            // Add AES keys
            for (const auto& key_pair : aes_keys) {
                decoder.add_aes_key(key_pair.first, key_pair.second);
            }
            
            // Add ADP keys
            for (const auto& key_pair : adp_keys) {
                decoder.add_adp_key(key_pair.first, key_pair.second);
            }
            
            if (!quiet) {
                int total_keys = des_keys.size() + aes_keys.size() + adp_keys.size();
                std::cout << "Enabled decryption with " << total_keys << " key(s): ";
                if (!des_keys.empty()) std::cout << des_keys.size() << " DES ";
                if (!aes_keys.empty()) std::cout << aes_keys.size() << " AES ";
                if (!adp_keys.empty()) std::cout << adp_keys.size() << " ADP ";
                std::cout << std::endl;
            }
        }
        
        // Process all files (file mode)
        int successful = 0;
        int failed = 0;
        auto start_time = std::chrono::steady_clock::now();

        for (const auto& file : files_to_process) {
            if (process_single_file(file, output_dir, verbose, quiet, enable_json, enable_wav, enable_text, enable_csv, audio_format, audio_bitrate, decoder)) {
                successful++;
            } else {
                failed++;
            }
            
            // Progress display is handled by individual "Processing:" messages
            // No need for overwriting progress line that causes output mixing
        }

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        if (!quiet) {
            std::cout << "\nProcessing complete!" << std::endl;
            std::cout << "Successful: " << successful << std::endl;
            if (failed > 0) {
                std::cout << "Failed: " << failed << std::endl;
            }
            std::cout << "Total time: " << duration.count() << "ms" << std::endl;
        }

        return (failed > 0) ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}