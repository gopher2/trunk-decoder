#ifndef P25_ADP_DECRYPT_H
#define P25_ADP_DECRYPT_H

#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>

class P25ADPDecrypt {
private:
    std::unordered_map<uint16_t, std::vector<uint8_t>> d_keys;
    uint8_t d_mi[9];
    uint8_t d_keystream[469]; // ADP keystream size
    uint32_t d_position;
    
    // RC4 swap function
    inline void adp_swap(uint8_t *S, uint32_t i, uint32_t j) {
        uint8_t temp = S[i];
        S[i] = S[j];
        S[j] = temp;
    }
    
    // Keystream generation
    void generate_keystream(const uint8_t key[5], const uint8_t mi[9]);
    
public:
    P25ADPDecrypt();
    ~P25ADPDecrypt();
    
    // Key management
    bool add_key(uint16_t keyid, const std::vector<uint8_t>& key);
    bool has_key(uint16_t keyid) const;
    
    // Decryption preparation and processing
    bool prepare(uint16_t keyid, const uint8_t mi[9]);
    bool decrypt_imbe_codeword(std::vector<uint8_t>& codeword, bool is_ldu2, int voice_frame_num);
};

#endif // P25_ADP_DECRYPT_H