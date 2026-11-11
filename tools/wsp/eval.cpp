// eval: a TSV of sentences (golden.tsv format) through Core/Wsp + Core/Command, as on the phone.
// g++ -std=c++11 -O2 -I../../apps/Assistente/Assistente/Core ../../apps/Assistente/Assistente/Core/{Text,Wsp,Command}.cpp \
//     eval.cpp -o eval && ./eval ../../apps/Assistente/Assistente/Assets/intent.wsp parafrasi.tsv [-v]
// Errors are split: "missed" (went online, the phone could have done it: costs a Gemini call) and "wrong" (did the
// wrong thing, or acted on something that had to go online: the bad kind).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Command.h"
#include "Wsp.h"

using namespace assist;

// a leading article does not count: "sulla ricetta" gives "ricetta", the label says "la ricetta"
static std::string Article(const std::string& s) {
    static const char* const a[] = {"il ", "lo ", "la ", "i ", "gli ", "le ", "l'", 0};
    for (int i = 0; a[i]; ++i)
        if (s.compare(0, std::strlen(a[i]), a[i]) == 0) return s.substr(std::strlen(a[i]));
    return s;
}

static std::string ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: eval intent.wsp sentences.tsv [-v]\n");
        return 2;
    }
    bool verbose = argc > 3 && std::strcmp(argv[3], "-v") == 0;
    Wsp net;
    std::string error;
    if (!net.Load(ReadFile(argv[1]), error)) {
        std::printf("%s: %s\n", argv[1], error.c_str());
        return 2;
    }
    const char* names[] = {"wi-fi", "bluetooth", "modalit\xc3\xa0 aereo", "rete cellulare", "posizione",
                           "risparmio batteria", "schermata di blocco", "rotazione schermo", "account email", "musica",
                           "orologio", "sveglie", "calendario", "meteo", "telegram", "posta", "registratore", "file",
                           "perle tv", "authenticator", "appunti", "esplora"};
    std::vector<std::string> targets(names, names + sizeof(names) / sizeof(names[0]));
    LocalTime now = {2026, 11, 3, 10, 0, 0, 2};
    std::istringstream in(ReadFile(argv[2]));
    std::string line;
    int rows = 0, good = 0, missed = 0, wrong = 0, badArgs = 0;
    std::map<std::string, std::pair<int, int> > perIntent;  // want -> (rows, good)
    std::vector<std::string> report;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> col;
        std::stringstream cs(line);
        std::string part;
        while (std::getline(cs, part, '\t')) col.push_back(part);
        if (col.size() < 2) continue;
        ++rows;
        WspResult r;
        Command c;
        bool ok = Understand(net, col[0], now, targets, c, r);
        std::string got = ok ? c.name : "online";
        // "a|b": any of them is right (lunghe.tsv: MASSIVE labels that are more than one of ours)
        if (col[1].find('|') != std::string::npos) {
            std::string alts = "|" + col[1] + "|";
            std::string first = col[1].substr(0, col[1].find('|'));
            col[1] = alts.find("|" + got + "|") != std::string::npos ? got : first;
        }
        std::string detail;
        bool argsOk = true;
        if (got == col[1] && col.size() > 2) {
            std::stringstream as(col[2]);
            std::string kv;
            while (std::getline(as, kv, ';')) {
                size_t eq = kv.find('=');
                std::string k = kv.substr(0, eq), want = kv.substr(eq + 1), have = c.Arg(k);
                if (Article(Wsp::Fold(have)) != Article(Wsp::Fold(want))) {
                    argsOk = false;
                    detail += " " + k + "=\"" + have + "\" (atteso \"" + want + "\")";
                }
            }
        }
        perIntent[col[1]].first++;
        const char* kind;
        if (got == col[1] && argsOk) {
            ++good;
            perIntent[col[1]].second++;
            kind = 0;
        } else if (got == col[1]) {
            ++badArgs;
            kind = "ARGOMENTI";
        } else if (got == "online") {
            ++missed;
            kind = "online   ";
        } else {
            ++wrong;
            kind = "SBAGLIATO";
        }
        if (kind || verbose) {
            char b[512];
            std::snprintf(b, sizeof(b), "%s \"%s\" -> %s%s (atteso %s; rete %s %.2f%s%s)", kind ? kind : "ok       ",
                          col[0].c_str(), got.c_str(), detail.c_str(), col[1].c_str(), r.best.c_str(), r.margin,
                          c.why.empty() ? "" : ", manca ", c.why.c_str());
            report.push_back(b);
        }
    }
    for (size_t i = 0; i < report.size(); ++i) std::printf("%s\n", report[i].c_str());
    std::printf("\n%-18s %s\n", "intento", "giuste");
    for (std::map<std::string, std::pair<int, int> >::const_iterator it = perIntent.begin(); it != perIntent.end(); ++it)
        std::printf("%-18s %d/%d\n", it->first.c_str(), it->second.second, it->second.first);
    std::printf("\ntotale %d/%d giuste (%.0f%%): %d online inutilmente, %d argomenti sbagliati, %d azioni sbagliate\n",
                good, rows, 100.0 * good / rows, missed, badArgs, wrong);
    return 0;
}
