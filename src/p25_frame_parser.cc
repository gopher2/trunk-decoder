#include "p25_frame_parser.h"
#include <iostream>
#include <iomanip>
#include <sstream>

P25FrameParser::P25FrameParser() {
}

P25FrameParser::~P25FrameParser() {
    close();
}

bool P25FrameParser::open(const std::string& filename) {
    filename_ = filename;
    file_.open(filename, std::ios::binary);
    if (!file_.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return false;
    }
    return true;
}

void P25FrameParser::close() {
    if (file_.is_open()) {
        file_.close();
    }
}

bool P25FrameParser::has_more_frames() {
    return file_.is_open() && !file_.eof() && file_.peek() != EOF;
}

std::string P25FrameParser::get_frame_type_name(uint8_t duid) {
    switch (duid) {
        case 0x00: return "HDU (Header Data Unit)";
        case 0x03: return "TDU (Terminator Data Unit)";
        case 0x05: return "LDU1 (Logical Data Unit 1)";
        case 0x07: return "TDU (Terminator Data Unit)";
        case 0x0A: return "LDU2 (Logical Data Unit 2)";
        case 0x0C: return "PDU (Packet Data Unit)";
        case 0x0F: return "TDU (Terminator Data Unit)";
        case 0x12: return "TSBK (Trunking System Block)";
        default: return "Unknown DUID (" + std::to_string(duid) + ")";
    }
}

bool P25FrameParser::read_frame(P25Frame& frame) {
    if (!file_.is_open() || file_.eof()) {
        return false;
    }
    
    // Read 5-byte frame header: DUID + NAC + Length
    uint8_t header[5];
    file_.read(reinterpret_cast<char*>(header), 5);
    
    if (file_.gcount() != 5) {
        return false; // End of file or read error
    }
    
    // Parse header
    frame.duid = header[0];
    frame.nac = (header[1] << 8) | header[2];
    frame.length = (header[3] << 8) | header[4];
    
    // Set frame info
    frame.frame_type_name = get_frame_type_name(frame.duid);
    frame.is_voice_frame = (frame.duid == 0x05 || frame.duid == 0x0A); // LDU1 or LDU2
    
    // Read frame data
    frame.data.resize(frame.length);
    file_.read(reinterpret_cast<char*>(frame.data.data()), frame.length);
    
    if (file_.gcount() != frame.length) {
        std::cerr << "Warning: Expected " << frame.length << " bytes, got " << file_.gcount() << std::endl;
        frame.data.resize(file_.gcount());
        return false;
    }
    
    // Parse encryption fields for LDU2 frames
    if (frame.duid == 0x0A) { // LDU2
        parse_encryption_fields(frame);
    }
    
    return true;
}

std::string P25FrameParser::dump_frame_text(const P25Frame& frame) {
    std::ostringstream oss;
    
    oss << "==== P25 Frame ====\n";
    oss << "DUID: 0x" << std::hex << std::setw(2) << std::setfill('0') << (int)frame.duid << " (" << frame.frame_type_name << ")\n";
    oss << "NAC:  0x" << std::hex << std::setw(3) << std::setfill('0') << frame.nac << std::dec << " (" << frame.nac << ")\n";
    oss << "Length: " << frame.length << " bytes (" << (frame.length * 8) << " bits)\n";
    
    if (frame.is_voice_frame) {
        oss << "Voice Frame: YES (contains IMBE voice data)\n";
    } else {
        oss << "Voice Frame: NO\n";
    }
    
    // Dump all raw data in hex
    oss << "Raw Data (" << frame.data.size() << " bytes):\n";
    for (size_t i = 0; i < frame.data.size(); i += 16) {
        oss << std::hex << std::setw(4) << std::setfill('0') << i << ": ";
        for (size_t j = i; j < std::min(i + 16, frame.data.size()); j++) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)frame.data[j] << " ";
        }
        oss << "\n";
    }
    
    oss << std::dec; // Reset to decimal
    oss << "\n";
    
    return oss.str();
}

void P25FrameParser::parse_encryption_fields(P25Frame& frame) {
    // Proper P25 LDU2 deinterleaving based on OP25 implementation
    // LDU2 contains Encryption Sync (ES) data with Algorithm ID, Key ID, and Message Indicator
    
    if (frame.data.size() < 216) { // Standard P25 LDU frame is 216 bytes
        return;
    }
    
    // Convert bytes to bit vector for deinterleaving (P25 uses bit-level interleaving)
    std::vector<bool> bits(frame.data.size() * 8);
    for (size_t i = 0; i < frame.data.size(); i++) {
        for (int j = 7; j >= 0; j--) {
            bits[i * 8 + (7 - j)] = (frame.data[i] >> j) & 1;
        }
    }
    
    // P25 LDU2 Link Signaling data bit positions (from OP25)
    static const uint16_t imbe_ldu_ls_data_bits[] = {
        410,  411,  412,  413,  414,  415,  416,  417,  418,  419,  420,  421,
        422,  423,  424,  425,  426,  427,  428,  429,  432,  433,  434,  435,
        436,  437,  438,  439,  440,  441,  442,  443,  444,  445,  446,  447,
        448,  449,  450,  451,  600,  601,  602,  603,  604,  605,  606,  607,
        608,  609,  610,  611,  612,  613,  614,  615,  616,  617,  618,  619,
        620,  621,  622,  623,  624,  625,  626,  627,  628,  629,  630,  631,
        632,  633,  634,  635,  636,  637,  638,  639,  788,  789,  792,  793,
        794,  795,  796,  797,  798,  799,  800,  801,  802,  803,  804,  805,
        806,  807,  808,  809,  810,  811,  812,  813,  814,  815,  816,  817,
        818,  819,  820,  821,  822,  823,  824,  825,  826,  827,  828,  829,
        978,  979,  980,  981,  982,  983,  984,  985,  986,  987,  988,  989,
        990,  991,  992,  993,  994,  995,  996,  997,  998,  999, 1000, 1001,
        1002, 1003, 1004, 1005, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015,
        1016, 1017, 1018, 1019, 1168, 1169, 1170, 1171, 1172, 1173, 1174, 1175,
        1176, 1177, 1178, 1179, 1180, 1181, 1182, 1183, 1184, 1185, 1186, 1187,
        1188, 1189, 1190, 1191, 1192, 1193, 1194, 1195, 1196, 1197, 1198, 1199,
        1200, 1201, 1202, 1203, 1204, 1205, 1206, 1207, 1356, 1357, 1358, 1359,
        1360, 1361, 1362, 1363, 1364, 1365, 1368, 1369, 1370, 1371, 1372, 1373,
        1374, 1375, 1376, 1377, 1378, 1379, 1380, 1381, 1382, 1383, 1384, 1385,
        1386, 1387, 1388, 1389, 1390, 1391, 1392, 1393, 1394, 1395, 1396, 1397
    };
    
    // Extract 24 10-bit codewords using proper deinterleaving
    std::vector<uint8_t> HB(63, 0); // Hexbit vector for Reed-Solomon processing
    int k = 0;
    
    for (int i = 0; i < 24; i++) { // 24 10-bit codewords
        uint32_t codeword = 0;
        for (int j = 0; j < 10; j++) { // 10 bits per codeword
            if (k < 240 && imbe_ldu_ls_data_bits[k] < bits.size()) {
                codeword = (codeword << 1) + (bits[imbe_ldu_ls_data_bits[k]] ? 1 : 0);
            }
            k++;
        }
        // Apply Hamming(10,6,3) decoding as per OP25
        HB[39 + i] = hamming_10_6_decode(codeword >> 4, codeword & 0x0f);
    }
    
    // Extract encryption fields from deinterleaved data (positions from OP25)
    // Message Indicator (MI) - 72 bits (9 bytes)
    int j = 39; // Starting position after Reed-Solomon processing
    uint8_t mi[9] = {0};
    for (int i = 0; i < 9;) {
        mi[i++] = (uint8_t)  (HB[j]         << 2) + (HB[j+1] >> 4);
        mi[i++] = (uint8_t) ((HB[j+1] & 0x0f) << 4) + (HB[j+2] >> 2);
        mi[i++] = (uint8_t) ((HB[j+2] & 0x03) << 6) +  HB[j+3];
        j += 4;
    }
    
    // Algorithm ID (ALGID) - 8 bits
    frame.algorithm_id = (HB[j] << 2) + (HB[j+1] >> 4);
    
    // Key ID (KID) - 16 bits  
    frame.key_id = ((HB[j+1] & 0x0f) << 12) + (HB[j+2] << 6) + HB[j+3];
    
    // Set encryption flag based on algorithm ID
    frame.is_encrypted = (frame.algorithm_id != 0x80); // 0x80 = ALG_UNENCRYPTED
}

// Simple Hamming(10,6,3) decoder - simplified version for extraction
uint8_t P25FrameParser::hamming_10_6_decode(uint8_t high, uint8_t low) {
    // This is a simplified version - production code should use full error correction
    // For now, just extract the data bits without error correction
    return (high << 2) | (low & 0x03);
}