#include "tsjson.h"
#include "tscrypto.h"   // snprintf on VS2013

#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace ts {

static const Json kNull;

const Json& Json::operator[](const char* k) const {
    if (type != Obj) return kNull;
    std::map<std::string, Json>::const_iterator it = obj.find(k);
    return it == obj.end() ? kNull : it->second;
}

const Json& Json::operator[](size_t i) const { return type == Arr && i < arr.size() ? arr[i] : kNull; }

namespace {
struct Parser {
    const char *p, *e;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool lit(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(e - p) < n || memcmp(p, s, n)) return false;
        p += n;
        return true;
    }
    static void utf8(std::string& s, unsigned c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | c >> 6); s += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += (char)(0xE0 | c >> 12); s += (char)(0x80 | (c >> 6 & 63)); s += (char)(0x80 | (c & 63)); }
        else { s += (char)(0xF0 | c >> 18); s += (char)(0x80 | (c >> 12 & 63)); s += (char)(0x80 | (c >> 6 & 63)); s += (char)(0x80 | (c & 63)); }
    }
    bool hex4(unsigned& c) {
        if (e - p < 4) return false;
        char t[5] = {p[0], p[1], p[2], p[3], 0};
        char* end;
        c = (unsigned)strtoul(t, &end, 16);
        if (end != t + 4) return false;
        p += 4;
        return true;
    }
    bool str(std::string& s) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') { s += *p++; continue; }
            if (++p >= e) return false;
            char c = *p++;
            switch (c) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u': {
                unsigned u;
                if (!hex4(u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(s, u);
                break;
            }
            default: s += c;
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool val(Json& v, int depth) {
        if (depth > 64) return false;
        ws();
        if (p >= e) return false;
        if (*p == '{') {
            v.type = Json::Obj;
            p++;
            ws();
            if (p < e && *p == '}') { p++; return true; }
            while (true) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= e || *p++ != ':') return false;
                if (!val(v.obj[k], depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.type = Json::Arr;
            p++;
            ws();
            if (p < e && *p == ']') { p++; return true; }
            while (true) {
                v.arr.push_back(Json());
                if (!val(v.arr.back(), depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.type = Json::Str; return str(v.str); }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Json::Bool; return true; }
        if (lit("null")) return true;
        const char* s = p;
        while (p < e && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) p++;
        if (p == s) return false;
        v.type = Json::Num;
        v.str.assign(s, p);
        v.num = strtod(v.str.c_str(), nullptr);
        return true;
    }
};
}  // namespace

bool Json::parse(const char* p, size_t n, Json& out) {
    Parser ps = {p, p + n};
    out = Json();
    return ps.val(out, 0);
}

std::string jsonQuote(const std::string& s) {
    std::string r = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            r += b;
        } else r += (char)c;
    }
    return r + "\"";
}

}  // namespace ts
