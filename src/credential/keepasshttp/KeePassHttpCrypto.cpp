#include "credential/keepasshttp/KeePassHttpCrypto.h"

#include <openssl/evp.h>

#include <cstring>

namespace openscrt::credential::keepasshttp {

std::string base64(const std::uint8_t* data, std::size_t size) {
    std::string out(4 * ((size + 2) / 3), '\0');
    const int written =
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
    if (written < 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

bool decodeBase64(const std::string& text, std::vector<std::uint8_t>& out) {
    out.clear();
    if (text.empty()) {
        return true;
    }
    out.resize(3 * (text.size() / 4) + 3);
    const int written =
        EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(text.data()),
                        static_cast<int>(text.size()));
    if (written < 0) {
        out.clear();
        return false;
    }
    std::size_t padding = 0;
    if (!text.empty() && text.back() == '=') {
        padding = 1;
        if (text.size() > 1 && text[text.size() - 2] == '=') {
            padding = 2;
        }
    }
    out.resize(static_cast<std::size_t>(written) - padding);
    return true;
}

bool encryptField(const std::string& key_base64, const std::string& nonce_base64,
                  const std::string& plaintext, std::string& out, std::string& error) {
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> nonce;
    if (!decodeBase64(key_base64, key) || !decodeBase64(nonce_base64, nonce)) {
        error = "the key or the nonce is not valid base64";
        return false;
    }
    if (key.size() != kKeyBytes || nonce.size() != kNonceBytes) {
        error = "the key or the nonce has the wrong length";
        return false;
    }

    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        error = "the cipher context could not be created";
        return false;
    }
    std::string encrypted;
    int length = 0;
    int total = 0;
    bool ok = false;
    if (EVP_EncryptInit_ex(context, EVP_aes_256_cbc(), nullptr, key.data(), nonce.data()) == 1) {
        encrypted.resize(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
        if (EVP_EncryptUpdate(context, reinterpret_cast<unsigned char*>(encrypted.data()), &length,
                              reinterpret_cast<const unsigned char*>(plaintext.data()),
                              static_cast<int>(plaintext.size())) == 1) {
            total = length;
            int final_length = 0;
            if (EVP_EncryptFinal_ex(context,
                                    reinterpret_cast<unsigned char*>(encrypted.data()) + total,
                                    &final_length) == 1) {
                total += final_length;
                encrypted.resize(static_cast<std::size_t>(total));
                out = base64(reinterpret_cast<const std::uint8_t*>(encrypted.data()),
                             encrypted.size());
                ok = true;
            }
        }
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        error = "the field could not be encrypted";
    }
    return ok;
}

bool decryptField(const std::string& key_base64, const std::string& nonce_base64,
                  const std::string& ciphertext_base64, std::string& out, std::string& error) {
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> nonce;
    std::vector<std::uint8_t> ciphertext;
    if (!decodeBase64(key_base64, key) || !decodeBase64(nonce_base64, nonce) ||
        !decodeBase64(ciphertext_base64, ciphertext)) {
        error = "the key, the nonce or the payload is not valid base64";
        return false;
    }
    if (key.size() != kKeyBytes || nonce.size() != kNonceBytes) {
        error = "the key or the nonce has the wrong length";
        return false;
    }

    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        error = "the cipher context could not be created";
        return false;
    }
    std::string plain;
    int length = 0;
    int total = 0;
    bool ok = false;
    if (EVP_DecryptInit_ex(context, EVP_aes_256_cbc(), nullptr, key.data(), nonce.data()) == 1) {
        plain.resize(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
        if (EVP_DecryptUpdate(context, reinterpret_cast<unsigned char*>(plain.data()), &length,
                              ciphertext.data(), static_cast<int>(ciphertext.size())) == 1) {
            total = length;
            int final_length = 0;
            if (EVP_DecryptFinal_ex(context, reinterpret_cast<unsigned char*>(plain.data()) + total,
                                    &final_length) == 1) {
                total += final_length;
                plain.resize(static_cast<std::size_t>(total));
                out = plain;
                ok = true;
            }
        }
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        error = "the field could not be decrypted (the key does not match)";
    }
    return ok;
}

}  // namespace openscrt::credential::keepasshttp
