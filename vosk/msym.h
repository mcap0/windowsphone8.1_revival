// words.msym: a word symbol table that is memory-mapped, not parsed (PLAN.md §59.4). Reading the 200k words of
// Gr.fst's two symbol tables was the slowest part of loading the model on the phone.
// Layout (little-endian): MsymHeader, uint32 offset[nkeys] (into blob, kNone = no symbol), int32 order[n] (keys in
// table order), uint32 slot[nslots] (open addressing on Hash(word), key + 1, 0 = empty), blob (NUL-terminated words).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace fst { class SymbolTable; }
namespace msym {
struct Header {
    char magic[8];  // "VOSKSYM1"
    uint32_t n, nkeys, nslots, blob;
};
constexpr uint32_t kNone = 0xFFFFFFFFu;
inline uint32_t Hash(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}
// writes table as words.msym; false on error
bool Write(const fst::SymbolTable& table, const std::string& path);
// maps path; nullptr if missing or invalid. The table owns the mapping.
fst::SymbolTable* Map(const std::string& path);
}  // namespace msym
