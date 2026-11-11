// fstalign in.fst out.fst — rewrites an FST (olabel_lookahead, ngram) with aligned sections, so the phone can
// memory-map it (fst_read_mode=map) instead of reading it into memory. Host tool.
// ngram: byte copy of the data after a header without symbol tables (IS_ALIGNED, padding to 16), words to words.msym
// next to out.fst (re-serialising through OpenFst wrote a symbol table it could not read back). Others: OpenFst Write
// with opts.align.
#include <fst/fst.h>
#include "msym.h"
#include <fst/symbol-table.h>
#include <fst/mapped-file.h>
#include <sstream>
#include <fstream>
#include <iterator>
#include <memory>
#include <cstdio>
#include <cstring>
static int Fail(const char* m, const char* f) { fprintf(stderr, "%s %s\n", m, f); return 1; }
int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: fstalign in.fst out.fst\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    fst::FstHeader hdr;
    if (!hdr.Read(in, argv[1])) return Fail("cannot read header of", argv[1]);
    printf("%s: %s, flags %d\n", argv[1], hdr.FstType().c_str(), hdr.GetFlags());
    if (hdr.FstType() == "ngram") {
        // the symbol tables (2 x 200k words, parsed and hashed at every load) go to words.msym, mapped by Vosk
        if (hdr.GetFlags() & fst::FstHeader::IS_ALIGNED) return Fail("already aligned:", argv[1]);
        std::unique_ptr<fst::SymbolTable> isyms, osyms;
        if (hdr.GetFlags() & fst::FstHeader::HAS_ISYMBOLS) isyms.reset(fst::SymbolTable::Read(in, fst::SymbolTableReadOptions()));
        if (hdr.GetFlags() & fst::FstHeader::HAS_OSYMBOLS) osyms.reset(fst::SymbolTable::Read(in, fst::SymbolTableReadOptions()));
        if (!osyms) return Fail("no output symbols in", argv[1]);
        std::string dir = argv[2];
        dir = dir.find('/') == std::string::npos ? "." : dir.substr(0, dir.rfind('/'));
        if (!msym::Write(*osyms, dir + "/words.msym")) return Fail("cannot write words.msym in", dir.c_str());
        const size_t head = in.tellg();
        in.seekg(0);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        hdr.SetFlags((hdr.GetFlags() & ~(fst::FstHeader::HAS_ISYMBOLS | fst::FstHeader::HAS_OSYMBOLS)) |
                     fst::FstHeader::IS_ALIGNED);
        std::ostringstream h;
        hdr.Write(h, argv[2]);
        std::string out = h.str();
        while (out.size() % fst::MappedFile::kArchAlignment) out.push_back(0);
        out.append(all, head, std::string::npos);
        std::ofstream o(argv[2], std::ios::binary);
        o.write(out.data(), out.size());
        if (!o.flush()) return Fail("cannot write", argv[2]);
        // check: every word maps back
        std::unique_ptr<fst::SymbolTable> m(msym::Map(dir + "/words.msym"));
        if (!m || m->NumSymbols() != osyms->NumSymbols()) return Fail("bad words.msym in", dir.c_str());
        for (const auto& it : *osyms)
            if (m->Find(it.Label()) != it.Symbol() || m->Find(it.Symbol()) != it.Label()) return Fail("words.msym differs at", it.Symbol().c_str());
        if (m->Find("parolachenonce") != fst::kNoSymbol) return Fail("words.msym finds a missing word", "");
        printf("words.msym: %zu words, same as the output symbols\n", (size_t)m->NumSymbols());
    } else {
        in.close();
        std::unique_ptr<fst::StdFst> f(fst::StdFst::Read(argv[1]));
        if (!f) return Fail("cannot read", argv[1]);
        std::ofstream out(argv[2], std::ios::binary);
        fst::FstWriteOptions opts(argv[2]);
        opts.align = true;
        if (!f->Write(out, opts) || !out.flush()) return Fail("cannot write", argv[2]);
    }
    std::unique_ptr<fst::StdFst> back(fst::StdFst::Read(argv[2]));
    if (!back) return Fail("cannot read back", argv[2]);
    printf("%s: %s, aligned, %d states\n", argv[2], back->Type().c_str(), (int)back->Start());
    return 0;
}
