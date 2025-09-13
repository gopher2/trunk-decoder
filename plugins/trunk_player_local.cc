/*
 * Trunk Player Local Plugin
 * Uploads processed calls to local trunk-player database via Django management command
 */

#include "../src/plugin_api.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <cstdio>
#include <memory>
#include <stdexcept>

class Trunk_Player_Local : public Base_Plugin {
private:
    std::string trunk_player_path_;
    std::string venv_python_path_;
    std::string manage_py_path_;
    int system_id_;
    int source_id_;
    bool keep_files_;
    bool verbose_;
    
    // Statistics
    int calls_processed_;
    int calls_successful_;
    int calls_failed_;
    
public:
    PLUGIN_INFO("Trunk Player Local", "1.0.0", "Dave K9DPD", "Uploads processed calls to local trunk-player database via Django commands")
    
    Trunk_Player_Local() : system_id_(0), source_id_(0), keep_files_(false), verbose_(false),
                           calls_processed_(0), calls_successful_(0), calls_failed_(0) {}
    
    virtual ~Trunk_Player_Local() = default;
    
    virtual int init(json config_data) override {
        if (parse_config(config_data) != 0) {
            set_state(Plugin_State::PLUGIN_ERROR);
            return -1;
        }
        
        if (!validate_paths()) {
            set_state(Plugin_State::PLUGIN_ERROR);
            return -1;
        }
        
        set_state(Plugin_State::PLUGIN_INITIALIZED);
        if (verbose_) {
            std::cout << "[TrunkPlayerUploader] Plugin initialized successfully" << std::endl;
        }
        return 0;
    }
    
    virtual int start() override {
        if (state_ != Plugin_State::PLUGIN_INITIALIZED) {
            return -1;
        }
        
        set_state(Plugin_State::PLUGIN_RUNNING);
        if (verbose_) {
            std::cout << "[TrunkPlayerUploader] Plugin started" << std::endl;
        }
        return 0;
    }
    
    virtual int stop() override {
        set_state(Plugin_State::PLUGIN_STOPPED);
        if (verbose_) {
            std::cout << "[TrunkPlayerUploader] Plugin stopped. Stats: " 
                      << calls_processed_ << " processed, " << calls_successful_ << " successful, " 
                      << calls_failed_ << " failed" << std::endl;
        }
        return 0;
    }
    
    virtual int parse_config(json config_data) override {
        config_ = config_data;
        
        trunk_player_path_ = config_.value("trunk_player_path", "/Users/dave/Documents/GitHub/trunk-player");
        system_id_ = config_.value("system_id", 0);
        source_id_ = config_.value("source_id", 0);
        keep_files_ = config_.value("keep_files", false);
        verbose_ = config_.value("verbose", false);
        enabled_ = config_.value("enabled", true);
        
        venv_python_path_ = trunk_player_path_ + "/venv/bin/python";
        manage_py_path_ = trunk_player_path_ + "/manage.py";
        
        if (verbose_) {
            std::cout << "[TrunkPlayerUploader] Config parsed:" << std::endl;
            std::cout << "  Trunk Player Path: " << trunk_player_path_ << std::endl;
            std::cout << "  System ID: " << system_id_ << std::endl;
            std::cout << "  Source ID: " << source_id_ << std::endl;
            std::cout << "  Keep Files: " << (keep_files_ ? "YES" : "NO") << std::endl;
        }
        
        return 0;
    }
    
    virtual int call_end(Call_Data_t call_info) override {
        // We don't need to do anything when a call ends
        return 0;
    }
    
    virtual int call_data_ready(Call_Data_t call_info) override {
        if (state_ != Plugin_State::PLUGIN_RUNNING || !enabled_) {
            return 0;
        }
        
        calls_processed_++;
        
        if (verbose_) {
            std::cout << "[TrunkPlayerUploader] Processing call: " << call_info.wav_filename << std::endl;
        }
        
        std::string audio_file = call_info.wav_filename;
        std::string json_file = call_info.json_filename;
        
        if (!std::filesystem::exists(audio_file)) {
            if (verbose_) {
                std::cout << "[TrunkPlayerUploader] Error: Audio file not found: " << audio_file << std::endl;
            }
            calls_failed_++;
            return -1;
        }
        
        if (!std::filesystem::exists(json_file)) {
            if (verbose_) {
                std::cout << "[TrunkPlayerUploader] Error: JSON file not found: " << json_file << std::endl;
            }
            calls_failed_++;
            return -1;
        }
        
        // Get audio duration
        double duration = get_audio_duration(audio_file);
        if (duration <= 0) {
            if (verbose_) {
                std::cout << "[TrunkPlayerUploader] Error: Could not determine audio duration" << std::endl;
            }
            calls_failed_++;
            return -1;
        }
        
        // Update JSON metadata
        if (!update_json_metadata(json_file, duration, source_id_)) {
            if (verbose_) {
                std::cout << "[TrunkPlayerUploader] Error: Failed to update JSON metadata" << std::endl;
            }
            calls_failed_++;
            return -1;
        }
        
        // Calculate web directory
        std::string web_dir = calculate_web_dir(audio_file);
        
        // Add to trunk-player
        if (!add_transmission_to_db(audio_file, web_dir)) {
            if (verbose_) {
                std::cout << "[TrunkPlayerUploader] Error: Failed to add transmission to trunk-player" << std::endl;
            }
            calls_failed_++;
            return -1;
        }
        
        // Clean up files if requested
        if (!keep_files_) {
            cleanup_files(audio_file, json_file);
        }
        
        calls_successful_++;
        
        if (verbose_) {
            std::cout << "[TrunkPlayerUploader] Successfully processed call: " 
                      << std::filesystem::path(audio_file).stem().string() << std::endl;
        }
        
        return 0;
    }
    
    virtual json get_stats() override {
        json stats = Base_Plugin::get_stats();
        stats["calls_processed"] = calls_processed_;
        stats["calls_successful"] = calls_successful_;
        stats["calls_failed"] = calls_failed_;
        stats["success_rate"] = calls_processed_ > 0 ? 
            static_cast<double>(calls_successful_) / calls_processed_ * 100.0 : 0.0;
        stats["trunk_player_path"] = trunk_player_path_;
        stats["system_id"] = system_id_;
        stats["configured"] = validate_paths();
        return stats;
    }
    
private:
    bool validate_paths() {
        return std::filesystem::exists(venv_python_path_) && std::filesystem::exists(manage_py_path_);
    }
    
    double get_audio_duration(const std::string& audio_file) {
        std::string command = "soxi -D \"" + audio_file + "\"";
        
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
        if (!pipe) {
            return 0.0;
        }
        
        char buffer[128];
        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
            result += buffer;
        }
        
        try {
            return std::stod(result);
        } catch (const std::exception& e) {
            return 0.0;
        }
    }
    
    bool update_json_metadata(const std::string& json_file, double duration, int source_id) {
        try {
            // Read existing JSON
            std::ifstream file(json_file);
            if (!file.is_open()) {
                return false;
            }
            
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();
            
            std::string json_content = buffer.str();
            
            // Simple JSON manipulation - find last brace and add our fields
            size_t last_brace = json_content.rfind('}');
            if (last_brace == std::string::npos) {
                return false;
            }
            
            // Check if we need to add a comma
            std::string before_brace = json_content.substr(0, last_brace);
            bool needs_comma = false;
            for (int i = before_brace.length() - 1; i >= 0; i--) {
                char c = before_brace[i];
                if (c == ' ' || c == '\n' || c == '\t' || c == '\r') continue;
                if (c == '"' || c == '}' || c == ']') {
                    needs_comma = true;
                }
                break;
            }
            
            // Build new JSON content
            std::ostringstream new_json;
            new_json << before_brace;
            if (needs_comma) new_json << ",";
            new_json << "\n  \"play_length\": " << duration;
            new_json << ",\n  \"source\": " << source_id;
            new_json << "\n}";
            
            // Write back to file
            std::ofstream outfile(json_file);
            if (!outfile.is_open()) {
                return false;
            }
            
            outfile << new_json.str();
            outfile.close();
            
            return true;
            
        } catch (const std::exception& e) {
            return false;
        }
    }
    
    std::string calculate_web_dir(const std::string& audio_file) {
        std::filesystem::path file_path(audio_file);
        std::filesystem::path parent = file_path.parent_path();
        
        // Extract last few directory components for web path
        std::vector<std::string> components;
        auto current = parent;
        int count = 0;
        const int max_components = 6;
        
        while (current != current.root_path() && count < max_components) {
            components.insert(components.begin(), current.filename().string());
            current = current.parent_path();
            count++;
        }
        
        std::string web_dir;
        for (const auto& component : components) {
            web_dir += component + "/";
        }
        
        return web_dir;
    }
    
    bool add_transmission_to_db(const std::string& audio_file, const std::string& web_dir) {
        std::filesystem::path file_path(audio_file);
        std::string basename = file_path.stem().string();
        
        std::ostringstream command;
        command << "cd \"" << trunk_player_path_ << "\" && "
                << "\"" << venv_python_path_ << "\" \"" << manage_py_path_ << "\" "
                << "add_transmission \"" << basename << "\" "
                << "--web_url=\"" << web_dir << "\" "
                << "--system=" << system_id_;
        
        int result = std::system(command.str().c_str());
        return result == 0;
    }
    
    void cleanup_files(const std::string& audio_file, const std::string& json_file) {
        try {
            if (std::filesystem::exists(audio_file)) {
                std::filesystem::remove(audio_file);
            }
            
            if (std::filesystem::exists(json_file)) {
                std::filesystem::remove(json_file);
            }
        } catch (const std::exception& e) {
            // Ignore cleanup errors
        }
    }
};

TRUNK_DECODER_PLUGIN_FACTORY(Trunk_Player_Local)