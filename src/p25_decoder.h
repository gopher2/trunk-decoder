/*
 * P25 Decoder Header
 * 
 * Copyright (C) 2024 David Kierzkowski (K9DPD)
 * 
 * This implementation incorporates substantial code and techniques from:
 * - trunk-recorder project: IMBE vocoder integration and P25 frame processing
 * - OP25 project: P25 protocol implementation
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef P25_DECODER_H
#define P25_DECODER_H

#include "p25_frame_parser.h"
#include "p25_des_decrypt.h"
#include "p25_aes_decrypt.h"
#include "p25_adp_decrypt.h"
#include <string>
#include <vector>
#include <memory>
#include <fstream>

// Forward declarations for OP25/IMBE decoder components (trunk-recorder integration)
// We'll need to copy or link the relevant source files
namespace gr {
    namespace op25_repeater {
        class software_imbe_decoder;
    }
}

struct CallMetadata {
    // Basic call info
    long talkgroup;
    long source_id;
    uint16_t nac;
    std::string system_short_name;
    time_t start_time;
    time_t end_time;
    double call_length;
    
    // Frame stats
    int total_frames;
    int voice_frames;
    bool has_encrypted_frames;
    
    // Audio info
    std::string audio_type;
    int freq;
    int freq_error;
    
    // Default values to match trunk-recorder format
    CallMetadata() : talkgroup(0), source_id(0), nac(0), start_time(0), end_time(0), call_length(0),
                    total_frames(0), voice_frames(0), has_encrypted_frames(false),
                    audio_type("digital"), freq(0), freq_error(0) {}
};

class P25Decoder {
private:
    std::unique_ptr<P25FrameParser> parser_;
    std::string input_filename_;
    std::string output_prefix_;
    
    // Audio output
    std::ofstream audio_file_;
    std::vector<int16_t> audio_buffer_;
    
    // JSON metadata output
    CallMetadata metadata_;
    
    // IMBE decoder components
    bool imbe_decoder_initialized_;
    
    // OP25 IMBE vocoder (trunk-recorder approach)  
    class imbe_vocoder* vocoder_;
    
    // P25 frame processing
    int current_frame_num_;
    
    // IMBE parameter extraction from P25 frames
    bool extract_imbe_from_p25_frame(const P25Frame& frame, std::vector<int16_t>& audio_samples);
    
    // Output control
    bool text_dump_enabled_;
    
    // Decryption support
    std::unique_ptr<P25DESDecrypt> des_decrypt_;
    std::unique_ptr<P25AESDecrypt> aes_decrypt_;
    std::unique_ptr<P25ADPDecrypt> adp_decrypt_;
    bool decryption_enabled_;
    uint8_t current_mi_[9]; // Current Message Indicator
    
    // Internal methods
    bool setup_wav_output(const std::string& filename);
    void write_audio_samples(const std::vector<int16_t>& samples);
    void close_audio_output();
    void write_wav_header();
    void finalize_wav_file();
    
    bool decode_voice_frame(const P25Frame& frame, std::vector<int16_t>& audio_samples);
    void extract_voice_params(const P25Frame& frame);
    
    std::string generate_json_metadata();
    
public:
    P25Decoder();
    ~P25Decoder();
    
    // Main processing methods
    bool open_p25_file(const std::string& filename);
    bool decode_to_audio(const std::string& output_prefix);
    bool process_frames_only(); // Process P25 frames without audio output
    
    // Output methods  
    bool save_json_metadata(const std::string& filename);
    bool save_text_dump(const std::string& filename);
    bool save_csv_dump(const std::string& filename);
    
    // Configuration
    void set_output_sample_rate(int rate = 8000);
    void enable_text_dump(bool enable = true);
    
    // Decryption methods
    bool add_des_key(uint16_t keyid, const std::vector<uint8_t>& key);
    bool add_aes_key(uint16_t keyid, const std::vector<uint8_t>& key);
    bool add_adp_key(uint16_t keyid, const std::vector<uint8_t>& key);
    void enable_decryption(bool enable = true);
    
    // Get call information
    const CallMetadata& get_call_metadata() const { return metadata_; }
};

#endif // P25_DECODER_H