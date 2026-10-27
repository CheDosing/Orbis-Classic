// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include "common/types.h"

namespace PkgCrypto {

constexpr u64 XtsSectorSize = 0x1000;

enum class RsaKeyset {
    PkgDerivedKey3,
    Fake,
};

bool RsaDecrypt(RsaKeyset keyset, std::span<const u8, 256> ciphertext, std::span<u8, 32> out);

std::array<u8, 32> Sha256(std::span<const u8> data);

bool AesCbcDecrypt(std::span<const u8, 32> iv_key, std::span<const u8> ciphertext,
                   std::span<u8> out);

void PfsGenCryptoKey(std::span<const u8, 32> ekpfs, std::span<const u8, 16> seed,
                     std::span<u8, 16> data_key, std::span<u8, 16> tweak_key);

class XtsDecryptor {
public:
    XtsDecryptor(std::span<const u8, 16> data_key, std::span<const u8, 16> tweak_key);
    ~XtsDecryptor();

    XtsDecryptor(const XtsDecryptor&) = delete;
    XtsDecryptor& operator=(const XtsDecryptor&) = delete;

    bool IsValid() const {
        return ctx != nullptr;
    }

    bool Decrypt(std::span<const u8> src, std::span<u8> dst, u64 first_sector);

private:
    void* ctx{};
};

class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();

    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;

    void Update(std::span<const u8> data);
    std::array<u8, 32> Finish();

private:
    void* ctx{};
};

} // namespace PkgCrypto
