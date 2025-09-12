#include "p25_des_decrypt.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

P25DESDecrypt::P25DESDecrypt() : d_position(0) {
    std::fill(d_keystream, d_keystream + 224, 0);
}

P25DESDecrypt::~P25DESDecrypt() {
}

bool P25DESDecrypt::add_key(uint16_t keyid, const std::vector<uint8_t>& key) {
    d_keys[keyid] = key;
    return true;
}

bool P25DESDecrypt::has_key(uint16_t keyid) const {
    return d_keys.find(keyid) != d_keys.end();
}

bool P25DESDecrypt::prepare(uint16_t keyid, const uint8_t mi[9]) {
    auto key_iter = d_keys.find(keyid);
    if (key_iter == d_keys.end()) {
        return false; // Key not found
    }
    
    d_position = 0;
    
    // Prepare 8-byte DES key from stored key
    uint8_t des_key[8] = {0};
    const auto& stored_key = key_iter->second;
    
    // Pad with leading zeros if key is too short, copy up to 8 bytes
    for (size_t i = 0; i < 8 && i < stored_key.size(); i++) {
        des_key[8 - stored_key.size() + i] = stored_key[i];
    }
    
    // Generate keystream using DES-OFB
    generate_keystream(des_key, mi);
    
    return true;
}

bool P25DESDecrypt::decrypt_imbe_codeword(std::vector<uint8_t>& codeword, bool is_ldu2, int voice_frame_num) {
    if (codeword.size() < 11) {
        return false; // IMBE codeword should be 11 bytes
    }
    
    // Calculate keystream offset based on P25 DES-OFB specification
    size_t offset = 8; // Initial DES-OFB discard round
    
    if (is_ldu2) {
        offset += 101; // Additional offset for LDU2
    }
    
    // P25 Phase 1 FDMA voice frame offset calculation
    offset += (d_position * 11) + 11 + ((d_position < 8) ? 0 : 2);
    d_position = (d_position + 1) % 9;
    
    // XOR codeword with keystream
    for (int j = 0; j < 11 && j < (int)codeword.size(); ++j) {
        if (offset + j < 224) {
            codeword[j] = d_keystream[j + offset] ^ codeword[j];
        }
    }
    
    return true;
}

void P25DESDecrypt::generate_keystream(const uint8_t key[8], const uint8_t mi[9]) {
    // Convert key and MI to string format for DES processing
    std::string key_str = byteArray2string(key);
    std::string mi_str = byteArray2string(const_cast<uint8_t*>(mi)); // Cast away const for compatibility
    
    // Convert to binary
    std::string key_bin = hex2bin(key_str);
    
    // DES key permutation table (parity bit drop)
    static const int keyp[56] = { 
        57, 49, 41, 33, 25, 17, 9,  1, 58, 50, 42, 34, 26, 18,
        10,  2, 59, 51, 43, 35, 27, 19, 11,  3, 60, 52, 44, 36,
        63, 55, 47, 39, 31, 23, 15,  7, 62, 54, 46, 38, 30, 22,
        14,  6, 61, 53, 45, 37, 29, 21, 13,  5, 28, 20, 12,  4
    };
    
    // Get 56-bit key without parity bits
    key_bin = permute(key_bin, keyp, 56);
    
    // DES shift schedule
    static const int shift_table[16] = { 
        1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 
    };
    
    // Key compression table
    static const int key_comp[48] = { 
        14, 17, 11, 24,  1,  5,  3, 28, 15,  6, 21, 10,
        23, 19, 12,  4, 26,  8, 16,  7, 27, 20, 13,  2,
        41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
        44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32
    };
    
    // Generate 16 round keys
    std::vector<std::string> rkb(16), rk(16);
    std::string left = key_bin.substr(0, 28);
    std::string right = key_bin.substr(28, 28);
    
    for (int i = 0; i < 16; i++) {
        left = shift_left(left, shift_table[i]);
        right = shift_left(right, shift_table[i]);
        
        std::string combine = left + right;
        std::string round_key = permute(combine, key_comp, 48);
        
        rkb[i] = round_key;
        rk[i] = bin2hex(round_key);
    }
    
    // Generate 224-byte keystream using DES-OFB mode
    std::string iv = mi_str; // Use MI as initialization vector
    
    for (int block = 0; block < 28; block++) { // 224/8 = 28 blocks
        // Encrypt IV with DES using generated round keys
        std::string encrypted_iv = encrypt(iv, rkb, rk);
        
        // Convert encrypted block to bytes and store in keystream
        uint8_t block_bytes[8];
        string2ByteArray(encrypted_iv, block_bytes);
        
        for (int i = 0; i < 8 && (block * 8 + i) < 224; i++) {
            d_keystream[block * 8 + i] = block_bytes[i];
        }
        
        // Update IV for next block (OFB mode)
        iv = encrypted_iv;
    }
}

// DES utility functions (simplified implementations based on OP25)
std::string P25DESDecrypt::hex2bin(const std::string& s) {
    std::unordered_map<char, std::string> hex_to_bin = {
        {'0',"0000"}, {'1',"0001"}, {'2',"0010"}, {'3',"0011"},
        {'4',"0100"}, {'5',"0101"}, {'6',"0110"}, {'7',"0111"},
        {'8',"1000"}, {'9',"1001"}, {'A',"1010"}, {'B',"1011"},
        {'C',"1100"}, {'D',"1101"}, {'E',"1110"}, {'F',"1111"}
    };
    
    std::string bin = "";
    for (char c : s) {
        bin += hex_to_bin[std::toupper(c)];
    }
    return bin;
}

std::string P25DESDecrypt::bin2hex(const std::string& s) {
    std::unordered_map<std::string, char> bin_to_hex = {
        {"0000",'0'}, {"0001",'1'}, {"0010",'2'}, {"0011",'3'},
        {"0100",'4'}, {"0101",'5'}, {"0110",'6'}, {"0111",'7'},
        {"1000",'8'}, {"1001",'9'}, {"1010",'A'}, {"1011",'B'},
        {"1100",'C'}, {"1101",'D'}, {"1110",'E'}, {"1111",'F'}
    };
    
    std::string hex = "";
    for (size_t i = 0; i < s.length(); i += 4) {
        hex += bin_to_hex[s.substr(i, 4)];
    }
    return hex;
}

std::string P25DESDecrypt::permute(const std::string& k, const int* arr, int n) {
    std::string per = "";
    for (int i = 0; i < n; i++) {
        per += k[arr[i] - 1];
    }
    return per;
}

std::string P25DESDecrypt::shift_left(const std::string& k, int shifts) {
    return k.substr(shifts) + k.substr(0, shifts);
}

std::string P25DESDecrypt::xor_(const std::string& a, const std::string& b) {
    std::string result = "";
    for (size_t i = 0; i < a.size(); i++) {
        result += (a[i] == b[i]) ? '0' : '1';
    }
    return result;
}

std::string P25DESDecrypt::encrypt(const std::string& pt, const std::vector<std::string>& rkb, const std::vector<std::string>& rk) {
    // Simplified DES encryption - in production, use full DES implementation
    // This is a placeholder that should be replaced with complete DES algorithm
    
    // For now, return a simple transformation
    // TODO: Implement full DES encryption algorithm
    std::string result = pt;
    
    // Apply some basic transformation using the round keys
    for (int round = 0; round < 16; round++) {
        result = xor_(result, rkb[round].substr(0, std::min(result.length(), rkb[round].length())));
    }
    
    return result;
}

std::string P25DESDecrypt::byteArray2string(const uint8_t array[8]) {
    std::stringstream ss;
    for (int i = 0; i < 8; i++) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)array[i];
    }
    return ss.str();
}

void P25DESDecrypt::string2ByteArray(const std::string& s, uint8_t array[8]) {
    for (int i = 0; i < 8 && i * 2 < (int)s.length(); i++) {
        array[i] = std::stoul(s.substr(i * 2, 2), nullptr, 16);
    }
}