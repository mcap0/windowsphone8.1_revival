#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Text.h"
#include <limits>

namespace
{
    bool DecodeWithCodePage(const unsigned char* bytes, int length, UINT codePage, DWORD flags, std::wstring& output)
    {
        if (length == 0)
        {
            output.clear();
            return true;
        }

        int count = MultiByteToWideChar(codePage, flags,
            reinterpret_cast<LPCCH>(bytes), length, nullptr, 0);
        if (count <= 0)
            return false;

        output.resize(count);
        return MultiByteToWideChar(codePage, flags,
            reinterpret_cast<LPCCH>(bytes), length, &output[0], count) == count;
    }

    bool EncodeWithCodePage(const std::wstring& input, UINT codePage, DWORD flags,
        std::vector<unsigned char>& output)
    {
        if (input.empty())
        {
            output.clear();
            return true;
        }

        BOOL usedDefault = FALSE;
        int count = WideCharToMultiByte(codePage, flags, input.data(),
            static_cast<int>(input.size()), nullptr, 0, "?", &usedDefault);
        if (count <= 0)
            return false;

        output.resize(count);
        usedDefault = FALSE;
        return WideCharToMultiByte(codePage, flags, input.data(),
            static_cast<int>(input.size()), reinterpret_cast<LPSTR>(&output[0]), count,
            "?", &usedDefault) == count;
    }

    std::wstring NormalizeLineEndings(const std::wstring& input, std::wstring& lineEnding)
    {
        bool hasCrLf = false;
        bool hasLf = false;
        for (size_t i = 0; i < input.size(); ++i)
        {
            if (input[i] == L'\r' && i + 1 < input.size() && input[i + 1] == L'\n')
                hasCrLf = true;
            else if (input[i] == L'\n')
                hasLf = true;
        }
        lineEnding = hasCrLf ? L"\r\n" : L"\n";

        std::wstring output;
        output.reserve(input.size());
        for (size_t i = 0; i < input.size(); ++i)
        {
            if (input[i] == L'\r')
            {
                if (i + 1 < input.size() && input[i + 1] == L'\n')
                    ++i;
                output.push_back(L'\n');
            }
            else
            {
                output.push_back(input[i]);
            }
        }
        return output;
    }

    std::wstring RestoreLineEndings(const std::wstring& input, const std::wstring& lineEnding)
    {
        std::wstring output;
        output.reserve(input.size() + input.size() / 8);
        for (size_t i = 0; i < input.size(); ++i)
        {
            if (input[i] == L'\r')
            {
                if (i + 1 < input.size() && input[i + 1] == L'\n')
                    ++i;
                output += lineEnding;
            }
            else if (input[i] == L'\n')
            {
                output += lineEnding;
            }
            else
            {
                output.push_back(input[i]);
            }
        }
        return output;
    }

    void AppendUtf16(const std::wstring& input, std::vector<unsigned char>& output)
    {
        output.reserve(input.size() * 2);
        for (size_t i = 0; i < input.size(); ++i)
        {
            unsigned int value = static_cast<unsigned int>(input[i]);
            output.push_back(static_cast<unsigned char>(value & 0xff));
            output.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
        }
    }
}

namespace TextCodec
{
    Document::Document()
        : encoding(Utf8), hasBom(false), lineEnding(L"\n")
    {
    }

    bool Decode(const std::vector<unsigned char>& bytes, Document& document, std::wstring& error)
    {
        error.clear();
        std::wstring decoded;
        size_t offset = 0;

        if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe)
        {
            document.encoding = Utf16LittleEndian;
            document.hasBom = true;
            offset = 2;
            size_t count = (bytes.size() - offset) / 2;
            decoded.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                unsigned int value = bytes[offset + i * 2] |
                    (static_cast<unsigned int>(bytes[offset + i * 2 + 1]) << 8);
                decoded.push_back(static_cast<wchar_t>(value));
            }
        }
        else
        {
            bool hasUtf8Bom = bytes.size() >= 3 &&
                bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
            offset = hasUtf8Bom ? 3 : 0;
            document.hasBom = hasUtf8Bom;

            if (DecodeWithCodePage(bytes.empty() ? nullptr : &bytes[offset],
                static_cast<int>(bytes.size() - offset), CP_UTF8, MB_ERR_INVALID_CHARS, decoded))
            {
                document.encoding = Utf8;
            }
            else
            {
                document.encoding = Windows1252;
                document.hasBom = false;
                offset = 0;
                if (!DecodeWithCodePage(bytes.empty() ? nullptr : &bytes[0],
                    static_cast<int>(bytes.size()), 1252, 0, decoded))
                {
                    error = L"impossibile decodificare il testo";
                    return false;
                }
            }
        }

        document.text = NormalizeLineEndings(decoded, document.lineEnding);
        return true;
    }

    bool Encode(const Document& document, std::vector<unsigned char>& bytes, std::wstring& error)
    {
        error.clear();
        bytes.clear();

        std::wstring lineEnding = document.lineEnding.empty() ? L"\n" : document.lineEnding;
        std::wstring restored = RestoreLineEndings(document.text, lineEnding);

        if (document.encoding == Utf16LittleEndian)
        {
            if (document.hasBom)
            {
                bytes.push_back(0xff);
                bytes.push_back(0xfe);
            }
            AppendUtf16(restored, bytes);
            return true;
        }

        if (document.encoding == Windows1252)
        {
            if (!EncodeWithCodePage(restored, 1252, WC_NO_BEST_FIT_CHARS, bytes))
            {
                error = L"impossibile codificare il testo in Windows-1252";
                return false;
            }
            return true;
        }

        if (!EncodeWithCodePage(restored, CP_UTF8, 0, bytes))
        {
            error = L"impossibile codificare il testo UTF-8";
            return false;
        }

        if (document.hasBom)
            bytes.insert(bytes.begin(), std::vector<unsigned char>{ 0xef, 0xbb, 0xbf }.begin(),
                std::vector<unsigned char>{ 0xef, 0xbb, 0xbf }.end());
        return true;
    }
}
