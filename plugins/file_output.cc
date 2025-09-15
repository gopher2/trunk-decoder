/*
 * Generic File Output Plugin
 * Organizes and copies audio files to specified directories with customizable folder structures
 */

#include "../src/plugin_api.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <iomanip>

class File_Output : public Base_Plugin {
private:
    std::string output_base_dir_;
    std::string folder_structure_;
    bool copy_wav_;
    bool copy_mp3_;
    bool copy_m4a_;
    bool copy_p25_;
    std::string p25_output_mode_;
    bool copy_json_;
    bool create_symlinks_;
    bool verbose_;
    
    // Statistics
    int files_processed_;
    int files_successful_;
    int files_failed_;
    
public:
    PLUGIN_INFO("Generic File Output", "1.0.0", "Dave K9DPD", "Organizes and copies audio files with customizable folder structures")
    
    File_Output() : copy_wav_(true), copy_mp3_(true), copy_m4a_(true), copy_p25_(true), copy_json_(true), 
                    create_symlinks_(false), verbose_(false), files_processed_(0), files_successful_(0), files_failed_(0) {}
    
    virtual ~File_Output() = default;
    
    virtual int init(json config_data) override {
        if (parse_config(config_data) != 0) {
            set_state(Plugin_State::PLUGIN_ERROR);
            return -1;
        }
        
        // Validate output directory
        if (!validate_output_dir()) {
            set_state(Plugin_State::PLUGIN_ERROR);
            return -1;
        }
        
        set_state(Plugin_State::PLUGIN_INITIALIZED);
        if (verbose_) {
            std::cout << "[FileOutput] Plugin initialized successfully" << std::endl;
        }
        return 0;
    }
    
    virtual int start() override {
        if (state_ != Plugin_State::PLUGIN_INITIALIZED) {
            return -1;
        }
        
        set_state(Plugin_State::PLUGIN_RUNNING);
        if (verbose_) {
            std::cout << "[FileOutput] Plugin started" << std::endl;
        }
        return 0;
    }
    
    virtual int stop() override {
        set_state(Plugin_State::PLUGIN_STOPPED);
        if (verbose_) {
            std::cout << "[FileOutput] Plugin stopped. Stats: " 
                      << files_processed_ << " processed, " << files_successful_ << " successful, " 
                      << files_failed_ << " failed" << std::endl;
        }
        return 0;
    }
    
    virtual int parse_config(json config_data) override {
        config_ = config_data;
        
        output_base_dir_ = config_.value("output_base_dir", "./output");
        folder_structure_ = config_.value("folder_structure", "{system}/{date}/{hour}");
        copy_wav_ = config_.value("copy_wav", true);
        copy_mp3_ = config_.value("copy_mp3", true);
        copy_m4a_ = config_.value("copy_m4a", true);
        copy_p25_ = config_.value("copy_p25", true);
        p25_output_mode_ = config_.value("p25_output_mode", "voice");
        copy_json_ = config_.value("copy_json", true);
        create_symlinks_ = config_.value("create_symlinks", false);
        verbose_ = config_.value("verbose", false);
        enabled_ = config_.value("enabled", true);
        
        if (verbose_) {
            std::cout << "[FileOutput] Config parsed:" << std::endl;
            std::cout << "  Output Base Dir: " << output_base_dir_ << std::endl;
            std::cout << "  Folder Structure: " << folder_structure_ << std::endl;
            std::cout << "  Copy WAV: " << (copy_wav_ ? "YES" : "NO") << std::endl;
            std::cout << "  Copy MP3: " << (copy_mp3_ ? "YES" : "NO") << std::endl;
            std::cout << "  Copy M4A: " << (copy_m4a_ ? "YES" : "NO") << std::endl;
            std::cout << "  Copy P25: " << (copy_p25_ ? "YES" : "NO") << std::endl;
            std::cout << "  P25 Output Mode: " << p25_output_mode_ << std::endl;
            std::cout << "  Copy JSON: " << (copy_json_ ? "YES" : "NO") << std::endl;
            std::cout << "  Create Symlinks: " << (create_symlinks_ ? "YES" : "NO") << std::endl;
        }
        
        return 0;
    }
    
    virtual int call_end(Call_Data_t call_info) override {
        return 0;
    }
    
    virtual int call_data_ready(Call_Data_t call_info) override {
        if (state_ != Plugin_State::PLUGIN_RUNNING || !enabled_) {
            return 0;
        }
        
        files_processed_++;
        
        if (verbose_) {
            std::cout << "[FileOutput] Processing call: " << call_info.wav_filename << std::endl;
        }
        
        std::string audio_file = call_info.wav_filename;
        std::string json_file = call_info.json_filename;
        
        if (!std::filesystem::exists(audio_file)) {
            if (verbose_) {
                std::cout << "[FileOutput] Error: Audio file not found: " << audio_file << std::endl;
            }
            files_failed_++;
            return -1;
        }
        
        // Generate output directory structure
        std::string output_dir = generate_output_path(call_info);
        
        try {
            // Create output directory
            std::filesystem::create_directories(output_dir);
            
            if (verbose_) {
                std::cout << "[FileOutput] Output directory: " << output_dir << std::endl;
            }
            
            // Copy or link audio files based on format
            std::filesystem::path base_src_path(audio_file);
            std::string base_name = base_src_path.stem().string();
            std::string base_dir = base_src_path.parent_path().string();
            
            // Handle WAV files
            if (copy_wav_) {
                copy_file_if_exists(audio_file, output_dir, "WAV");
            }
            
            // Handle MP3 files
            if (copy_mp3_) {
                std::string mp3_file = base_dir + "/" + base_name + ".mp3";
                copy_file_if_exists(mp3_file, output_dir, "MP3");
            }
            
            // Handle M4A files  
            if (copy_m4a_) {
                std::string m4a_file = base_dir + "/" + base_name + ".m4a";
                copy_file_if_exists(m4a_file, output_dir, "M4A");
            }
            
            // Handle P25 files
            if (copy_p25_) {
                std::string p25_file = base_dir + "/" + base_name + ".p25";
                
                // Check P25 output mode
                bool should_copy_p25 = true;
                if (p25_output_mode_ == "voice") {
                    // Only copy P25 files for voice calls (check audio_type in JSON)
                    if (call_info.call_json.contains("audio_type")) {
                        std::string audio_type = call_info.call_json["audio_type"];
                        should_copy_p25 = (audio_type == "digital" || audio_type == "analog");
                    }
                }
                // p25_output_mode_ == "always" copies all P25 files
                
                if (should_copy_p25) {
                    copy_file_if_exists(p25_file, output_dir, "P25");
                }
            }
            
            // Also check converted_files map for additional formats
            for (const auto& [format, filepath] : call_info.converted_files) {
                if ((format == "mp3" && copy_mp3_) || 
                    (format == "m4a" && copy_m4a_) ||
                    (format == "wav" && copy_wav_) ||
                    (format == "p25" && copy_p25_)) {
                    copy_file_if_exists(filepath, output_dir, format);
                }
            }
            
            // Copy or link JSON file
            if (copy_json_ && std::filesystem::exists(json_file)) {
                std::filesystem::path src_path(json_file);
                std::filesystem::path dest_path = std::filesystem::path(output_dir) / src_path.filename();
                
                if (create_symlinks_) {
                    std::filesystem::create_symlink(src_path.string(), dest_path.string());
                    if (verbose_) {
                        std::cout << "[FileOutput] Created symlink: " << dest_path << std::endl;
                    }
                } else {
                    std::filesystem::copy_file(src_path, dest_path, std::filesystem::copy_options::overwrite_existing);
                    if (verbose_) {
                        std::cout << "[FileOutput] Copied JSON: " << dest_path << std::endl;
                    }
                }
            }
            
            files_successful_++;
            
            if (verbose_) {
                std::cout << "[FileOutput] Successfully processed call" << std::endl;
            }
            
        } catch (const std::exception& e) {
            if (verbose_) {
                std::cout << "[FileOutput] Error processing call: " << e.what() << std::endl;
            }
            files_failed_++;
            return -1;
        }
        
        return 0;
    }
    
    virtual json get_stats() override {
        json stats = Base_Plugin::get_stats();
        stats["files_processed"] = files_processed_;
        stats["files_successful"] = files_successful_;
        stats["files_failed"] = files_failed_;
        stats["success_rate"] = files_processed_ > 0 ? 
            static_cast<double>(files_successful_) / files_processed_ * 100.0 : 0.0;
        stats["output_base_dir"] = output_base_dir_;
        stats["folder_structure"] = folder_structure_;
        return stats;
    }
    
private:
    bool validate_output_dir() {
        try {
            std::filesystem::create_directories(output_base_dir_);
            return std::filesystem::exists(output_base_dir_);
        } catch (const std::exception& e) {
            if (verbose_) {
                std::cout << "[FileOutput] Error creating output directory: " << e.what() << std::endl;
            }
            return false;
        }
    }
    
    std::string generate_output_path(const Call_Data_t& call_info) {
        std::string path = folder_structure_;
        
        // Get current time
        std::time_t now = std::time(nullptr);
        std::tm* local_time = std::localtime(&now);
        
        // Replace tokens
        replace_token(path, "{system}", call_info.system_short_name.empty() ? "unknown" : call_info.system_short_name);
        replace_token(path, "{talkgroup}", std::to_string(call_info.talkgroup));
        replace_token(path, "{source}", std::to_string(call_info.source_id));
        
        // Date/time tokens
        replace_token(path, "{year}", std::to_string(1900 + local_time->tm_year));
        replace_token(path, "{month}", format_zero_pad(local_time->tm_mon + 1, 2));
        replace_token(path, "{day}", format_zero_pad(local_time->tm_mday, 2));
        replace_token(path, "{hour}", format_zero_pad(local_time->tm_hour, 2));
        replace_token(path, "{minute}", format_zero_pad(local_time->tm_min, 2));
        replace_token(path, "{date}", std::to_string(1900 + local_time->tm_year) + "-" + 
                                    format_zero_pad(local_time->tm_mon + 1, 2) + "-" + 
                                    format_zero_pad(local_time->tm_mday, 2));
        
        // Site info
        replace_token(path, "{site_id}", std::to_string(call_info.site_id));
        replace_token(path, "{site_name}", call_info.site_name.empty() ? "unknown" : call_info.site_name);
        
        return std::filesystem::path(output_base_dir_) / path;
    }
    
    void copy_file_if_exists(const std::string& source_file, const std::string& output_dir, const std::string& format_name) {
        if (!std::filesystem::exists(source_file)) {
            return;
        }
        
        try {
            std::filesystem::path src_path(source_file);
            std::filesystem::path dest_path = std::filesystem::path(output_dir) / src_path.filename();
            
            if (create_symlinks_) {
                std::filesystem::create_symlink(src_path.string(), dest_path.string());
                if (verbose_) {
                    std::cout << "[FileOutput] Created " << format_name << " symlink: " << dest_path << std::endl;
                }
            } else {
                std::filesystem::copy_file(src_path, dest_path, std::filesystem::copy_options::overwrite_existing);
                if (verbose_) {
                    std::cout << "[FileOutput] Copied " << format_name << " file: " << dest_path << std::endl;
                }
            }
        } catch (const std::exception& e) {
            if (verbose_) {
                std::cout << "[FileOutput] Error copying " << format_name << " file: " << e.what() << std::endl;
            }
        }
    }
    
    void replace_token(std::string& path, const std::string& token, const std::string& value) {
        size_t pos = 0;
        while ((pos = path.find(token, pos)) != std::string::npos) {
            path.replace(pos, token.length(), value);
            pos += value.length();
        }
    }
    
    std::string format_zero_pad(int value, int width) {
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(width) << value;
        return oss.str();
    }
    
};

// Plugin factory function
extern "C" boost::shared_ptr<Plugin_Api> create_plugin() {
    return boost::shared_ptr<Plugin_Api>(new File_Output());
}