// mkhclg <graph dir> <out HCLG.fst> — the decoding graph Vosk builds on the fly (LookaheadComposeFst of HCLr.fst and
// Gr.fst, disambiguation transition-ids to epsilon, recognizer.cc) expanded once on the PC and written as an aligned
// ConstFst, so the phone maps it and the decoder does no composition (PLAN.md §59.5 step 1). Host tool.
//
// Layout: the decoder touches few states (5% over 100 commands), spread over the file. Tried renumbering (env
// MKHCLG_ORDER): by shortest distance from the start (graph cost + MKHCLG_FRAME per emitting arc) or depth-first;
// both touch more pages than the expansion order, which is kept.
// mkhclg --stats <HCLG.fst> <out.bin>: per state narcs (u32) and distance (f32), for the layout simulation.
#include <fst/fst.h>
#include <fst/vector-fst.h>
#include <fst/const-fst.h>
#include "fstext/fstext-utils.h"
#include "util/simple-io-funcs.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <numeric>
#include <queue>
#include <vector>
DECLARE_string(fst_read_mode);
static int Fail(const char* m, const std::string& f) { fprintf(stderr, "%s %s\n", m, f.c_str()); return 1; }
static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Dijkstra from the start; arc cost = weight + frame cost if it emits (ilabel != 0)
static std::vector<float> Distance(const fst::StdFst& f, int n, float frame) {
    std::vector<float> d(n, 1e30f);
    using Q = std::pair<float, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    d[f.Start()] = 0;
    q.push({0, f.Start()});
    while (!q.empty()) {
        auto [ds, s] = q.top();
        q.pop();
        if (ds > d[s]) continue;
        for (fst::ArcIterator<fst::StdFst> a(f, s); !a.Done(); a.Next()) {
            const auto& arc = a.Value();
            float nd = ds + std::max(0.f, arc.weight.Value()) + (arc.ilabel ? frame : 0.f);
            if (nd < d[arc.nextstate]) { d[arc.nextstate] = nd; q.push({nd, (int)arc.nextstate}); }
        }
    }
    return d;
}

// depth-first from the start, cheapest arc first: the states of a word path stay together. Returns the visit rank.
static std::vector<float> DfsRank(const fst::StdFst& f, int n) {
    std::vector<float> rank(n, 1e30f);
    std::vector<int> stack{(int)f.Start()};
    float next = 0;
    std::vector<std::pair<float, int>> kids;
    while (!stack.empty()) {
        int s = stack.back();
        stack.pop_back();
        if (rank[s] < 1e30f) continue;
        rank[s] = next++;
        kids.clear();
        for (fst::ArcIterator<fst::StdFst> a(f, s); !a.Done(); a.Next())
            if (rank[a.Value().nextstate] >= 1e30f) kids.push_back({a.Value().weight.Value(), (int)a.Value().nextstate});
        std::sort(kids.begin(), kids.end(), std::greater<>());  // cheapest pushed last = visited first
        for (auto& k : kids) stack.push_back(k.second);
    }
    return rank;
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) { fprintf(stderr, "usage: mkhclg <graph dir> <out HCLG.fst> | --stats <HCLG.fst> <out.bin>\n"); return 2; }
    const char* fe = getenv("MKHCLG_FRAME");
    float frame = fe ? atof(fe) : 1.0f;
    if (argc == 4) {
        FLAGS_fst_read_mode = "map";
        std::unique_ptr<fst::StdFst> f(fst::StdFst::Read(argv[2]));
        if (!f) return Fail("cannot read", argv[2]);
        int n = 0;
        for (fst::StateIterator<fst::StdFst> it(*f); !it.Done(); it.Next()) n++;
        std::vector<uint32_t> na(n);
        for (int s = 0; s < n; s++) na[s] = f->NumArcs(s);
        auto d = getenv("MKHCLG_ORDER") ? DfsRank(*f, n) : Distance(*f, n, frame);
        FILE* o = fopen(argv[3], "wb");
        fwrite(&n, 4, 1, o);
        fwrite(na.data(), 4, n, o);
        fwrite(d.data(), 4, n, o);
        fclose(o);
        return 0;
    }
    std::string dir = argv[1];
    double t0 = Now();
    std::unique_ptr<fst::StdFst> hcl(fst::StdFst::Read(dir + "/HCLr.fst"));
    std::unique_ptr<fst::StdFst> g(fst::StdFst::Read(dir + "/Gr.fst"));
    if (!hcl || !g) return Fail("cannot read HCLr.fst/Gr.fst in", dir);
    std::vector<int32_t> disambig;
    if (!kaldi::ReadIntegerVectorSimple(dir + "/disambig_tid.int", &disambig)) return Fail("cannot read", dir + "/disambig_tid.int");
    std::unique_ptr<fst::StdFst> la(fst::LookaheadComposeFst(*hcl, *g, disambig));
    fst::StdVectorFst v(*la);  // expands every reachable state
    const int n = v.NumStates();
    size_t arcs = 0;
    for (int s = 0; s < n; s++) arcs += v.NumArcs(s);
    printf("HCLG: %d states, %zu arcs, %.1f s\n", n, arcs, Now() - t0);

    // MKHCLG_ORDER=dist|dfs renumbers the states; by default the expansion order (breadth-first) stays: on the traces
    // of 100 commands it touches the fewest pages (2 commands: 24 MB, by distance 61 MB, depth-first 60 MB)
    const char* ord = getenv("MKHCLG_ORDER");
    if (ord) {
    auto d = std::string(ord) == "dfs" ? DfsRank(v, n) : Distance(v, n, frame);
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return d[a] < d[b]; });
    std::vector<int> newid(n);
    for (int i = 0; i < n; i++) newid[order[i]] = i;
    fst::StdVectorFst r;
    r.ReserveStates(n);
    for (int i = 0; i < n; i++) r.AddState();
    for (int i = 0; i < n; i++) {
        int s = order[i];
        r.SetFinal(i, v.Final(s));
        r.ReserveArcs(i, v.NumArcs(s));
        for (fst::ArcIterator<fst::StdVectorFst> a(v, s); !a.Done(); a.Next()) {
            fst::StdArc arc = a.Value();
            arc.nextstate = newid[arc.nextstate];
            r.AddArc(i, arc);
        }
    }
    r.SetStart(newid[v.Start()]);
    v = fst::StdVectorFst();
    printf("renumbered (%s, frame cost %.2f), start %d, %.1f s\n", ord, frame, r.Start(), Now() - t0);
    v = std::move(r);
    }
    fst::StdConstFst c(v);
    std::ofstream out(argv[2], std::ios::binary);
    fst::FstWriteOptions opts(argv[2]);
    opts.align = true;
    if (!c.Write(out, opts) || !out.flush()) return Fail("cannot write", argv[2]);
    return 0;
}
