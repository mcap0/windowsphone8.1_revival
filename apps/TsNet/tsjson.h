// Minimal JSON reader for the control server's answers (register, map responses).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;   // also the raw text of numbers (64-bit IDs)
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json& operator[](const char* k) const;
    const Json& operator[](size_t i) const;
    bool has(const char* k) const { return type == Obj && obj.count(k) > 0; }
    size_t size() const { return type == Arr ? arr.size() : type == Obj ? obj.size() : 0; }
    std::string s() const { return type == Str || type == Num ? str : std::string(); }
    long long i() const { return type == Num ? (long long)num : 0; }

    static bool parse(const char* p, size_t n, Json& out);
};

std::string jsonQuote(const std::string& s);

}  // namespace ts
