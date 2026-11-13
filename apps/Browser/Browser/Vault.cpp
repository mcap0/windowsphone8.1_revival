// The PIN vault (Vault.h). All the crypto is the system's own (Windows.Security.Cryptography: CNG underneath).
#include "pch.h"
#include "Vault.h"
#include <algorithm>
#include <cstring>

using namespace concurrency;
using namespace Platform;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::Security::Cryptography::DataProtection;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace
{
    const unsigned char magic[4] = { 'D', 'P', 'V', '1' };
    const size_t saltSize = 16;
    const size_t nonceSize = 12;
    const size_t tagSize = 16;
    const size_t headerSize = 4 + 4 + saltSize + nonceSize + tagSize;

    std::wstring Path()
    {
        return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\vault.bin";
    }

    std::vector<unsigned char> Bytes(IBuffer^ buffer)
    {
        Array<unsigned char>^ array = nullptr;
        CryptographicBuffer::CopyToByteArray(buffer, &array);
        if (!array || !array->Length)
            return std::vector<unsigned char>();
        return std::vector<unsigned char>(array->Data, array->Data + array->Length);
    }

    IBuffer^ ToBuffer(const unsigned char* data, size_t size)
    {
        return CryptographicBuffer::CreateFromByteArray(
            ArrayReference<unsigned char>(const_cast<unsigned char*>(data), static_cast<unsigned int>(size)));
    }

    IBuffer^ Utf8(const std::wstring& text)
    {
        return CryptographicBuffer::ConvertStringToBinary(ref new String(text.c_str(), static_cast<unsigned int>(text.size())),
            BinaryStringEncoding::Utf8);
    }

    // AES-256-GCM key from the PIN.
    CryptographicKey^ Key(const std::wstring& pin, const unsigned char* salt, unsigned int iterations)
    {
        auto kdf = KeyDerivationAlgorithmProvider::OpenAlgorithm(KeyDerivationAlgorithmNames::Pbkdf2Sha256);
        auto secret = kdf->CreateKey(Utf8(pin));
        auto parameters = KeyDerivationParameters::BuildForPbkdf2(ToBuffer(salt, saltSize), iterations);
        auto material = CryptographicEngine::DeriveKeyMaterial(secret, parameters, 32);
        return SymmetricKeyAlgorithmProvider::OpenAlgorithm(SymmetricAlgorithmNames::AesGcm)->CreateSymmetricKey(material);
    }

    // As many iterations as take ~0.4 s on this phone: each guess at the PIN costs that much too.
    unsigned int Calibrate(const std::wstring& pin)
    {
        unsigned char salt[saltSize] = {};
        const unsigned int probe = 20000;
        ULONGLONG start = GetTickCount64();
        Key(pin, salt, probe);
        ULONGLONG spent = std::max<ULONGLONG>(GetTickCount64() - start, 1);
        unsigned long long wanted = probe * 400ULL / spent;
        return static_cast<unsigned int>(std::min<unsigned long long>(std::max<unsigned long long>(wanted, 50000), 5000000));
    }

    bool ReadAll(const std::wstring& path, std::vector<unsigned char>& data)
    {
        HANDLE file = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        FILE_STANDARD_INFO info = {};
        bool ok = GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof info) && info.EndOfFile.QuadPart > 0
            && info.EndOfFile.QuadPart < 1024 * 1024;
        if (ok)
        {
            data.resize(static_cast<size_t>(info.EndOfFile.QuadPart));
            DWORD read = 0;
            ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size();
        }
        CloseHandle(file);
        return ok;
    }

    // Through a temporary file: a crash half-way never leaves a broken vault.
    bool WriteAll(const std::wstring& path, const std::vector<unsigned char>& data)
    {
        std::wstring temporary = path + L".tmp";
        HANDLE file = CreateFile2(temporary.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
        ok = FlushFileBuffers(file) && ok;
        CloseHandle(file);
        return ok && MoveFileEx(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

namespace Vault
{
    void Wipe(std::wstring& text)
    {
        if (!text.empty())
            SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
        text.clear();
    }

    std::wstring LocalFile(const std::wstring& name)
    {
        return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\" + name;
    }

    task<std::wstring> ReadProtectedText(const std::wstring& name)
    {
        std::vector<unsigned char> file;
        if (!ReadAll(LocalFile(name), file))
            return task_from_result(std::wstring());
        auto provider = ref new DataProtectionProvider();
        return create_task(provider->UnprotectAsync(ToBuffer(file.data(), file.size()))).then([](task<IBuffer^> unwrapped) -> std::wstring
        {
            try
            {
                return std::wstring(CryptographicBuffer::ConvertBinaryToString(BinaryStringEncoding::Utf8, unwrapped.get())->Data());
            }
            catch (Exception^)
            {
                return std::wstring();
            }
        }, task_continuation_context::use_arbitrary());
    }

    task<void> WriteProtectedText(const std::wstring& name, const std::wstring& text)
    {
        std::wstring path = LocalFile(name);
        if (text.empty())
        {
            DeleteFile(path.c_str());
            return task_from_result();
        }
        auto provider = ref new DataProtectionProvider(L"LOCAL=user");
        return create_task(provider->ProtectAsync(Utf8(text))).then([path](IBuffer^ wrapped)
        {
            if (!WriteAll(path, Bytes(wrapped)))
                throw ref new FailureException(L"file protetto non scritto");
        }, task_continuation_context::use_arbitrary());
    }

    bool Exists()
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        return GetFileAttributesEx(Path().c_str(), GetFileExInfoStandard, &attributes) != 0;
    }

    void Remove()
    {
        DeleteFile(Path().c_str());
    }

    task<void> Create(const std::wstring& pin, const Credentials& credentials)
    {
        std::wstring secret = pin;
        std::wstring plain = credentials.user + L"\n" + credentials.password;
        // VS2013 tasks call their lambda as const: the copies to wipe are made inside.
        return create_task([secret, plain]() -> IBuffer^
        {
            std::wstring pinText = secret;
            std::wstring plainText = plain;
            unsigned int iterations = Calibrate(pinText);
            auto salt = Bytes(CryptographicBuffer::GenerateRandom(saltSize));
            auto nonce = Bytes(CryptographicBuffer::GenerateRandom(nonceSize));
            auto key = Key(pinText, salt.data(), iterations);
            auto box = CryptographicEngine::EncryptAndAuthenticate(key, Utf8(plainText), ToBuffer(nonce.data(), nonceSize), nullptr);
            Wipe(pinText);
            Wipe(plainText);
            std::vector<unsigned char> blob(magic, magic + 4);
            for (int i = 0; i < 4; ++i)
                blob.push_back(static_cast<unsigned char>(iterations >> (8 * i)));
            auto tag = Bytes(box->AuthenticationTag);
            auto cipher = Bytes(box->EncryptedData);
            if (salt.size() != saltSize || nonce.size() != nonceSize || tag.size() != tagSize)
                throw ref new FailureException(L"vault: dimensioni inattese");
            blob.insert(blob.end(), salt.begin(), salt.end());
            blob.insert(blob.end(), nonce.begin(), nonce.end());
            blob.insert(blob.end(), tag.begin(), tag.end());
            blob.insert(blob.end(), cipher.begin(), cipher.end());
            return ToBuffer(blob.data(), blob.size());
        }).then([](IBuffer^ blob)
        {
            auto provider = ref new DataProtectionProvider(L"LOCAL=user");
            return create_task(provider->ProtectAsync(blob));
        }, task_continuation_context::use_arbitrary()).then([](IBuffer^ wrapped)
        {
            if (!WriteAll(Path(), Bytes(wrapped)))
                throw ref new FailureException(L"vault.bin non scritto");
        }, task_continuation_context::use_arbitrary());
    }

    task<std::shared_ptr<Credentials>> Open(const std::wstring& pin)
    {
        std::vector<unsigned char> file;
        if (!ReadAll(Path(), file))
            return task_from_result(std::shared_ptr<Credentials>());
        auto provider = ref new DataProtectionProvider();
        std::wstring secret = pin;
        return create_task(provider->UnprotectAsync(ToBuffer(file.data(), file.size()))).then(
            [secret](task<IBuffer^> unwrapped) -> std::shared_ptr<Credentials>
        {
            std::wstring pinText = secret;
            std::vector<unsigned char> blob;
            try
            {
                blob = Bytes(unwrapped.get());
            }
            catch (Exception^)
            {
                Wipe(pinText);
                return nullptr;
            }
            if (blob.size() < headerSize || memcmp(blob.data(), magic, 4) != 0)
            {
                Wipe(pinText);
                return nullptr;
            }
            unsigned int iterations = blob[4] | (blob[5] << 8) | (blob[6] << 16) | (static_cast<unsigned int>(blob[7]) << 24);
            const unsigned char* salt = blob.data() + 8;
            const unsigned char* nonce = salt + saltSize;
            const unsigned char* tag = nonce + nonceSize;
            const unsigned char* cipher = tag + tagSize;
            IBuffer^ plain = nullptr;
            try
            {
                auto key = Key(pinText, salt, iterations);
                plain = CryptographicEngine::DecryptAndAuthenticate(key, ToBuffer(cipher, blob.size() - headerSize),
                    ToBuffer(nonce, nonceSize), ToBuffer(tag, tagSize), nullptr);
            }
            catch (Exception^)
            {
                plain = nullptr; // wrong PIN: the tag does not match
            }
            Wipe(pinText);
            if (!plain)
                return nullptr;
            std::wstring text = CryptographicBuffer::ConvertBinaryToString(BinaryStringEncoding::Utf8, plain)->Data();
            auto credentials = std::make_shared<Credentials>();
            size_t newline = text.find(L'\n');
            credentials->user = text.substr(0, newline);
            if (newline != std::wstring::npos)
                credentials->password = text.substr(newline + 1);
            Wipe(text);
            return credentials;
        }, task_continuation_context::use_arbitrary());
    }
}
