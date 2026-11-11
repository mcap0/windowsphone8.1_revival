#pragma once

#include <string>
#include <vector>
#include <cctype>

namespace CercaCore
{
    inline std::string UrlEncode(const std::string& value)
    {
        static const char hex[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(value.size() * 3);

        for (size_t i = 0; i < value.size(); ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(value[i]);
            if ((ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~')
            {
                encoded.push_back(static_cast<char>(ch));
            }
            else
            {
                encoded.push_back('%');
                encoded.push_back(hex[(ch >> 4) & 0x0F]);
                encoded.push_back(hex[ch & 0x0F]);
            }
        }

        return encoded;
    }

    inline bool StartsWithHttp(const std::string& value)
    {
        if (value.size() < 4)
            return false;

        return (value[0] == 'h' || value[0] == 'H') &&
               (value[1] == 't' || value[1] == 'T') &&
               (value[2] == 't' || value[2] == 'T') &&
               (value[3] == 'p' || value[3] == 'P');
    }

    inline bool LooksLikeUrl(const std::string& value)
    {
        if (StartsWithHttp(value))
            return true;

        bool hasDot = false;
        for (size_t i = 0; i < value.size(); ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(value[i]);
            if (ch == '.')
                hasDot = true;
            if (std::isspace(ch))
                return false;
        }

        return hasDot;
    }

    class JsonReader
    {
    public:
        explicit JsonReader(const std::string& json)
            : _json(json), _position(0)
        {
        }

        std::vector<std::string> Parse()
        {
            std::vector<std::string> suggestions;
            if (!Consume('{'))
                return suggestions;

            while (true)
            {
                SkipWhitespace();
                if (Consume('}'))
                    break;

                std::string key;
                if (!ParseString(key) || !Consume(':'))
                    return std::vector<std::string>();

                if (key == "AS")
                {
                    if (!ParseAs(suggestions))
                        return std::vector<std::string>();
                }
                else if (!SkipValue())
                {
                    return std::vector<std::string>();
                }

                SkipWhitespace();
                if (Consume('}'))
                    break;
                if (!Consume(','))
                    return std::vector<std::string>();
            }

            return suggestions;
        }

    private:
        const std::string& _json;
        size_t _position;

        void SkipWhitespace()
        {
            while (_position < _json.size() &&
                   (_json[_position] == ' ' || _json[_position] == '\t' ||
                    _json[_position] == '\r' || _json[_position] == '\n'))
                ++_position;
        }

        bool Consume(char expected)
        {
            SkipWhitespace();
            if (_position >= _json.size() || _json[_position] != expected)
                return false;
            ++_position;
            return true;
        }

        static void AppendUtf8(unsigned int codePoint, std::string& output)
        {
            if (codePoint <= 0x7F)
            {
                output.push_back(static_cast<char>(codePoint));
            }
            else if (codePoint <= 0x7FF)
            {
                output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else if (codePoint <= 0xFFFF)
            {
                output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else
            {
                output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        }

        bool ParseHexUnit(unsigned int& value)
        {
            if (_position + 4 > _json.size())
                return false;

            value = 0;
            for (unsigned int i = 0; i < 4; ++i)
            {
                const char ch = _json[_position++];
                value <<= 4;
                if (ch >= '0' && ch <= '9')
                    value |= static_cast<unsigned int>(ch - '0');
                else if (ch >= 'a' && ch <= 'f')
                    value |= static_cast<unsigned int>(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F')
                    value |= static_cast<unsigned int>(ch - 'A' + 10);
                else
                    return false;
            }
            return true;
        }

        bool ParseString(std::string& output)
        {
            if (!Consume('"'))
                return false;

            output.clear();
            while (_position < _json.size())
            {
                const unsigned char ch = static_cast<unsigned char>(_json[_position++]);
                if (ch == '"')
                    return true;

                if (ch != '\\')
                {
                    output.push_back(static_cast<char>(ch));
                    continue;
                }

                if (_position >= _json.size())
                    return false;

                const char escaped = _json[_position++];
                switch (escaped)
                {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u':
                    {
                        unsigned int first = 0;
                        if (!ParseHexUnit(first))
                            return false;

                        unsigned int codePoint = first;
                        if (first >= 0xD800 && first <= 0xDBFF &&
                            _position + 6 <= _json.size() &&
                            _json[_position] == '\\' && _json[_position + 1] == 'u')
                        {
                            _position += 2;
                            unsigned int second = 0;
                            if (!ParseHexUnit(second))
                                return false;
                            if (second >= 0xDC00 && second <= 0xDFFF)
                                codePoint = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                        }

                        AppendUtf8(codePoint, output);
                        break;
                    }
                default:
                    return false;
                }
            }

            return false;
        }

        bool SkipValue()
        {
            SkipWhitespace();
            if (_position >= _json.size())
                return false;

            if (_json[_position] == '"')
            {
                std::string ignored;
                return ParseString(ignored);
            }

            if (_json[_position] == '{')
            {
                ++_position;
                SkipWhitespace();
                if (Consume('}'))
                    return true;

                while (true)
                {
                    std::string key;
                    if (!ParseString(key) || !Consume(':') || !SkipValue())
                        return false;
                    if (Consume('}'))
                        return true;
                    if (!Consume(','))
                        return false;
                }
            }

            if (_json[_position] == '[')
            {
                ++_position;
                SkipWhitespace();
                if (Consume(']'))
                    return true;

                while (true)
                {
                    if (!SkipValue())
                        return false;
                    if (Consume(']'))
                        return true;
                    if (!Consume(','))
                        return false;
                }
            }

            const size_t start = _position;
            while (_position < _json.size() &&
                   _json[_position] != ',' && _json[_position] != ']' &&
                   _json[_position] != '}' && _json[_position] != ' ' &&
                   _json[_position] != '\t' && _json[_position] != '\r' &&
                   _json[_position] != '\n')
                ++_position;

            return _position > start;
        }

        bool ParseAs(std::vector<std::string>& suggestions)
        {
            if (!Consume('{'))
                return SkipValue();

            while (true)
            {
                SkipWhitespace();
                if (Consume('}'))
                    return true;

                std::string key;
                if (!ParseString(key) || !Consume(':'))
                    return false;

                if (key == "Results")
                {
                    if (!ParseResults(suggestions))
                        return false;
                }
                else if (!SkipValue())
                {
                    return false;
                }

                if (Consume('}'))
                    return true;
                if (!Consume(','))
                    return false;
            }
        }

        bool ParseResults(std::vector<std::string>& suggestions)
        {
            if (!Consume('['))
                return SkipValue();

            while (true)
            {
                SkipWhitespace();
                if (Consume(']'))
                    return true;

                if (_position < _json.size() && _json[_position] == '{')
                {
                    if (!ParseResult(suggestions))
                        return false;
                }
                else if (!SkipValue())
                {
                    return false;
                }

                if (Consume(']'))
                    return true;
                if (!Consume(','))
                    return false;
            }
        }

        bool ParseResult(std::vector<std::string>& suggestions)
        {
            if (!Consume('{'))
                return false;

            while (true)
            {
                SkipWhitespace();
                if (Consume('}'))
                    return true;

                std::string key;
                if (!ParseString(key) || !Consume(':'))
                    return false;

                if (key == "Suggests")
                {
                    if (!ParseSuggests(suggestions))
                        return false;
                }
                else if (!SkipValue())
                {
                    return false;
                }

                if (Consume('}'))
                    return true;
                if (!Consume(','))
                    return false;
            }
        }

        bool ParseSuggests(std::vector<std::string>& suggestions)
        {
            if (!Consume('['))
                return SkipValue();

            while (true)
            {
                SkipWhitespace();
                if (Consume(']'))
                    return true;

                if (_position < _json.size() && _json[_position] == '{')
                {
                    if (!ParseSuggestion(suggestions))
                        return false;
                }
                else if (!SkipValue())
                {
                    return false;
                }

                if (Consume(']'))
                    return true;
                if (!Consume(','))
                    return false;
            }
        }

        bool ParseSuggestion(std::vector<std::string>& suggestions)
        {
            if (!Consume('{'))
                return false;

            std::string text;
            while (true)
            {
                SkipWhitespace();
                if (Consume('}'))
                {
                    if (!text.empty())
                        suggestions.push_back(text);
                    return true;
                }

                std::string key;
                if (!ParseString(key) || !Consume(':'))
                    return false;

                if (key == "Txt" && _position < _json.size() && _json[_position] == '"')
                {
                    if (!ParseString(text))
                        return false;
                }
                else if (!SkipValue())
                {
                    return false;
                }

                if (Consume('}'))
                {
                    if (!text.empty())
                        suggestions.push_back(text);
                    return true;
                }
                if (!Consume(','))
                    return false;
            }
        }
    };

    inline std::vector<std::string> ParseSuggestions(const std::string& json)
    {
        JsonReader reader(json);
        return reader.Parse();
    }
}
