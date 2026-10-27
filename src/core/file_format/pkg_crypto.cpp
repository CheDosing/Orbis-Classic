// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#define OPENSSL_SUPPRESS_DEPRECATED

#include <cstring>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rsa.h>

#include "core/file_format/pkg_crypto.h"
#include "core/file_format/pkg_keys.h"

namespace PkgCrypto {

namespace {

template <typename Keyset>
RSA* CreateRsaKey() {
    const auto bn = [](const auto& bytes) {
        return BN_bin2bn(bytes, static_cast<int>(sizeof(bytes)), nullptr);
    };
    RSA* rsa = RSA_new();
    if (rsa == nullptr) {
        return nullptr;
    }
    BIGNUM* n = bn(Keyset::Modulus);
    BIGNUM* e = bn(Keyset::PublicExponent);
    BIGNUM* d = bn(Keyset::PrivateExponent);
    BIGNUM* p = bn(Keyset::Prime1);
    BIGNUM* q = bn(Keyset::Prime2);
    BIGNUM* dmp1 = bn(Keyset::Exponent1);
    BIGNUM* dmq1 = bn(Keyset::Exponent2);
    BIGNUM* iqmp = bn(Keyset::Coefficient);
    if (RSA_set0_key(rsa, n, e, d) != 1 || RSA_set0_factors(rsa, p, q) != 1 ||
        RSA_set0_crt_params(rsa, dmp1, dmq1, iqmp) != 1) {
        RSA_free(rsa);
        return nullptr;
    }
    return rsa;
}

} // namespace

bool RsaDecrypt(RsaKeyset keyset, std::span<const u8, 256> ciphertext, std::span<u8, 32> out) {
    RSA* rsa = keyset == RsaKeyset::PkgDerivedKey3 ? CreateRsaKey<PkgKeys::PkgDerivedKey3Keyset>()
                                                   : CreateRsaKey<PkgKeys::FakeKeyset>();
    if (rsa == nullptr) {
        return false;
    }
    std::array<u8, 256> decrypted{};
    const int length = RSA_private_decrypt(static_cast<int>(ciphertext.size()), ciphertext.data(),
                                           decrypted.data(), rsa, RSA_PKCS1_PADDING);
    RSA_free(rsa);
    if (length < static_cast<int>(out.size())) {
        return false;
    }
    std::memcpy(out.data(), decrypted.data(), out.size());
    return true;
}

std::array<u8, 32> Sha256(std::span<const u8> data) {
    std::array<u8, 32> digest{};
    EVP_Digest(data.data(), data.size(), digest.data(), nullptr, EVP_sha256(), nullptr);
    return digest;
}

bool AesCbcDecrypt(std::span<const u8, 32> iv_key, std::span<const u8> ciphertext,
                   std::span<u8> out) {
    if (out.size() < ciphertext.size()) {
        return false;
    }
    const size_t block_bytes = ciphertext.size() & ~size_t{0xF};
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return false;
    }
    int written = 0;
    const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, iv_key.data() + 16,
                                       iv_key.data()) == 1 &&
                    EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
                    EVP_DecryptUpdate(ctx, out.data(), &written, ciphertext.data(),
                                      static_cast<int>(block_bytes)) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (block_bytes < ciphertext.size()) {
        std::memcpy(out.data() + block_bytes, ciphertext.data() + block_bytes,
                    ciphertext.size() - block_bytes);
    }
    return ok;
}

void PfsGenCryptoKey(std::span<const u8, 32> ekpfs, std::span<const u8, 16> seed,
                     std::span<u8, 16> data_key, std::span<u8, 16> tweak_key) {
    std::array<u8, 20> message{};
    const u32 index = 1;
    std::memcpy(message.data(), &index, sizeof(index));
    std::memcpy(message.data() + sizeof(index), seed.data(), seed.size());

    std::array<u8, 32> digest{};
    unsigned int digest_length = 0;
    HMAC(EVP_sha256(), ekpfs.data(), static_cast<int>(ekpfs.size()), message.data(), message.size(),
         digest.data(), &digest_length);
    std::memcpy(tweak_key.data(), digest.data(), tweak_key.size());
    std::memcpy(data_key.data(), digest.data() + tweak_key.size(), data_key.size());
}

XtsDecryptor::XtsDecryptor(std::span<const u8, 16> data_key, std::span<const u8, 16> tweak_key) {
    std::array<u8, 32> key{};
    std::memcpy(key.data(), data_key.data(), data_key.size());
    std::memcpy(key.data() + data_key.size(), tweak_key.data(), tweak_key.size());
    auto* cipher_ctx = EVP_CIPHER_CTX_new();
    if (cipher_ctx == nullptr) {
        return;
    }
    if (EVP_DecryptInit_ex(cipher_ctx, EVP_aes_128_xts(), nullptr, key.data(), nullptr) != 1) {
        EVP_CIPHER_CTX_free(cipher_ctx);
        return;
    }
    ctx = cipher_ctx;
}

XtsDecryptor::~XtsDecryptor() {
    EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX*>(ctx));
}

bool XtsDecryptor::Decrypt(std::span<const u8> src, std::span<u8> dst, u64 first_sector) {
    if (ctx == nullptr || src.size() % XtsSectorSize != 0 || dst.size() < src.size()) {
        return false;
    }
    auto* cipher_ctx = static_cast<EVP_CIPHER_CTX*>(ctx);
    for (u64 offset = 0; offset < src.size(); offset += XtsSectorSize) {
        std::array<u8, 16> tweak{};
        const u64 sector = first_sector + offset / XtsSectorSize;
        for (size_t i = 0; i < sizeof(sector); i++) {
            tweak[i] = static_cast<u8>(sector >> (8 * i));
        }
        int written = 0;
        if (EVP_DecryptInit_ex(cipher_ctx, nullptr, nullptr, nullptr, tweak.data()) != 1 ||
            EVP_DecryptUpdate(cipher_ctx, dst.data() + offset, &written, src.data() + offset,
                              static_cast<int>(XtsSectorSize)) != 1) {
            return false;
        }
    }
    return true;
}

Sha256Stream::Sha256Stream() {
    auto* md_ctx = EVP_MD_CTX_new();
    if (md_ctx != nullptr && EVP_DigestInit_ex(md_ctx, EVP_sha256(), nullptr) == 1) {
        ctx = md_ctx;
    } else {
        EVP_MD_CTX_free(md_ctx);
    }
}

Sha256Stream::~Sha256Stream() {
    EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx));
}

void Sha256Stream::Update(std::span<const u8> data) {
    if (ctx != nullptr) {
        EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx), data.data(), data.size());
    }
}

std::array<u8, 32> Sha256Stream::Finish() {
    std::array<u8, 32> digest{};
    if (ctx != nullptr) {
        unsigned int length = 0;
        EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx), digest.data(), &length);
    }
    return digest;
}

} // namespace PkgCrypto
