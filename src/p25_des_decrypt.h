#ifndef P25_DES_DECRYPT_H
#define P25_DES_DECRYPT_H

#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>

class P25DESDecrypt {
private:
    std::unordered_map<uint16_t, std::vector<uint8_t>> d_keys;
    uint8_t d_keystream[224];
    uint32_t d_position;
    
    // DES implementation methods
    std::string hex2bin(const std::string& s);
    std::string bin2hex(const std::string& s);
    std::string permute(const std::string& k, const int* arr, int n);
    std::string shift_left(const std::string& k, int shifts);
    std::string xor_(const std::string& a, const std::string& b);
    std::string encrypt(const std::string& pt, const std::vector<std::string>& rkb, const std::vector<std::string>& rk);
    std::string byteArray2string(const uint8_t array[8]);
    void string2ByteArray(const std::string& s, uint8_t array[8]);
    
    // Keystream generation
    void generate_keystream(const uint8_t key[8], const uint8_t mi[9]);
    
public:
    P25DESDecrypt();
    ~P25DESDecrypt();
    
    // Key management
    bool add_key(uint16_t keyid, const std::vector<uint8_t>& key);
    bool has_key(uint16_t keyid) const;
    
    // Decryption preparation and processing
    bool prepare(uint16_t keyid, const uint8_t mi[9]);
    bool decrypt_imbe_codeword(std::vector<uint8_t>& codeword, bool is_ldu2, int voice_frame_num);
};

#endif // P25_DES_DECRYPT_H