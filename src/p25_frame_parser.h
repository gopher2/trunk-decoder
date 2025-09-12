#ifndef P25_FRAME_PARSER_H
#define P25_FRAME_PARSER_H

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>

struct P25Frame {
    uint8_t duid;           // Data Unit ID
    uint16_t nac;           // Network Access Code  
    uint16_t length;        // Frame length in bytes
    std::vector<uint8_t> data;  // Raw frame data
    
    // Parsed frame info
    std::string frame_type_name;
    bool is_voice_frame;
    bool is_encrypted;      // Frame contains encrypted voice data
    bool emergency_flag;    // Emergency transmission flag
    uint16_t talk_group;    // Talk group ID (if available)
    uint32_t source_id;     // Radio source ID (if available)
    uint8_t algorithm_id;   // Algorithm ID (ALGID) for encryption
    uint16_t key_id;        // Key ID (KID) for encryption
    
    P25Frame() : duid(0), nac(0), length(0), is_voice_frame(false), 
                 is_encrypted(false), emergency_flag(false), talk_group(0), source_id(0),
                 algorithm_id(0), key_id(0) {}
};

class P25FrameParser {
private:
    std::ifstream file_;
    std::string filename_;
    
    // Frame type mapping
    std::string get_frame_type_name(uint8_t duid);
    
    // Parse encryption fields from LDU2 frames
    void parse_encryption_fields(P25Frame& frame);
    
    // Hamming decoder helper
    uint8_t hamming_10_6_decode(uint8_t high, uint8_t low);
    
public:
    P25FrameParser();
    ~P25FrameParser();
    
    bool open(const std::string& filename);
    void close();
    
    // Read next frame from file
    bool read_frame(P25Frame& frame);
    
    // Parse and dump frame info as text
    std::string dump_frame_text(const P25Frame& frame);
    
    // Check if file has more frames
    bool has_more_frames();
};

#endif // P25_FRAME_PARSER_H