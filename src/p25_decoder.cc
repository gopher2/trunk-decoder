/*
 * P25 Decoder Implementation
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
 * 
 * AUDIO CODEC NOTICE: This software may use audio codecs that are subject
 * to patent licensing requirements. Users are responsible for ensuring
 * compliance with applicable patent licenses for MP3, AAC, and other codecs.
 */

#include "p25_decoder.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <cstdio>

// OP25 IMBE decoder includes (based on trunk-recorder implementation)
#include "imbe_vocoder/imbe_vocoder.h"
#include "op25_imbe_frame.h"

// Type definitions from OP25
typedef std::vector<bool> bit_vector;
typedef std::vector<bool> voice_codeword;

P25Decoder::P25Decoder() {
    parser_ = std::make_unique<P25FrameParser>();
    text_dump_enabled_ = false; // Default to false to reduce output
    imbe_decoder_initialized_ = false;
    current_frame_num_ = 0;
    decryption_enabled_ = false;
    audio_format_ = "wav";
    audio_bitrate_ = 0;
    
    // Initialize IMBE vocoder (trunk-recorder approach)
    vocoder_ = new imbe_vocoder();
    imbe_decoder_initialized_ = true;
    // Note: IMBE vocoder constructor prints its own copyright message
    
    // Initialize decryption support
    des_decrypt_ = std::make_unique<P25DESDecrypt>();
    aes_decrypt_ = std::make_unique<P25AESDecrypt>();
    adp_decrypt_ = std::make_unique<P25ADPDecrypt>();
    std::fill(current_mi_, current_mi_ + 9, 0);
}

P25Decoder::~P25Decoder() {
    close_audio_output();
    if (vocoder_) {
        delete vocoder_;
    }
}

bool P25Decoder::open_p25_file(const std::string& filename) {
    input_filename_ = filename;
    if (!parser_->open(filename)) {
        std::cerr << "Error: Failed to open P25 file: " << filename << std::endl;
        return false;
    }
    
    // Initialize metadata
    metadata_ = CallMetadata();
    metadata_.start_time = time(nullptr); // For now, use current time
    
    // Clear audio buffer for new file
    audio_buffer_.clear();
    
    return true;
}

bool P25Decoder::setup_wav_output(const std::string& filename) {
    // Create WAV file like trunk-recorder does
    audio_file_.open(filename + ".wav", std::ios::binary);
    if (!audio_file_.is_open()) {
        std::cerr << "Error: Could not create WAV output file: " << filename << ".wav" << std::endl;
        return false;
    }
    
    // Write placeholder WAV header (we'll update it when we close the file)
    write_wav_header();
    
    return true;
}

void P25Decoder::write_wav_header() {
    // WAV header for 16-bit, 8kHz, mono audio
    const int sample_rate = 8000;
    const int bits_per_sample = 16;
    const int channels = 1;
    const int data_size = 0; // We'll update this later
    
    // WAV header structure
    struct {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t file_size = 36 + data_size; // Will be updated later
        char wave[4] = {'W', 'A', 'V', 'E'};
        char fmt[4] = {'f', 'm', 't', ' '};
        uint32_t fmt_size = 16;
        uint16_t format = 1; // PCM
        uint16_t num_channels = channels;
        uint32_t sample_rate_val = sample_rate;
        uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
        uint16_t block_align = channels * bits_per_sample / 8;
        uint16_t bits_per_sample_val = bits_per_sample;
        char data[4] = {'d', 'a', 't', 'a'};
        uint32_t data_size_val = data_size; // Will be updated later
    } wav_header;
    
    audio_file_.write(reinterpret_cast<const char*>(&wav_header), sizeof(wav_header));
}

void P25Decoder::finalize_wav_file() {
    if (!audio_file_.is_open()) return;
    
    // Calculate actual data size
    std::streampos current_pos = audio_file_.tellp();
    uint32_t data_size = static_cast<uint32_t>(current_pos) - 44; // 44 is WAV header size
    uint32_t file_size = 36 + data_size;
    
    // Update WAV header with correct sizes
    audio_file_.seekp(4);
    audio_file_.write(reinterpret_cast<const char*>(&file_size), 4);
    
    audio_file_.seekp(40);
    audio_file_.write(reinterpret_cast<const char*>(&data_size), 4);
}

void P25Decoder::write_audio_samples(const std::vector<int16_t>& samples) {
    if (audio_file_.is_open() && !samples.empty()) {
        audio_file_.write(reinterpret_cast<const char*>(samples.data()), 
                         samples.size() * sizeof(int16_t));
    }
}

void P25Decoder::close_audio_output() {
    if (audio_file_.is_open()) {
        finalize_wav_file();
        audio_file_.close();
    }
}

bool P25Decoder::decode_voice_frame(const P25Frame& frame, std::vector<int16_t>& audio_samples) {
    audio_samples.clear();
    
    if (frame.is_voice_frame && imbe_decoder_initialized_) {
        // Try to decode actual IMBE voice data
        if (extract_imbe_from_p25_frame(frame, audio_samples)) {
            if (text_dump_enabled_) {
                std::cout << "  [VOICE] Decoded IMBE audio (" << frame.data.size() 
                          << " bytes P25 -> " << audio_samples.size() << " samples)" << std::endl;
            }
            return true;
        }
    }
    
    // Fall back to synthetic audio if IMBE decoding fails
    if (frame.is_voice_frame) {
        const int SAMPLES_PER_FRAME = 160;
        audio_samples.resize(SAMPLES_PER_FRAME, 0); // Silence as fallback
        
        if (text_dump_enabled_) {
            std::cout << "  [VOICE] IMBE decode failed, using silence (" << frame.data.size() 
                      << " bytes P25 -> " << SAMPLES_PER_FRAME << " samples)" << std::endl;
        }
        
        return true;
    }
    
    return false;
}

bool P25Decoder::extract_imbe_from_p25_frame(const P25Frame& frame, std::vector<int16_t>& audio_samples) {
    if (!imbe_decoder_initialized_ || !vocoder_) {
        return false;
    }
    
    const int SAMPLES_PER_FRAME = 160;
    audio_samples.clear();
    
    try {
        // Convert frame data to bit vector for processing (same as trunk-recorder)
        bit_vector frame_bits(frame.data.size() * 8);
        for (size_t i = 0; i < frame.data.size(); i++) {
            for (int bit = 0; bit < 8; bit++) {
                frame_bits[i * 8 + bit] = (frame.data[i] >> (7 - bit)) & 1;
            }
        }
        
        // P25 voice frames contain 9 IMBE codewords - decode each one (trunk-recorder approach)
        for (int i = 0; i < 9; i++) {
            voice_codeword codeword(144); // IMBE codeword is 144 bits
            
            // Use proper OP25 deinterleaving (same as trunk-recorder)
            imbe_deinterleave(frame_bits, codeword, i);
            
            // Decode IMBE parameters (same as trunk-recorder)
            uint32_t u[8];
            uint32_t E0, ET;
            size_t errs = imbe_header_decode(codeword, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], E0, ET);
            
            // Convert to frame vector format for vocoder (same as trunk-recorder)
            int16_t frame_vector[8];
            for (int j = 0; j < 8; j++) {
                frame_vector[j] = u[j];
            }
            frame_vector[7] >>= 1; // Same bit shift as trunk-recorder
            
            // Decode with IMBE vocoder (same as trunk-recorder)
            int16_t snd[160]; // 160 samples per IMBE frame
            vocoder_->imbe_decode(frame_vector, snd);
            
            // Add decoded samples to output
            for (int j = 0; j < 160; j++) {
                audio_samples.push_back(snd[j]);
            }
        }
        
        if (!audio_samples.empty()) {
            return true; // Successfully decoded audio
        }
        
    } catch (const std::exception& e) {
        if (text_dump_enabled_) {
            std::cout << "  [ERROR] IMBE decode exception: " << e.what() << std::endl;
        }
    }
    
    return false; // Failed to decode
}

std::string P25Decoder::generate_json_metadata() {
    // Check if there's a corresponding .json file - use just filename, not full path
    std::string basename = input_filename_;
    
    // Remove path, keeping only filename
    size_t last_slash = basename.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        basename = basename.substr(last_slash + 1);
    }
    
    // Create JSON filename from basename
    std::string json_filename = basename;
    if (json_filename.size() > 4 && json_filename.substr(json_filename.size() - 4) == ".p25") {
        json_filename = json_filename.substr(0, json_filename.size() - 4) + ".json";
    }
    
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);
    
    // Try to read existing JSON metadata from trunk-recorder
    std::ifstream existing_json(json_filename);
    bool has_existing_metadata = false;
    std::string existing_content;
    
    if (existing_json.is_open()) {
        std::string line;
        while (std::getline(existing_json, line)) {
            existing_content += line + "\n";
        }
        existing_json.close();
        has_existing_metadata = true;
    }
    
    if (has_existing_metadata) {
        // Parse and enhance existing metadata
        // Remove the closing brace and add our P25 analysis
        size_t last_brace = existing_content.rfind("}");
        if (last_brace != std::string::npos) {
            std::string base_json = existing_content.substr(0, last_brace);
            
            // Remove trailing whitespace and comma if present
            while (!base_json.empty() && (base_json.back() == ' ' || base_json.back() == '\n' || base_json.back() == '\t')) {
                base_json.pop_back();
            }
            
            json << base_json;
            if (!base_json.empty() && base_json.back() != ',') {
                json << ",\n";
            }
            
            // Add our P25 frame analysis
            json << "  \"decoder_source\": \"trunk-decoder\",\n";
            json << "  \"input_file\": \"" << basename << "\",\n";
            json << "  \"p25_frames\": " << metadata_.total_frames << ",\n";
            json << "  \"voice_frames\": " << metadata_.voice_frames << ",\n";
            json << "  \"nac\": " << metadata_.nac << ",\n";
            json << "  \"note\": \"Enhanced with P25 frame analysis from trunk-decoder\"\n";
            json << "}";
        } else {
            // Fallback if JSON parsing fails
            has_existing_metadata = false;
        }
    }
    
    if (!has_existing_metadata) {
        // Check if we have external metadata to use
        if (!external_metadata_.empty()) {
            // Parse external metadata and merge with our decoder info
            std::string base_json = external_metadata_;
            
            // Remove closing brace to add our fields
            size_t last_brace = base_json.rfind("}");
            if (last_brace != std::string::npos) {
                base_json = base_json.substr(0, last_brace);
                
                // Remove trailing whitespace and comma if present
                while (!base_json.empty() && (base_json.back() == ' ' || base_json.back() == '\n' || base_json.back() == '\t')) {
                    base_json.pop_back();
                }
                
                json << base_json;
                if (!base_json.empty() && base_json.back() != ',') {
                    json << ",\n";
                }
                
                // Add decoder-specific info
                json << "  \"decoder_source\": \"trunk-decoder\",\n";
                json << "  \"input_file\": \"" << basename << "\",\n";
                json << "  \"p25_frames\": " << metadata_.total_frames << ",\n";
                json << "  \"voice_frames\": " << metadata_.voice_frames << "\n";
                json << "}";
            } else {
                // Fallback if parsing fails
                has_existing_metadata = false;
            }
        }
        
        if (external_metadata_.empty()) {
            // Generate basic metadata (original behavior)
            json << "{\n";
            json << "  \"call_length\": " << metadata_.call_length << ",\n";
            json << "  \"audio_type\": \"" << metadata_.audio_type << "\",\n";
            json << "  \"nac\": " << metadata_.nac << ",\n";
            json << "  \"encrypted\": " << (metadata_.has_encrypted_frames ? 1 : 0) << ",\n";
            
            // Decoder-specific info
            json << "  \"decoder_source\": \"trunk-decoder\",\n";
            json << "  \"input_file\": \"" << basename << "\",\n";
            json << "  \"p25_frames\": " << metadata_.total_frames << ",\n";
            json << "  \"voice_frames\": " << metadata_.voice_frames << "\n";
            
            json << "}";
        }
    }
    
    return json.str();
}

bool P25Decoder::decode_to_audio(const std::string& output_prefix) {
    if (!parser_) {
        std::cerr << "Error: No P25 file opened" << std::endl;
        return false;
    }
    
    output_prefix_ = output_prefix;
    
    // Setup WAV audio output
    if (!setup_wav_output(output_prefix)) {
        return false;
    }
    
    // Only show these messages in verbose mode now
    // std::cout << "Decoding P25 file: " << input_filename_ << std::endl;
    // std::cout << "Output prefix: " << output_prefix << std::endl;
    // std::cout << std::endl;
    
    P25Frame frame;
    int frame_count = 0;
    
    while (parser_->read_frame(frame)) {
        frame_count++;
        metadata_.total_frames++;
        
        // Update metadata from first frame
        if (frame_count == 1) {
            metadata_.nac = frame.nac;
        }
        
        // Check for encrypted frames
        if (frame.is_encrypted) {
            metadata_.has_encrypted_frames = true;
        }
        
        // Print frame info if text dump is enabled
        if (text_dump_enabled_) {
            std::cout << "Frame " << frame_count << ":\n";
            std::cout << parser_->dump_frame_text(frame);
        }
        
        // Process voice frames
        if (frame.is_voice_frame) {
            metadata_.voice_frames++;
            
            std::vector<int16_t> audio_samples;
            if (decode_voice_frame(frame, audio_samples)) {
                write_audio_samples(audio_samples);
                
                // Accumulate audio buffer for later processing if needed
                audio_buffer_.insert(audio_buffer_.end(), 
                                   audio_samples.begin(), audio_samples.end());
            }
        }
        
        if (text_dump_enabled_) {
            std::cout << "----------------------------------------\n";
        }
    }
    
    // Finalize metadata
    metadata_.end_time = time(nullptr);
    metadata_.call_length = audio_buffer_.size() / 8000.0; // Duration in seconds at 8kHz
    
    // Close audio file
    close_audio_output();
    
    // Convert to modern format if requested
    if (audio_format_ != "wav") {
        std::string wav_file = output_prefix + ".wav";
        std::string extension;
        if (audio_format_ == "mp3") extension = "mp3";
        else if (audio_format_ == "m4a") extension = "m4a";
        else if (audio_format_ == "opus") extension = "opus";
        else if (audio_format_ == "webm") extension = "webm";
        
        std::string final_audio_file = output_prefix + "." + extension;
        
        if (convert_to_modern_format(wav_file, final_audio_file)) {
            // Keep WAV file - don't remove it, users may want both formats
            // std::remove(wav_file.c_str()); // Commented out - preserve WAV
        }
    }
    
    // Write JSON metadata
    std::string json_filename = output_prefix + ".json";
    std::ofstream json_file(json_filename);
    if (json_file.is_open()) {
        json_file << generate_json_metadata();
        json_file.close();
        if (text_dump_enabled_) {
            std::cout << "Wrote metadata to: " << json_filename << std::endl;
        }
    }
    
    // Completion info is now only shown when text_dump_enabled is true
    
    return true;
}

void P25Decoder::set_output_sample_rate(int rate) {
    // For P25, output is always 8kHz
    if (rate != 8000) {
        std::cout << "Warning: P25 audio is always 8kHz, ignoring requested rate " << rate << std::endl;
    }
}

void P25Decoder::enable_text_dump(bool enable) {
    text_dump_enabled_ = enable;
}

bool P25Decoder::process_frames_only() {
    if (!parser_) {
        std::cerr << "Error: No P25 file opened" << std::endl;
        return false;
    }
    
    P25Frame frame;
    int frame_count = 0;
    
    while (parser_->read_frame(frame)) {
        frame_count++;
        metadata_.total_frames++;
        
        // Update metadata from first frame
        if (frame_count == 1) {
            metadata_.nac = frame.nac;
        }
        
        // Check for encrypted frames
        if (frame.is_encrypted) {
            metadata_.has_encrypted_frames = true;
        }
        
        // Print frame info if text dump is enabled
        if (text_dump_enabled_) {
            std::cout << "Frame " << frame_count << ":\n";
            std::cout << parser_->dump_frame_text(frame);
        }
        
        // Process voice frames (for metadata only, no audio output)
        if (frame.is_voice_frame) {
            metadata_.voice_frames++;
        }
        
        if (text_dump_enabled_) {
            std::cout << "----------------------------------------\n";
        }
    }
    
    // Finalize metadata
    metadata_.end_time = time(nullptr);
    metadata_.call_length = metadata_.voice_frames * 0.18; // Approximation: 180ms per voice frame
    
    // Frame processing completion info only shown when text_dump_enabled is true
    
    return true;
}

bool P25Decoder::save_json_metadata(const std::string& filename) {
    std::ofstream json_file(filename);
    if (!json_file.is_open()) {
        std::cerr << "Error: Failed to open JSON file for writing: " << filename << std::endl;
        return false;
    }
    
    json_file << generate_json_metadata();
    json_file.close();
    
    if (text_dump_enabled_) {
        std::cout << "Wrote JSON metadata to: " << filename << std::endl;
    }
    
    return true;
}

bool P25Decoder::save_text_dump(const std::string& filename) {
    if (!parser_) {
        std::cerr << "Error: No P25 file opened" << std::endl;
        return false;
    }
    
    std::ofstream text_file(filename);
    if (!text_file.is_open()) {
        std::cerr << "Error: Failed to open text file for writing: " << filename << std::endl;
        return false;
    }
    
    // Write header information
    text_file << "P25 Frame Analysis Report\n";
    text_file << "=========================\n\n";
    text_file << "Input file: " << input_filename_ << "\n";
    text_file << "Total frames: " << metadata_.total_frames << "\n";
    text_file << "Voice frames: " << metadata_.voice_frames << "\n";
    text_file << "NAC: 0x" << std::hex << metadata_.nac << std::dec << " (" << metadata_.nac << ")\n";
    text_file << "Duration: ~" << metadata_.call_length << " seconds\n";
    text_file << "\n";
    
    // Re-process file to generate text dump
    // Reset parser to beginning of file
    parser_.reset(new P25FrameParser());
    if (!parser_->open(input_filename_)) {
        text_file.close();
        std::cerr << "Error: Failed to reopen P25 file for text dump: " << input_filename_ << std::endl;
        return false;
    }
    
    P25Frame frame;
    int frame_count = 0;
    
    while (parser_->read_frame(frame)) {
        frame_count++;
        
        text_file << "Frame " << frame_count << ":\n";
        text_file << parser_->dump_frame_text(frame);
        text_file << "----------------------------------------\n";
    }
    
    text_file.close();
    
    if (text_dump_enabled_) {
        std::cout << "Wrote text dump to: " << filename << std::endl;
    }
    
    return true;
}

bool P25Decoder::save_csv_dump(const std::string& filename) {
    if (!parser_) {
        std::cerr << "Error: No P25 file opened" << std::endl;
        return false;
    }
    
    std::ofstream csv_file(filename);
    if (!csv_file.is_open()) {
        std::cerr << "Error: Failed to open CSV file for writing: " << filename << std::endl;
        return false;
    }
    
    // Write CSV header
    csv_file << "Frame,DUID,DUID_Name,NAC,Length_Bytes,Is_Voice_Frame,Is_Encrypted,Emergency_Flag,Talk_Group,Source_ID,Algorithm_ID,Key_ID,Data_Size,Frame_Data_Hex\n";
    
    // Re-process file to generate CSV data
    // Reset parser to beginning of file
    parser_.reset(new P25FrameParser());
    if (!parser_->open(input_filename_)) {
        csv_file.close();
        std::cerr << "Error: Failed to reopen P25 file for CSV dump: " << input_filename_ << std::endl;
        return false;
    }
    
    P25Frame frame;
    int frame_count = 0;
    
    while (parser_->read_frame(frame)) {
        frame_count++;
        
        // Check for encrypted frames and update metadata
        if (frame.is_encrypted) {
            metadata_.has_encrypted_frames = true;
        }
        
        // Populate missing frame metadata from call metadata
        frame.talk_group = (metadata_.talkgroup > 0) ? (uint16_t)metadata_.talkgroup : 0;
        frame.source_id = (metadata_.source_id > 0) ? (uint32_t)metadata_.source_id : 0;
        
        // Format frame data as CSV
        csv_file << frame_count << ",";
        csv_file << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)frame.duid << ",";
        csv_file << "\"" << frame.frame_type_name << "\",";
        csv_file << "0x" << std::hex << frame.nac << ",";
        csv_file << std::dec << frame.length << ",";
        csv_file << (frame.is_voice_frame ? "YES" : "NO") << ",";
        csv_file << (frame.is_encrypted ? "YES" : "NO") << ",";
        csv_file << (frame.emergency_flag ? "YES" : "NO") << ",";
        csv_file << frame.talk_group << ",";
        csv_file << frame.source_id << ",";
        csv_file << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)frame.algorithm_id << ",";
        csv_file << frame.key_id << ",";
        csv_file << std::dec << frame.data.size() << ",";
        
        // Add hex dump of frame data
        csv_file << "\"";
        for (size_t i = 0; i < frame.data.size(); i++) {
            csv_file << std::hex << std::setw(2) << std::setfill('0') << (int)frame.data[i];
            if (i < frame.data.size() - 1) {
                csv_file << " ";
            }
        }
        csv_file << "\"\n";
    }
    
    csv_file.close();
    
    if (text_dump_enabled_) {
        std::cout << "Wrote CSV dump to: " << filename << std::endl;
    }
    
    return true;
}

bool P25Decoder::add_des_key(uint16_t keyid, const std::vector<uint8_t>& key) {
    if (des_decrypt_) {
        return des_decrypt_->add_key(keyid, key);
    }
    return false;
}

bool P25Decoder::add_aes_key(uint16_t keyid, const std::vector<uint8_t>& key) {
    if (aes_decrypt_) {
        return aes_decrypt_->add_key(keyid, key);
    }
    return false;
}

bool P25Decoder::add_adp_key(uint16_t keyid, const std::vector<uint8_t>& key) {
    if (adp_decrypt_) {
        return adp_decrypt_->add_key(keyid, key);
    }
    return false;
}

void P25Decoder::enable_decryption(bool enable) {
    decryption_enabled_ = enable;
}

void P25Decoder::set_external_metadata(const std::string& json_metadata) {
    if (json_metadata.empty()) return;
    
    // Store the external metadata to merge with our decoder metadata
    external_metadata_ = json_metadata;
}

void P25Decoder::set_audio_format(const std::string& format) {
    audio_format_ = format;
}

void P25Decoder::set_audio_bitrate(int bitrate) {
    audio_bitrate_ = bitrate;
}

bool P25Decoder::convert_to_modern_format(const std::string& wav_file, const std::string& output_file) {
    std::string command;
    
    // Determine bitrate - use configured value or format defaults
    int bitrate = audio_bitrate_;
    if (bitrate == 0) { // Auto-select based on format
        if (audio_format_ == "mp3" || audio_format_ == "m4a") bitrate = 64;
        else if (audio_format_ == "opus" || audio_format_ == "webm") bitrate = 32;
    }
    
    // Base command with mono forced and sample rate
    std::string base_opts = " -ac 1 -ar 8000";
    std::string bitrate_str = std::to_string(bitrate) + "k";
    
    if (audio_format_ == "mp3") {
        // MP3 - legacy compatibility, good browser support
        command = "ffmpeg -i \"" + wav_file + "\"" + base_opts + " -c:a libmp3lame -b:a " + bitrate_str + " \"" + output_file + "\" 2>/dev/null";
    } else if (audio_format_ == "m4a") {
        // AAC in M4A container - web optimized, good quality/size balance
        command = "ffmpeg -i \"" + wav_file + "\"" + base_opts + " -c:a aac -b:a " + bitrate_str + " -movflags +faststart \"" + output_file + "\" 2>/dev/null";
    } else if (audio_format_ == "opus") {
        // Opus codec - best compression for voice
        command = "ffmpeg -i \"" + wav_file + "\"" + base_opts + " -c:a libopus -b:a " + bitrate_str + " \"" + output_file + "\" 2>/dev/null";
    } else if (audio_format_ == "webm") {
        // WebM container with Opus - native web format
        command = "ffmpeg -i \"" + wav_file + "\"" + base_opts + " -c:a libopus -b:a " + bitrate_str + " \"" + output_file + "\" 2>/dev/null";
    } else {
        return false;
    }
    
    int result = std::system(command.c_str());
    return result == 0;
}