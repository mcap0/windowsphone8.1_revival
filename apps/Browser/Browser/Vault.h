#pragma once

// The PIN vault of a site app (PLAN §64.1): the site's username and password, encrypted with a key derived from a PIN
// that the user types when the app opens. The PIN itself is never stored.
//
//   LocalFolder\vault.bin = DPAPI("LOCAL=user") of: "DPV1" | PBKDF2 iterations (u32 LE) | salt (16) | nonce (12) |
//                           AES-256-GCM tag (16) | ciphertext of UTF-8 "username\npassword"
//   key = PBKDF2-SHA256(PIN, salt, iterations), iterations calibrated on the phone at creation (~0.4 s).
//
// A wrong PIN fails the GCM tag. DPAPI binds the file to this phone and user, so a copy of it is useless elsewhere.

#include <memory>
#include <string>

namespace Vault
{
    struct Credentials
    {
        std::wstring user;
        std::wstring password;
    };

    bool Exists();
    // On a thread-pool thread (PBKDF2 takes ~0.4 s). Throws Platform::Exception on failure.
    concurrency::task<void> Create(const std::wstring& pin, const Credentials& credentials);
    // null when the PIN is wrong (or the file is damaged).
    concurrency::task<std::shared_ptr<Credentials>> Open(const std::wstring& pin);
    void Remove();
    // Overwrites the string's characters before it is released.
    void Wipe(std::wstring& text);

    // Internet's saved passwords for servers that ask (HTTP authentication): one text in LocalFolder\<name>, DPAPI
    // "LOCAL=user" (useless on another phone). Read: empty when there is none or it cannot be read. Write: empty text
    // removes the file.
    concurrency::task<std::wstring> ReadProtectedText(const std::wstring& name);
    concurrency::task<void> WriteProtectedText(const std::wstring& name, const std::wstring& text);
}
