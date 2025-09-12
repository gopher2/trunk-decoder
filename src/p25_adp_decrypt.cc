#include "p25_adp_decrypt.h"
#include <iostream>
#include <cstring>
#include <algorithm>

P25ADPDecrypt::P25ADPDecrypt() : d_position(0) {
    std::fill(d_keystream, d_keystream + 469, 0);
    std::fill(d_mi, d_mi + 9, 0);
}

P25ADPDecrypt::~P25ADPDecrypt() {
}

bool P25ADPDecrypt::add_key(uint16_t keyid, const std::vector<uint8_t>& key) {
    d_keys[keyid] = key;
    return true;
}

bool P25ADPDecrypt::has_key(uint16_t keyid) const {
    return d_keys.find(keyid) != d_keys.end();
}

bool P25ADPDecrypt::prepare(uint16_t keyid, const uint8_t mi[9]) {
    auto key_iter = d_keys.find(keyid);
    if (key_iter == d_keys.end()) {
        return false; // Key not found
    }
    
    d_position = 0;
    std::memcpy(d_mi, mi, sizeof(d_mi));
    
    // Prepare 5-byte ADP key from stored key
    uint8_t adp_key[5] = {0};
    const auto& stored_key = key_iter->second;
    
    // Pad with leading zeros if key is too short, copy up to 5 bytes
    for (size_t i = 0; i < 5 && i < stored_key.size(); i++) {
        adp_key[5 - stored_key.size() + i] = stored_key[i];
    }
    
    // Generate keystream using RC4 algorithm
    generate_keystream(adp_key, mi);
    
    return true;
}

bool P25ADPDecrypt::decrypt_imbe_codeword(std::vector<uint8_t>& codeword, bool is_ldu2, int voice_frame_num) {
    if (codeword.size() < 11) {
        return false; // IMBE codeword should be 11 bytes
    }
    
    // Calculate keystream offset based on P25 ADP specification
    size_t offset = 0; // ADP doesn't have initial discard like DES/AES
    
    if (is_ldu2) {
        offset += 101; // Additional offset for LDU2
    }
    
    // P25 Phase 1 FDMA voice frame offset calculation
    // ADP uses different offset calculation than DES/AES
    offset += (d_position * 11) + 267 + ((d_position < 8) ? 0 : 2);
    d_position = (d_position + 1) % 9;
    
    // XOR codeword with keystream
    for (int j = 0; j < 11 && j < (int)codeword.size(); ++j) {
        if (offset + j < 469) {
            codeword[j] = d_keystream[j + offset] ^ codeword[j];
        }
    }
    
    return true;
}

void P25ADPDecrypt::generate_keystream(const uint8_t key[5], const uint8_t mi[9]) {
    // ADP/RC4 keystream generation based on Boatboad's implementation
    uint8_t adp_key[13], S[256], K[256];
    uint32_t i, j, k;
    
    // Create 13-byte key by concatenating 5-byte key with 8 bytes of MI
    for (i = 0; i < 5; i++) {
        adp_key[i] = key[i];
    }
    
    // Append 8 bytes of MI (skip the 9th byte)
    for (i = 5; i < 13; ++i) {
        adp_key[i] = mi[i - 5];
    }
    
    // Initialize K array with repeated key pattern
    for (i = 0; i < 256; ++i) {
        K[i] = adp_key[i % 13];
    }
    
    // Initialize S array
    for (i = 0; i < 256; ++i) {
        S[i] = i;
    }
    
    // Key-scheduling algorithm (KSA)
    j = 0;
    for (i = 0; i < 256; ++i) {
        j = (j + S[i] + K[i]) & 0xFF;
        adp_swap(S, i, j);
    }
    
    // Pseudo-random generation algorithm (PRGA)
    i = j = 0;
    for (k = 0; k < 469; ++k) {
        i = (i + 1) & 0xFF;
        j = (j + S[i]) & 0xFF;
        adp_swap(S, i, j);
        d_keystream[k] = S[(S[i] + S[j]) & 0xFF];
    }
}