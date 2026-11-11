#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>

namespace TextCodec
{
    enum Encoding
    {
        Utf8 = 0,
        Utf16LittleEndian = 1,
        Windows1252 = 2
    };

    struct Document
    {
        std::wstring text;
        Encoding encoding;
        bool hasBom;
        std::wstring lineEnding;

        Document();
    };

    bool Decode(const std::vector<unsigned char>& bytes, Document& document, std::wstring& error);
    bool Encode(const Document& document, std::vector<unsigned char>& bytes, std::wstring& error);
}
