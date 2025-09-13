/*
 * Multi-Format Output Plugin for trunk-decoder
 * Generates multiple audio formats per stream configuration
 */

#pragma once

#include "../../src/plugin_api.h"
#include "../../src/p25_decoder.h"
#include <map>
#include <vector>
#include <string>

class Multi_Format_Output : public Base_Plugin {
private:
    struct Format_Config {
        bool enabled;
        int bitrate;
        std::string output_dir;
        std::string filename_template; // e.g. "{system}_{talkgroup}_{timestamp}"
        bool keep_wav;
    };
    
    struct Stream_Config {
        std::string name;
        std::string system_name;
        std::map<std::string, Format_Config> formats; // format -> config
        std::string upload_script;
        bool async_processing;
    };
    
    std::map<std::string, Stream_Config> stream_configs_; // stream_name -> config
    std::map<std::string, P25Decoder> decoders_; // thread-local decoders
    
    // Statistics
    std::atomic<int> files_generated_;
    std::atomic<int> conversions_failed_;
    std::atomic<int> uploads_completed_;
    
    // Helper methods
    std::string generate_filename(const Call_Data_t& call_info, 
                                const std::string& format,
                                const std::string& template_str);
    bool convert_audio(const std::string& wav_file, 
                      const std::string& output_file,
                      const std::string& format,
                      int bitrate);
    bool execute_upload_script(const Call_Data_t& call_info,
                             const std::map<std::string, std::string>& generated_files);
    Stream_Config* get_stream_config(const std::string& stream_name);
    
public:
    Multi_Format_Output();
    virtual ~Multi_Format_Output();
    
    // Plugin API implementation
    virtual int init(json config_data) override;
    virtual int start() override;
    virtual int stop() override;
    virtual int parse_config(json config_data) override;
    
    virtual int call_end(Call_Data_t call_info) override;
    virtual int call_data_ready(Call_Data_t call_info) override;
    
    virtual json get_stats() override;
    
    // Plugin metadata
    PLUGIN_INFO("Multi-Format Output", "1.0.0", "trunk-decoder", 
                "Generates multiple audio formats per stream configuration")
};

TRUNK_DECODER_PLUGIN_FACTORY(Multi_Format_Output)