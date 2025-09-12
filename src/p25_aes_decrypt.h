#ifndef P25_AES_DECRYPT_H
#define P25_AES_DECRYPT_H

#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>

#define AES_BLOCKLEN 16

class P25AESDecrypt {
private:
    std::unordered_map<uint16_t, std::vector<uint8_t>> d_keys;
    uint8_t d_keystream[240]; // 16 bytes per block x 15 blocks (FDMA)
    uint32_t d_position;
    
    // AES-256 parameters
    unsigned Nb = 4;
    unsigned Nk = 8;  // AES-256
    unsigned Nr = 14; // 14 rounds for AES-256
    
    // AES implementation structures
    struct AES_ctx {
        uint8_t RoundKey[240];
        uint8_t Iv[16];
    };
    
    typedef uint8_t state_t[4][4];
    
    // AES S-boxes and constants
    static const uint8_t sbox[256];
    static const uint8_t rsbox[256];
    static const uint8_t Rcon[11];
    
    // AES internal functions
    void KeyExpansion(uint8_t* RoundKey, const uint8_t* Key);
    void AddRoundKey(uint8_t round, state_t* state, const uint8_t* RoundKey);
    void SubBytes(state_t* state);
    void ShiftRows(state_t* state);
    void MixColumns(state_t* state);
    void InvMixColumns(state_t* state);
    void InvSubBytes(state_t* state);
    void InvShiftRows(state_t* state);
    void Cipher(state_t* state, const uint8_t* RoundKey);
    void InvCipher(state_t* state, const uint8_t* RoundKey);
    
    // AES context functions
    void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);
    void AES_ctx_set_iv(struct AES_ctx* ctx, const uint8_t* iv);
    void AES_ECB_encrypt(const struct AES_ctx* ctx, uint8_t* buf);
    
    // Utility functions
    uint8_t xtime(uint8_t x);
    void XorWithIv(uint8_t* buf, const uint8_t* Iv);
    
    // Keystream generation
    void generate_keystream(const uint8_t key[32], const uint8_t mi[9]);
    
public:
    P25AESDecrypt();
    ~P25AESDecrypt();
    
    // Key management
    bool add_key(uint16_t keyid, const std::vector<uint8_t>& key);
    bool has_key(uint16_t keyid) const;
    
    // Decryption preparation and processing
    bool prepare(uint16_t keyid, const uint8_t mi[9]);
    bool decrypt_imbe_codeword(std::vector<uint8_t>& codeword, bool is_ldu2, int voice_frame_num);
};

#endif // P25_AES_DECRYPT_H