#include "msym.h"
#include <fst/mapped-file.h>
#include <fst/symbol-table.h>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace msym {
namespace {
class Impl final : public fst::internal::ConstSymbolTableImpl {
  public:
    Impl(fst::MappedFile* file, const std::string& name) : file_(file), name_(name) {
        const char* p = static_cast<const char*>(file_->data());
        h_ = reinterpret_cast<const Header*>(p);
        offset_ = reinterpret_cast<const uint32_t*>(p + sizeof(Header));
        order_ = reinterpret_cast<const int32_t*>(offset_ + h_->nkeys);
        slot_ = reinterpret_cast<const uint32_t*>(order_ + h_->n);
        blob_ = reinterpret_cast<const char*>(slot_ + h_->nslots);
    }
    bool Write(std::ostream&) const override { return false; }
    std::string Find(int64_t key) const override {
        const char* s = Word(key);
        return s ? std::string(s) : std::string();
    }
    int64_t Find(SymbolType symbol) const override {
        std::string_view w(symbol);
        const uint32_t mask = h_->nslots - 1;
        for (uint32_t i = Hash(w) & mask;; i = (i + 1) & mask) {
            const uint32_t k = slot_[i];
            if (!k) return fst::kNoSymbol;
            const char* s = Word(k - 1);
            if (s && w == s) return k - 1;
        }
    }
    bool Member(int64_t key) const override { return Word(key) != nullptr; }
    int64_t GetNthKey(ssize_t pos) const override {
        return pos >= 0 && (size_t)pos < h_->n ? order_[pos] : fst::kNoSymbol;
    }
    const std::string& Name() const override { return name_; }
    const std::string& CheckSum() const override { return empty_; }
    const std::string& LabeledCheckSum() const override { return empty_; }
    int64_t AvailableKey() const override { return h_->nkeys; }
    size_t NumSymbols() const override { return h_->n; }

  private:
    const char* Word(int64_t key) const {
        if (key < 0 || (uint64_t)key >= h_->nkeys || offset_[key] == kNone) return nullptr;
        return blob_ + offset_[key];
    }
    std::unique_ptr<fst::MappedFile> file_;
    std::string name_, empty_;
    const Header* h_;
    const uint32_t* offset_;
    const int32_t* order_;
    const uint32_t* slot_;
    const char* blob_;
};

class Table : public fst::SymbolTable {
  public:
    explicit Table(std::shared_ptr<fst::internal::SymbolTableImplBase> impl) : fst::SymbolTable(std::move(impl)) {}
};
}  // namespace

bool Write(const fst::SymbolTable& table, const std::string& path) {
    Header h;
    memcpy(h.magic, "VOSKSYM1", 8);
    h.n = (uint32_t)table.NumSymbols();
    int64_t maxkey = -1;
    std::vector<int32_t> order;
    for (const auto& it : table) {
        order.push_back((int32_t)it.Label());
        if (it.Label() > maxkey) maxkey = it.Label();
    }
    if (order.size() != h.n || maxkey < 0 || maxkey >= INT32_MAX) return false;
    h.nkeys = (uint32_t)maxkey + 1;
    h.nslots = 1;
    while (h.nslots < h.n * 2) h.nslots <<= 1;
    std::vector<uint32_t> offset(h.nkeys, kNone), slot(h.nslots, 0);
    std::string blob;
    for (const auto& it : table) {
        offset[it.Label()] = (uint32_t)blob.size();
        blob += it.Symbol();
        blob.push_back(0);
        for (uint32_t i = Hash(it.Symbol()) & (h.nslots - 1);; i = (i + 1) & (h.nslots - 1))
            if (!slot[i]) { slot[i] = (uint32_t)it.Label() + 1; break; }
    }
    h.blob = (uint32_t)blob.size();
    std::ofstream o(path, std::ios::binary);
    o.write((const char*)&h, sizeof h);
    o.write((const char*)offset.data(), offset.size() * 4);
    o.write((const char*)order.data(), order.size() * 4);
    o.write((const char*)slot.data(), slot.size() * 4);
    o.write(blob.data(), blob.size());
    return (bool)o.flush();
}

fst::SymbolTable* Map(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    in.seekg(0, std::ios::end);
    const size_t size = (size_t)in.tellg();
    in.seekg(0);
    if (size < sizeof(Header)) return nullptr;
    std::unique_ptr<fst::MappedFile> file(fst::MappedFile::Map(&in, true, path, size));
    if (!file) return nullptr;
    Header h;
    memcpy(&h, file->data(), sizeof h);
    const uint64_t need = sizeof h + 4ull * h.nkeys + 4ull * h.n + 4ull * h.nslots + h.blob;
    if (memcmp(h.magic, "VOSKSYM1", 8) || need != size || !h.nslots || (h.nslots & (h.nslots - 1))) return nullptr;
    return new Table(std::make_shared<Impl>(file.release(), path));
}
}  // namespace msym
