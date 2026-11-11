#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "Command.h"

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

#include <cstdio>
#include <cstring>

namespace assist {

namespace {

typedef std::vector<WspToken> Tokens;
void TrimTail(const Wsp& net, Tokens& t);

bool Is(const WspToken& t, const char* folded) { return t.folded == folded; }

bool IsAny(const WspToken& t, const char* const* list) {
    for (; *list; ++list)
        if (t.folded == *list) return true;
    return false;
}

std::string Two(int n) {
    char b[8];
    snprintf(b, sizeof(b), "%02d", n);
    return b;
}

std::string DateText(long long days) {
    int y = 0, m = 0, d = 0;
    CivilFromDays(days, y, m, d);
    char b[16];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
    return b;
}

std::string Original(const WspResult& r, size_t first, size_t last) {
    if (first > last || last >= r.tokens.size()) return std::string();
    return r.text.substr(r.tokens[first].begin, r.tokens[last].end - r.tokens[first].begin);
}

bool Comma(const WspResult& r, size_t i) {  // a comma between word i and word i + 1
    return r.text.find_first_of(",;", r.tokens[i].end) < r.tokens[i + 1].begin;
}

// the end of a name or title that starts at word a: up to the first closing word ("mamma LO PUOI FARE", "Vasco
// GRAZIE", "Catania PORCA MISERIA", "Giuseppe CHE gli devo dire") or comma, then without the small words left at
// its end ("mamma lo", "Queen per")
size_t NameEnd(const Wsp& net, const WspResult& r, size_t a) {
    size_t b = a;
    while (b + 1 < r.tokens.size() && !Comma(r, b) && !net.Has(r.tokens[b + 1], "coda")) ++b;
    static const char* const small[] = {"per", "lo", "la", "il", "le", "li", "gli", "e", "o", "di", "a", "mi", "ti",
                                        "ci", "ma", "se", "che", "me", "te", "in", 0};  // "pink floyd IN sottofondo"
    while (b > a && (net.Has(r.tokens[b], "coda") || IsAny(r.tokens[b], small))) --b;
    return b;
}

bool Connector(const WspToken& t) {  // inside a place name: "Reggio DI Calabria", "San Giovanni LA Punta"
    static const char* const w[] = {"di", "del", "della", "dei", "de", "la", "lo", "le", "d", "sul", "sull", 0};
    return IsAny(t, w);
}

// the words tagged as a span (name, city, label): the first run of them, as written, with the small words between
// ("Reggio di Calabria"), never across a comma ("a Catania, devo uscire in moto"); "palermo PORCA MISERIA" and
// "catania per favore LA SPECIFICA" end at the city. toEnd: from the first to its NameEnd (titles and artists have
// known words in them: "Lucio Dalla", "La voce del padrone")
std::string Span(const Wsp& net, const WspResult& r, bool toEnd = false) {
    const size_t n = r.tokens.size();
    size_t first = 0;
    while (first < n && net.Tag(r.tokens[first], "span") < 0.5f) ++first;
    if (first == n) return std::string();
    size_t last = first;
    if (toEnd) {
        last = NameEnd(net, r, first);
    } else {
        for (;;) {
            if (last + 1 < n && !Comma(r, last) && net.Tag(r.tokens[last + 1], "span") >= 0.5f) ++last;
            else if (last + 2 < n && !Comma(r, last) && !Comma(r, last + 1) && Connector(r.tokens[last + 1]) &&
                     net.Tag(r.tokens[last + 2], "span") >= 0.5f) last += 2;
            else break;
        }
    }
    return Original(r, first, last);
}

// index of the first word with the feature, or -1
int First(const Wsp& net, const Tokens& t, const char* feature) {
    for (size_t i = 0; i < t.size(); ++i)
        if (net.Has(t[i], feature)) return static_cast<int>(i);
    return -1;
}

int UnitSeconds(const WspToken& t) {
    if (Is(t, "secondi") || Is(t, "secondo") || Is(t, "sec") || Is(t, "s")) return 1;
    if (Is(t, "minuti") || Is(t, "minuto") || Is(t, "min") || Is(t, "m")) return 60;
    if (Is(t, "ore") || Is(t, "ora") || Is(t, "h")) return 3600;
    return 0;
}

bool IsHalf(const WspToken& t) { return Is(t, "mezza") || Is(t, "mezzo") || Is(t, "mezz"); }

// "5 minuti", "tre minuti e trenta secondi", "mezz'ora", "un'ora e mezza", "un quarto d'ora". Two durations
// apart are two things: "la pasta ci mette 11 minuti, fammi un timer di 11 minuti" = the last one.
int Duration(const Wsp& net, const Tokens& t) {
    int total = 0;
    size_t lastQty = t.size();
    for (size_t i = 0; i < t.size(); ++i) {
        if (net.Tag(t[i], "qty") < 0.5f) continue;
        if (lastQty < t.size() && i > lastQty + 4) total = 0;  // "3 minuti e 30 secondi" stays one
        lastQty = i;
        if (t[i].word == "<num>" && i + 1 < t.size()) {
            int unit = UnitSeconds(t[i + 1]);
            total += t[i].num * unit;
            if (i + 3 < t.size() && Is(t[i + 2], "e") && IsHalf(t[i + 3])) total += unit / 2;
        } else if (IsHalf(t[i]) && i + 1 < t.size()) {
            total += UnitSeconds(t[i + 1]) / 2;
        } else if (UnitSeconds(t[i]) > 0) {  // tagged on "ora" of "un quarto d'ora"
            total += UnitSeconds(t[i]) / 4;
        }
    }
    return total;
}

struct Day {
    int offset;     // days from today, -1 = no day said
    bool weekday;   // a weekday name: next week if that moment is already past
    bool week;      // "settimana"
    bool weekend;
    bool evening;   // stasera
    bool night;     // stanotte
};

int Month(const WspToken& t) {
    static const char* const m[] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto",
                                    "settembre", "ottobre", "novembre", "dicembre", 0};
    for (int i = 0; m[i]; ++i)
        if (t.folded == m[i]) return i + 1;
    return 0;
}

// "il 20 dicembre", "il giorno undici novembre", "l'11 novembre 2027", "primo maggio": days from today (the next
// one: a date already past is next year), -1 if none. used: the date's words, so no title or text takes them.
int FindDate(const Tokens& t, const LocalTime& now, std::vector<bool>* used) {
    long long today = DaysFromCivil(now.year, now.month, now.day);
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        int dd = t[i].word == "<num>" ? t[i].num : Is(t[i], "primo") ? 1 : 0;
        int mo = Month(t[i + 1]);
        if (dd < 1 || dd > 31 || mo == 0) continue;
        size_t end = i + 2;
        int y = now.year;
        bool year = end < t.size() && t[end].word == "<num>" && t[end].num >= 2000 && t[end].num < 2100;
        if (year) { y = t[end].num; ++end; }
        long long target = DaysFromCivil(y, mo, dd);
        if (!year && target < today) target = DaysFromCivil(y + 1, mo, dd);
        if (target < today) continue;
        if (used) {
            size_t a = i;
            if (a > 0 && Is(t[a - 1], "giorno")) --a;
            static const char* const art[] = {"il", "l", "del", "dell", "per", 0};
            if (a > 0 && IsAny(t[a - 1], art)) --a;
            for (size_t k = a; k < end; ++k) (*used)[k] = true;
        }
        return static_cast<int>(target - today);
    }
    return -1;
}

Day FindDay(const Wsp& net, const Tokens& t, const LocalTime& now, std::vector<bool>* used) {
    static const char* const names[] = {"domenica", "lunedi", "martedi", "mercoledi", "giovedi", "venerdi", "sabato"};
    Day d = {-1, false, false, false, false, false};
    int date = FindDate(t, now, used);
    if (date >= 0) {  // a date said in full wins over "domani", and is no weekday ("giovedì 11 novembre")
        d.offset = date;
        return d;
    }
    for (size_t i = 0; i < t.size(); ++i) {
        if (!net.Has(t[i], "day")) continue;
        const std::string& w = t[i].folded;
        int off = -1;
        if (w == "oggi" || w == "stamattina") off = 0;
        else if (w == "stasera") { off = 0; d.evening = true; }
        else if (w == "stanotte") { off = 0; d.night = true; }
        else if (w == "domani" || w == "domattina") off = 1;
        else if (w == "dopodomani") off = 2;
        else if (w == "weekend") { off = (6 - now.weekday + 7) % 7; d.weekend = true; }
        else if (w == "settimana") {
            d.week = true;
            bool next = i > 0 && (Is(t[i - 1], "prossima") || Is(t[i - 1], "prossimi"));
            bool weekend = i > 0 && Is(t[i - 1], "fine");
            if (weekend) { d.week = false; d.weekend = true; off = (6 - now.weekday + 7) % 7; }
            else if (next) { off = (1 - now.weekday + 7) % 7; if (off == 0) off = 7; }
            else off = 0;
        } else {
            for (int k = 0; k < 7; ++k)
                if (w == names[k]) { off = (k - now.weekday + 7) % 7; d.weekday = true; }
        }
        if (off >= 0 && d.offset < 0) d.offset = off;
    }
    // "tra due giorni"
    for (size_t i = 0; i + 2 < t.size() && d.offset < 0; ++i)
        if ((Is(t[i], "tra") || Is(t[i], "fra")) && t[i + 1].word == "<num>" && (Is(t[i + 2], "giorni") || Is(t[i + 2], "giorno")))
            d.offset = t[i + 1].num;
    return d;
}

bool HasWord(const Tokens& t, const char* w) {
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].folded == w) return true;
    return false;
}

// the hour said: "alle 7", "alle 7 e mezza", "alle otto meno un quarto", "7:45", "alle 9 di sera"
// a number of minutes from word j: "25", "venticinque", "venti cinque" (Vosk splits them), "zero zero", "zero
// cinque"; the words taken in n. Not a quantity: "alle 9 e 15 minuti di pausa" is not 9:15.
bool Minutes(const Tokens& t, size_t j, bool afterE, int& minute, size_t& n) {
    if (j >= t.size() || t[j].word != "<num>") return false;
    int v = t[j].num;
    n = 1;
    bool unitNext = false;
    if (v == 0) {
        if (j + 1 < t.size() && t[j + 1].word == "<num>" && t[j + 1].num < 10) { v = t[j + 1].num; n = 2; }
        else if (!afterE) return false;
    } else if (v % 10 == 0 && v >= 20 && v <= 50 && j + 1 < t.size() && t[j + 1].word == "<num>" &&
               t[j + 1].num >= 1 && t[j + 1].num <= 9) {
        v += t[j + 1].num;
        n = 2;
    } else if (v >= 60 || (!afterE && v < 10)) {
        return false;  // "alle nove cinque" is no time a person says; "alle 9 e 5" is
    }
    unitNext = j + n < t.size() && UnitSeconds(t[j + n]) > 0;
    if (unitNext) return false;
    minute = v;
    return true;
}

bool FindTime(const Wsp& net, const Tokens& t, const Day& day, int& hour, int& minute, bool& part,
              std::vector<bool>* used = 0) {
    size_t h = t.size();
    for (size_t i = 0; i < t.size() && h == t.size(); ++i)
        if (net.Tag(t[i], "hour") >= 0.5f && !(used && (*used)[i])) h = i;  // not the 20 of "il 20 dicembre"
    if (h == t.size()) return false;
    hour = t[h].word == "<time>" ? t[h].hour : t[h].num;
    minute = t[h].word == "<time>" ? t[h].minute : 0;
    size_t end = h + 1;  // the time's words: [h, end)
    if (t[h].word != "<time>" || minute == 0) {  // "mezzogiorno e mezza"
        size_t j = h + 1, n = 0;
        if (j + 1 < t.size() && Is(t[j], "e")) {
            const WspToken& a = t[j + 1];
            bool quarter = j + 2 < t.size() && Is(t[j + 2], "quarto");
            bool quarters = j + 2 < t.size() && Is(t[j + 2], "quarti");
            if (IsHalf(a)) { minute = 30; end = j + 2; }
            else if (a.word == "<num>" && a.num == 1 && quarter) { minute = 15; end = j + 3; }
            else if (a.word == "<num>" && a.num == 3 && quarters) { minute = 45; end = j + 3; }
            else if (!quarter && !quarters && Minutes(t, j + 1, true, minute, n)) end = j + 1 + n;
        } else if (j + 1 < t.size() && Is(t[j], "meno") && t[j + 1].word == "<num>") {
            int less = t[j + 1].num;
            end = j + 2;
            if (less == 1 && j + 2 < t.size() && Is(t[j + 2], "quarto")) { less = 15; end = j + 3; }
            if (less < 1 || less > 30) return false;
            hour = (hour + 23) % 24;
            minute = 60 - less;
        } else if (t[h].word == "<num>" && Minutes(t, j, false, minute, n)) {  // "alle nove quindici", "nove zero zero"
            end = j + n;
        }
    }
    if (used)
        for (size_t k = h; k < end; ++k) (*used)[k] = true;
    if (hour == 24) hour = 0;
    if (hour < 0 || hour > 23) return false;
    bool evening = day.evening || HasWord(t, "sera") || HasWord(t, "pomeriggio");
    bool night = day.night || HasWord(t, "notte");
    part = evening || night || HasWord(t, "mattina") || HasWord(t, "mattino");
    if (evening && hour < 12) hour += 12;
    if (night && hour >= 7 && hour < 12) hour += 12;
    return true;
}

bool TimeLike(const Wsp& net, const WspToken& t) {
    static const char* const f[] = {"tprep", "unit", "day", "part", "tra", "e_w", "half", "quarter", "meno", 0};
    for (int i = 0; f[i]; ++i)
        if (net.Has(t, f[i])) return true;
    return net.Tag(t, "hour") >= 0.5f || net.Tag(t, "minute") >= 0.5f || net.Tag(t, "qty") >= 0.5f ||
           t.word == "<time>";
}

std::string HourText(int h, int m) { return Two(h) + ":" + Two(m); }

bool Infinitive(const WspToken& t) {
    const std::string& w = t.folded;
    return w.size() >= 4 && (w.compare(w.size() - 3, 3, "are") == 0 || w.compare(w.size() - 3, 3, "ere") == 0 ||
                            w.compare(w.size() - 3, 3, "ire") == 0);
}

bool Edge(const Wsp& net, const WspToken& t) {  // chatter at the edge of a free text: "SENTI ...", "... GRAZIE"
    static const char* const w[] = {"per", "favore", "piacere", "cortesia", "grazie", "mille", "di", "a", "e", "lo",
                                    "la", "mi", "me", "come", "sul", "sulla", "nel", "nella", "in", "al", "alla",
                                    "il", "un", "una", "uno", "pratica", "che", "non", 0};  // "NON è che riusciresti"
    return net.Has(t, "joint") || net.Has(t, "coda") || net.Has(t, "wish") || IsAny(t, w);
}

// The free text of an alarm or an event, on either side of the command (user's request 2026-11-03):
// "metti una sveglia alle 7 CON IL TESTO fare la spesa", "FARE LA SPESA mettilo come sveglia alle 7",
// "metti sul calendario domani FARE UN GIRO A CATANIA". anchor: the command's own words; after the last one the
// time words are skipped; needMarker: an alarm takes the text after it only behind "con il testo", "che dice",
// "per" ("svegliami alle 6 che domani lavoro" has no text).
std::string FreeText(const Wsp& net, const WspResult& r, bool (*anchor)(const Wsp&, const WspToken&), bool needMarker,
                     const std::vector<bool>* used = 0) {
    const Tokens& t = r.tokens;
    // a word of the time or the date found (FindTime, FindDate): "alle nove QUINDICI", "il GIORNO 11 NOVEMBRE"
    struct TimeAt {
        const Wsp& net; const Tokens& t; const std::vector<bool>* used;
        bool operator()(size_t i) const { return TimeLike(net, t[i]) || (used && i < used->size() && (*used)[i]); }
    } timeAt = {net, t, used};
    int fa = -1, la = -1;
    for (size_t i = 0; i < t.size(); ++i)
        if (anchor(net, t[i])) { if (fa < 0) fa = static_cast<int>(i); la = static_cast<int>(i); }
    if (fa < 0) return std::string();
    // after: skip the time and the day, then the marker
    size_t a = static_cast<size_t>(la + 1), b = t.size();
    static const char* const lead[] = {"per", "il", "lo", "la", "l", "di", "un", "una", "uno", "sul", "sulla", "nel",
                                       "nella", "in", "al", "alla", "come", 0};
    while (a < b && (timeAt(a) || net.Has(t[a], "day") || net.Has(t[a], "part") || IsAny(t[a], lead) ||
                     net.Has(t[a], "alarm_n")) &&
           !(needMarker && Is(t[a], "per") && a + 1 < b && !timeAt(a + 1) && !net.Has(t[a + 1], "day") &&
             !IsAny(t[a + 1], lead)))
        ++a;
    bool marked = false;
    static const char* const marker[] = {"con", "che", "il", "la", "lo", "per", "un", "una", "dal", "si", "come", 0};
    // "... INTITOLATO la laurea di Marta", "... CON TITOLO cena", "... CHE SI CHIAMA ..." (phone log 2026-11-09)
    static const char* const titled[] = {"intitolato", "intitolata", "intitolalo", "intitolala", "titolo", "titolato",
                                         "titolata", "chiamato", "chiamata", 0};
    size_t m = a;
    while (m < b && (IsAny(t[m], marker) || IsAny(t[m], titled) || net.Has(t[m], "label_w") ||
                     (Is(t[m], "chiama") && m > a && Is(t[m - 1], "si")))) {
        marked = marked || net.Has(t[m], "label_w") || IsAny(t[m], titled) || Is(t[m], "chiama") || Is(t[m], "per") ||
                 (Is(t[m], "che") && m + 1 < b && Is(t[m + 1], "dice"));
        ++m;
    }
    if (needMarker) {
        if (marked) a = m;
        else a = b;  // no text after
    } else if (marked) {
        a = m;
    }
    while (b > a && (Edge(net, t[b - 1]) || timeAt(b - 1) || net.Has(t[b - 1], "day") ||
                     net.Has(t[b - 1], "part")) && !needMarker)
        --b;
    while (b > a && Edge(net, t[b - 1]) && needMarker) --b;
    if (a < b) return Original(r, a, b - 1);
    // before: "fare la spesa, mettilo come sveglia alle sette". For an alarm only when a verb points back to it
    // ("mettiLO", "impostaLA") or a comma closes it: else it is the address and the polite frame ("ciao tesoro
    // sarebbe possibile mettere una sveglia...", giro 11)
    if (needMarker) {
        bool back = false;
        for (int i = fa; i <= la && !back; ++i) {
            const std::string& f = t[static_cast<size_t>(i)].folded;
            back = f.size() > 4 && (f.compare(f.size() - 2, 2, "lo") == 0 || f.compare(f.size() - 2, 2, "la") == 0);
        }
        for (int i = 0; i + 1 < fa && !back; ++i) back = Comma(r, static_cast<size_t>(i));
        if (!back) return std::string();
    }
    a = 0;
    b = static_cast<size_t>(fa);
    while (a < b && (Edge(net, t[a]) || (net.Has(t[a], "filler") && !Infinitive(t[a])))) ++a;  // not "PRENDERE le medicine"
    while (b > a && Edge(net, t[b - 1])) --b;
    if (a < b) return Original(r, a, b - 1);
    // between: "aggiungi DAL DENTISTA giovedì al calendario"
    a = static_cast<size_t>(fa + 1);
    b = static_cast<size_t>(la);
    while (a < b && (Edge(net, t[a]) || anchor(net, t[a]))) ++a;
    while (b > a && (Edge(net, t[b - 1]) || timeAt(b - 1) || net.Has(t[b - 1], "day") ||
                     net.Has(t[b - 1], "part") || anchor(net, t[b - 1])))
        --b;
    return a < b && !needMarker ? Original(r, a, b - 1) : std::string();
}

bool AlarmAnchor(const Wsp& net, const WspToken& t) {
    return net.Has(t, "alarm_n") || net.Has(t, "set_v") || net.Tag(t, "hour") >= 0.5f || net.Tag(t, "minute") >= 0.5f;
}

bool EventAnchor(const Wsp& net, const WspToken& t) {
    return net.Has(t, "set_v") || net.Has(t, "add_v") || net.Has(t, "write_v") || net.Has(t, "cal_new") ||
           t.folded == "calendario" || t.folded == "agenda" || t.folded == "evento";
}

void Set(Command& c, const char* k, const std::string& v) { c.args.push_back(std::make_pair(std::string(k), v)); }

bool Fail(Command& c, const char* why) {
    c.why = why;
    return false;
}

}  // namespace

std::string Command::Arg(const std::string& key) const {
    for (size_t i = 0; i < args.size(); ++i)
        if (args[i].first == key) return args[i].second;
    return std::string();
}

namespace {
bool r_comma(const std::string& text, const WspToken& a, const WspToken& b) {
    return text.find_first_of(",;", a.end) < b.begin;
}

bool Ends(const Tokens& t, size_t from, size_t to, const char* a, const char* b = 0) {
    if (b == 0) return to > from && t[to - 1].folded == a;
    return to > from + 1 && t[to - 2].folded == a && t[to - 1].folded == b;
}

// "... a caso", "... in ordine casuale", "... in riproduzione casuale", "... mischiate": cut from the end of [from, to)
bool CutShuffle(const Tokens& t, size_t from, size_t& to) {
    static const char* const one[] = {"casuale", "casualmente", "shuffle", "random", "mischiate", "mischiati", "mischiata",
                                      "mischiato", "mescolate", "mescolati", "mescolata", "mescolato", "sparso", 0};
    static const char* const lead[] = {"in", "ordine", "modo", "riproduzione", "modalita", "con", "la", 0};
    bool found = false;
    if (Ends(t, from, to, "a", "caso")) { to -= 2; found = true; }
    else if (Ends(t, from, to, "alla", "rinfusa")) { to -= 2; found = true; }
    else if (to > from && IsAny(t[to - 1], one)) { --to; found = true; }
    if (found)
        while (to > from && IsAny(t[to - 1], lead)) --to;
    return found;
}

// A music request is read by its frame, not by the words of the title. The library is full of titles that the network
// takes for commands ("Time" = timer, "Can I Call You Tonight" = call, "Never Tear Us Apart" = "neve", weather), and it
// keeps only the words it tags as a name ("stand by" for Stand By Me). After the music verb, the rest of the sentence
// is the request, and Musica finds it in the library. share: tools/wsp/titoli.tsv, the real titles of the phone.
bool MusicFrame(const Wsp& net, const WspResult& in, Command& c) {
    WspResult r = in;
    TrimTail(net, r.tokens);
    const Tokens& t = r.tokens;
    if (t.empty() || t.size() > 24) return false;

    // "riproduzione casuale", "attiva la riproduzione casuale": everything, in random order
    for (size_t i = 0; i + 1 < t.size(); ++i)
        if (t[i].folded == "riproduzione" && (t[i + 1].folded == "casuale" || t[i + 1].folded == "shuffle")) {
            bool verb = false;
            for (size_t k = 0; k < i; ++k) verb = verb || net.Has(t[k], "music_v");
            if (verb) break;  // "metti i queen in riproduzione casuale": below
            if (in.intent != "online" && in.intent != "riproduci_musica") return false;
            c = Command();
            c.name = "riproduci_musica";
            c.args.push_back(std::make_pair(std::string("richiesta"), std::string()));
            c.args.push_back(std::make_pair(std::string("tipo"), std::string("casuale")));
            return true;
        }

    // the verb: after the last comma if there is one there ("ho voglia di sentire un po' di musica, metti Battiato");
    // "ascolta" and "ascoltami" alone are how a sentence starts, not a verb; "ho ascoltato" is a story
    static const char* const opener[] = {"ascolta", "ascoltami", "senti", "sentimi", 0};
    static const char* const aux[] = {"ho", "hai", "ha", "abbiamo", "avevo", "avete", "hanno", 0};
    size_t v = t.size();
    size_t from = 0;
    for (size_t i = 0; i + 1 < t.size(); ++i)
        if (Comma(r, i)) {
            size_t k = i + 1;
            while (k < t.size() && !net.Has(t[k], "music_v")) ++k;
            if (k + 1 < t.size()) from = i + 1;  // not "..., me lo metti?"
        }
    for (size_t i = from; i < t.size(); ++i) {
        if (!net.Has(t[i], "music_v") || IsAny(t[i], opener)) continue;
        if (i > 0 && IsAny(t[i - 1], aux)) return false;
        v = i;
        break;
    }
    if (v >= t.size()) return false;
    for (size_t i = from; i < v; ++i)  // "come posso ascoltare...", "non mettere..."
        if (net.Has(t[i], "q") || net.Tag(t[i], "che_q") >= 0.5f || net.Has(t[i], "neg")) return false;
    if (v + 1 < t.size() && Comma(r, v)) return false;

    static const char* const article[] = {"il", "lo", "la", "l", "i", "gli", "le", "un", "una", "uno", "qualche", "delle",
                                          "dei", "degli", "del", "della", "mi", "un'", 0};
    static const char* const song[] = {"canzone", "brano", "pezzo", 0};
    static const char* const songs[] = {"canzoni", "brani", "pezzi", "musica", "musiche", "roba", 0};
    static const char* const of[] = {"di", "dei", "degli", "del", "della", "delle", "dello", "dal", "da", 0};
    static const char* const away[] = {"precedente", "successiva", "successivo", "seguente", "prossima", "prossimo", "dopo",
                                       "prima", "pausa", "stop", 0};
    size_t a = v + 1;
    while (a < t.size() && (t[a].folded == "su" || t[a].folded == "mi")) ++a;  // "metti SU i beatles"
    if (a < t.size() && t[a].folded == "e") ++a;  // an echo of Vosk: "mettere e i pink floyd"
    if (a < t.size() && (t[a].folded == "pausa" || (t[a].folded == "in" && a + 1 < t.size() && t[a + 1].folded == "pausa")))
        return false;
    size_t n = a;
    while (n < t.size() && IsAny(t[n], article)) ++n;
    std::string kind;
    bool noun = false;
    if (n < t.size()) {
        if (IsAny(t[n], song)) { kind = "brano"; noun = true; }
        else if (IsAny(t[n], songs)) noun = true;
        else if (Is(t[n], "album") || (Is(t[n], "disco") && n + 1 < t.size())) { kind = "album"; noun = true; }
        else if (Is(t[n], "playlist")) { kind = "playlist"; noun = true; }
        else if (Is(t[n], "artista")) { kind = "artista"; noun = true; }
    }

    // the words of a phrase that say "not a title": the network keeps those sentences
    if (noun) {
        a = n + 1;
        if (a < t.size() && IsAny(t[a], of)) ++a;
        // "metti la canzone precedente", "metti la canzone di prima e basta"
        if (in.intent == "controlla_musica" && a < t.size() && IsAny(t[a], away)) return false;
    } else {
        const std::string& i = in.intent;
        if (i != "online" && i != "riproduci_musica" && i != "chiama" && i != "meteo") return false;
        static const char* const real[] = {"meteo", "tempo", "previsioni", "chiama", "chiamare", "chiamami", "telefona",
                                           "telefonare", "piove", "pioggia", "nevica", 0};
        for (size_t k = 0; k < t.size(); ++k)
            if (i != "online" && i != "riproduci_musica" && IsAny(t[k], real)) return false;
        // "ho dimenticato di mettere la sveglia": something else is being set, whatever the network made of the rest
        static const char* const other[] = {"sveglia", "sveglie", "timer", "nota", "note", "promemoria", "calendario",
                                            "appuntamento", "evento", "radio", 0};
        for (size_t k = a; k < t.size(); ++k)
            if (IsAny(t[k], other) && (t[k].folded != "radio" || k + 1 == t.size())) return false;  // not "radio ga ga"
    }

    size_t e = t.size();
    for (size_t k = a; k + 1 < e; ++k)
        if (Comma(r, k)) e = k + 1;  // "un po' di Pino Daniele, me lo metti?"
    bool shuffle = CutShuffle(t, a, e);
    if (Ends(t, a, e, "in", "sottofondo")) e -= 2;
    if (Ends(t, a, e, "gentilmente")) --e;
    // "... grazie infinite", "... ti ringrazio di cuore", "... sei un tesoro", not "grazie roma"
    for (size_t k = a + 1; k < e; ++k)
        if (e - k <= 5 && (t[k].folded == "grazie" || t[k].folded == "ringrazio" ||
                           (t[k].folded == "sei" && k + 1 < e && t[k + 1].folded == "un"))) {
            e = k > a + 1 && t[k - 1].folded == "ti" ? k - 1 : k;
            break;
        }
    if (Ends(t, a, e, "con", "calma")) e -= 2;
    if (a + 1 < e && t[a].folded == "a" && t[a + 1].folded == "caso") { a += 2; shuffle = true; }  // "metti a caso i queen"
    // "metti qualcosa di ...", "un po' di ...", "metti delle canzoni ...", "metti i pink floyd"
    if (a < e && (t[a].folded == "qualcosa" || t[a].folded == "qualche")) {
        ++a;
        if (a < e && IsAny(t[a], of)) ++a;
    } else if (a + 1 < e && t[a].folded == "un" && (t[a + 1].folded == "po" || t[a + 1].folded == "poco")) {
        a += 2;
        if (a < e && IsAny(t[a], of)) ++a;
    }
    static const char* const plain[] = {"il", "lo", "la", "l", "i", "gli", "le", 0};
    // not the "i" of "i'm a believer"
    while (a + 1 < e && IsAny(t[a], plain) && r.text[t[a].end] != '\'') ++a;
    if (noun && a < e && IsAny(t[a], of)) ++a;
    if (a + 1 == e && (IsAny(t[a], songs) || IsAny(t[a], song))) a = e;  // "metti un po' di musica"

    // nothing left and no noun: the network's own reading ("metti", "ascolta", "suona bene")
    if (a >= e && !noun && !shuffle && in.intent != "riproduci_musica") return false;

    c = Command();
    c.name = "riproduci_musica";
    std::string what = a < e ? Original(r, a, e - 1) : std::string();
    if (kind.empty()) {  // "riproduci l'ultimo album dei Coldplay"
        if (HasWord(t, "album")) kind = "album";
        else if (HasWord(t, "playlist")) kind = "playlist";
        else if (HasWord(t, "artista")) kind = "artista";
    }
    if (what.empty()) kind = "casuale";
    c.args.push_back(std::make_pair(std::string("richiesta"), what));
    c.args.push_back(std::make_pair(std::string("tipo"), kind));
    if (shuffle && !what.empty()) c.args.push_back(std::make_pair(std::string("casuale"), std::string("si")));
    return true;
}
}  // namespace

bool Understand(const Wsp& net, const std::string& text, const LocalTime& now, const std::vector<std::string>& targets,
                Command& out, WspResult& used) {
    used = net.Run(text);
    if (MusicFrame(net, used, out)) return true;
    const std::vector<WspToken> w = used.tokens;
    // where a clause can start: after a discourse marker ("PERÒ", "COMUNQUE", "lasciamo STARE"), "il punto è CHE",
    // "a parte QUESTO", a comma
    std::vector<size_t> starts(1, 0);
    for (size_t i = 1; i < w.size(); ++i) {
        const WspToken& p = w[i - 1];
        bool start = net.Has(p, "joint") || r_comma(text, p, w[i]) ||
                     (p.folded == "che" && i >= 2 && w[i - 2].folded == "e") ||
                     (p.folded == "questo" && i >= 2 && (w[i - 2].folded == "parte" || w[i - 2].folded == "detto"));
        if (start && !net.Has(w[i], "joint")) starts.push_back(i);  // "però allora ...": after the last marker
    }
    // a long spoken sentence (Vosk writes no commas): chatter, then the request. The whole of it is not judged:
    // its chatter has words of every command ("il motivo per cui ti CHIAMO", "dal SOLE"). Each clause is, from the
    // last one back, then each clause with the one after it ("metti la sveglia, allora, alle sette").
    if (w.size() > 12 && starts.size() > 1) {
        for (int span = 1; span <= 2; ++span) {
            for (size_t k = starts.size(); k-- > 0;) {
                size_t a = starts[k], e = k + span < starts.size() ? starts[k + span] - 1 : w.size();  // without the marker
                if (k + span > starts.size()) continue;
                while (e > a && net.Has(w[e - 1], "joint") && e < w.size()) --e;
                if (e <= a) continue;
                WspResult r = net.Run(text.substr(w[a].begin, w[e - 1].end - w[a].begin));
                Command c;
                if (!BuildCommand(net, r, now, targets, c)) continue;
                // the day said before still counts: "domani ho il treno presto comunque svegliami alle cinque"
                bool hasDay = false;
                std::string day;
                for (size_t i = 0; i < r.tokens.size(); ++i) hasDay = hasDay || net.Has(r.tokens[i], "day");
                for (size_t i = 0; i < a && day.empty(); ++i)
                    if (net.Has(w[i], "day")) day = w[i].folded;
                if (!hasDay && !day.empty()) {
                    WspResult r2 = net.Run(text.substr(w[a].begin, w[e - 1].end - w[a].begin) + " " + day);
                    Command c2;
                    if (BuildCommand(net, r2, now, targets, c2) && c2.name == c.name) { c = c2; r = r2; }
                }
                out = c;
                used = r;
                return true;
            }
        }
    }
    // a long sentence is never judged whole; its comma parts still are ("..., me lo scrivi nelle note?")
    bool longText = w.size() > 12 && starts.size() > 1;
    if (!longText && BuildCommand(net, used, now, targets, out)) return true;
    if (longText) out.why = "nessuna frase \xc3\xa8 un comando";
    Command firstTry = out;
    // the parts: split at commas, then cut each before "che" / "perche" / "ma"
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != ',' && text[i] != ';') continue;
        parts.push_back(text.substr(start, i - start));
        start = i + 1;
    }
    std::vector<std::string> cuts;
    for (size_t p = 0; p < parts.size(); ++p) {
        cuts.push_back(parts[p]);
        if (longText) continue;  // long: its clauses were tried above, only the comma parts are left
        std::string f = " " + Wsp::Fold(parts[p]) + " ";
        // a "che" that asks is not a cut: "ora CHE cos'è un timer" (tag che_q of the network)
        bool cheAsks = false;
        WspResult whole = net.Run(parts[p]);
        for (size_t i = 0; i < whole.tokens.size() && !cheAsks; ++i)
            cheAsks = whole.tokens[i].folded == "che" && net.Tag(whole.tokens[i], "che_q") >= 0.5f;
        static const char* const words[] = {" che ", " perche ", " ma ", " quindi ", " poi ", " allora ", 0};
        for (int w = 0; words[w]; ++w) {
            if (w == 0 && cheAsks) continue;
            size_t at = f.find(words[w]);
            if (at != std::string::npos && at > 1) cuts.push_back(f.substr(1, at - 1));  // folded: the net folds anyway
            // and what comes after it, only for connectors that start a new request ("... quindi SVEGLIAMI ALLE SEI");
            // after "che"/"ma" the context is lost ("che CANZONE È QUESTA", "il meteo dice che PIOVE SEMPRE")
            bool starts = w >= 3;
            size_t last = f.rfind(words[w]);
            if (starts && last != std::string::npos) cuts.push_back(f.substr(last + std::strlen(words[w])));
        }
    }
    if (cuts.size() < 2) return false;
    // a day said in another part still counts: "DOMANI ho il treno presto, svegliami alle cinque"
    std::string day;
    for (size_t i = 0; i < used.tokens.size() && day.empty(); ++i)
        if (net.Has(used.tokens[i], "day")) day = used.tokens[i].folded;
    for (size_t k = 0; k < cuts.size(); ++k) {
        WspResult r = net.Run(cuts[k]);
        Command c;
        if (!BuildCommand(net, r, now, targets, c)) {
            // "me lo scrivi nelle note?": the note is the part before, from its last "che"
            bool noteWord = false, writeWord = false;
            for (size_t i = 0; i < r.tokens.size(); ++i) {
                noteWord = noteWord || net.Has(r.tokens[i], "note_n");
                writeWord = writeWord || net.Has(r.tokens[i], "write_v") || net.Has(r.tokens[i], "add_v");
            }
            if (noteWord && writeWord && k > 0) {
                std::string before = Wsp::Fold(parts[0]);
                size_t che = before.rfind(" che ");
                if (che != std::string::npos) before = before.substr(che + 5);
                WspResult r2 = net.Run("scrivi nelle note " + before);
                if (BuildCommand(net, r2, now, targets, c)) {
                    out = c;
                    used = r2;
                    return true;
                }
            }
            continue;
        }
        bool hasDay = false;
        for (size_t i = 0; i < r.tokens.size(); ++i) hasDay = hasDay || net.Has(r.tokens[i], "day");
        if (!hasDay && !day.empty()) {
            WspResult r2 = net.Run(cuts[k] + " " + day);
            Command c2;
            if (BuildCommand(net, r2, now, targets, c2) && c2.name == c.name) {
                c = c2;
                r = r2;
            }
        }
        out = c;
        used = r;
        return true;
    }
    out = firstTry;
    return false;
}

std::string SearchQuery(const Wsp& net, const std::string& text) {
    WspResult r;
    r.text = text;
    r.tokens = net.Tokenize(text);
    // openings: fillers, wishes, "senti una cosa", "volevo chiederti"; never a question word or a number
    static const char* const keep[] = {"q", "che_w", "num", "time", "day", "open", 0};
    static const char* const lead[] = {"filler", "wish", "coda", 0};
    static const char* const asking[] = {"sapere", "dici", "dire", "dirmi", "dimmi", "cerca", "cercami", "cercare",
                                         "chiederti", "domandarti", "in", 0};  // "volevo SAPERE", "mi DICI", "IN pratica"
    size_t a = 0, b = r.tokens.size();
    for (; a < b; ++a) {
        bool k = false, l = false;
        for (int i = 0; keep[i]; ++i) k = k || net.Has(r.tokens[a], keep[i]);
        for (int i = 0; lead[i]; ++i) l = l || net.Has(r.tokens[a], lead[i]);
        // "volevo SAPERE", "mi DICI", "CERCA": asking verbs before the question
        l = l || IsAny(r.tokens[a], asking);
        if (k || !l) break;
    }
    // "una cosa" after "senti": the question starts after it
    if (a + 1 < b && r.tokens[a].word == "<num>" && r.tokens[a + 1].folded == "cosa") a += 2;
    while (a < b && (net.Has(r.tokens[a], "filler") || net.Has(r.tokens[a], "wish")) && !net.Has(r.tokens[a], "q") &&
           !net.Has(r.tokens[a], "che_w") && !net.Has(r.tokens[a], "open")) ++a;
    // endings: politeness and chatter ("per favore", "grazie mille", "se puoi", "porca miseria")
    static const char* const small[] = {"per", "lo", "se", "ti", "mi", "e", 0};
    while (b > a && (net.Has(r.tokens[b - 1], "coda") || IsAny(r.tokens[b - 1], small)) && !net.Has(r.tokens[b - 1], "q") &&
           !net.Has(r.tokens[b - 1], "open") && r.tokens[b - 1].word != "<num>")
        --b;
    return a < b ? Original(r, a, b - 1) : std::string();
}

std::string ItalianDate(int year, int month, int day) {
    static const char* days[] = {"domenica", "luned\xc3\xac", "marted\xc3\xac", "mercoled\xc3\xac", "gioved\xc3\xac",
                                 "venerd\xc3\xac", "sabato"};
    static const char* months[] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio",
                                   "agosto", "settembre", "ottobre", "novembre", "dicembre"};
    long long z = DaysFromCivil(year, month, day);
    long long wd = (z + 4) % 7;
    if (wd < 0) wd += 7;
    char b[64];
    snprintf(b, sizeof(b), "%s %d %s", days[wd], day, months[(month - 1) % 12]);
    return b;
}

namespace {
// the politeness at the end of a request, word by word from the end, until nothing more comes off
void TrimTail(const Wsp& net, Tokens& t) {
    static const char* const thanksMore[] = {"mille", "tante", "tanto", "davvero", "molte", 0};
    static const char* const ifTail[] = {"ci", "riesci", "puoi", "non", "ti", "dispiace", "vuoi", "va", 0};
    static const char* const adjLead[] = {"molto", "sei", "davvero", "proprio", "troppo", "tanto", "e", 0};
    for (bool again = true; again && !t.empty();) {
        again = false;
        size_t n = t.size();
        const std::string& last = t[n - 1].folded;
        const std::string& before = n >= 2 ? t[n - 2].folded : std::string();
        if (n >= 2 && before == "per" && (last == "favore" || last == "piacere" || last == "cortesia")) {
            t.resize(n - 2);
            again = true;
        } else if (n >= 2 && (before == "grazie" || before == "ringrazio") && IsAny(t[n - 1], thanksMore)) {
            t.resize(n - 1);  // "grazie DAVVERO", "ti ringrazio TANTO": then "grazie"/"ringrazio" below
            again = true;
        } else if (n >= 2 && before == "ti" && last == "ringrazio") {
            t.resize(n - 2);
            again = true;
        } else if (last == "grazie" || last == "please" || last == "pls" || last == "plz" || last == "perfavore") {
            t.resize(n - 1);
            again = true;
        } else if (n >= 2 && before == "quando" && last == "puoi") {
            t.resize(n - 2);
            again = true;
        } else if (IsAny(t[n - 1], ifTail)) {
            // "se puoi", "se ci riesci", "se non ti dispiace": back to the "se" within three words
            size_t k = n - 1;
            while (k > 0 && n - k <= 4 && IsAny(t[k], ifTail)) --k;
            if (t[k].folded == "se" && k > 0) {
                t.resize(k);
                again = true;
            }
        } else if (net.Has(t[n - 1], "polite_adj") || (net.Has(t[n - 1], "frame") && n >= 2) ||
                   (n >= 2 && last.size() >= 8 && last.compare(last.size() - 5, 5, "mente") == 0)) {
            // "molto GENTILE", "sei GENTILISSIMO", "CORTESEMENTE" (a word of the polite frame at the end)
            size_t k = n - 1;
            while (k > 0 && IsAny(t[k - 1], adjLead) && !TimeLike(net, t[k - 1])) --k;  // not "alle SEI gentilmente"
            t.resize(k);
            again = true;
        }
    }
}
}  // namespace

bool BuildCommand(const Wsp& net, const WspResult& in, const LocalTime& now, const std::vector<std::string>& targets,
                  Command& c) {
    c = Command();
    c.name = in.intent;
    // "... per favore", "... grazie davvero", "... se ci riesci", "... sei gentilissimo": politeness at the end is
    // never part of a name, a title or a text (giro 11: one rule for every intent, TrimTail)
    WspResult r = in;
    TrimTail(net, r.tokens);
    const Tokens& t = r.tokens;
    const std::string& n = r.intent;
    long long today = DaysFromCivil(now.year, now.month, now.day);
    std::vector<bool> used(t.size(), false);  // the words of the date and the time
    Day day = FindDay(net, t, now, &used);
    if (n == "online") return Fail(c, "online");

    if (n == "stato_timer" || n == "annulla_timer" || n == "elenca_sveglie") return true;

    if (n == "imposta_timer") {
        int s = Duration(net, t);
        if (s <= 0) return Fail(c, "durata");
        Set(c, "secondi", std::to_string(static_cast<long long>(s)));
        Set(c, "etichetta", Span(net, r));
        return true;
    }

    if (n == "imposta_sveglia" || n == "cancella_sveglia" || n == "promemoria") {
        int h = 0, m = 0;
        bool part = false;
        bool timed = FindTime(net, t, day, h, m, part, &used);
        if (n == "cancella_sveglia") {
            if (!timed) return Fail(c, "ora");
            Set(c, "ora", HourText(h, m));
            return true;
        }
        if (n == "imposta_sveglia") {
            if (!timed) return Fail(c, "ora");
            std::string repeat;
            for (size_t i = 0; i < t.size(); ++i) {
                if (Is(t[i], "feriali")) repeat = "feriali";
                else if (Is(t[i], "weekend") || (Is(t[i], "fine") && i + 1 < t.size() && Is(t[i + 1], "settimana")))
                    repeat = repeat.empty() ? "weekend" : repeat;
                else if ((Is(t[i], "ogni") || Is(t[i], "tutti")) && i + 1 < t.size() &&
                         (Is(t[i + 1], "giorno") || Is(t[i + 1], "giorni") || (i + 2 < t.size() && Is(t[i + 2], "giorni"))))
                    repeat = "ogni giorno";
            }
            Set(c, "ora", HourText(h, m));
            std::string giorno;
            if (repeat.empty() && day.offset >= 0) {
                long long target = today + day.offset;
                if (day.weekday && day.offset == 0 && h * 60 + m <= now.hour * 60 + now.minute) target += 7;
                giorno = DateText(target);
            }
            Set(c, "giorno", giorno);
            Set(c, "ripeti", repeat.empty() ? "no" : repeat);
            Set(c, "etichetta", FreeText(net, r, AlarmAnchor, true, &used));
            return true;
        }
        // promemoria: the text after "ricordami", without the time words at its ends
        int k = First(net, t, "remind_v");
        if (k < 0) return Fail(c, "testo");
        static const char* const lead[] = {"di", "che", "a", "ad", "mi", "me", "per", 0};
        static const char* const trail[] = {"di", "del", "della", 0};  // "alle 8 DI sera"
        size_t a = static_cast<size_t>(k) + 1, b = t.size();
        while (a < b && (TimeLike(net, t[a]) || IsAny(t[a], lead))) ++a;
        while (b > a && (TimeLike(net, t[b - 1]) || IsAny(t[b - 1], trail))) --b;
        if (a >= b) return Fail(c, "testo");
        std::string text = Original(r, a, b - 1);
        long long target = today;
        if (timed) {
            int nowMin = now.hour * 60 + now.minute;
            // "alle 6" said at 10:00 means 18:00, as a person would take it
            if (!part && day.offset < 0 && h < 12 && h * 60 + m <= nowMin && (h + 12) * 60 + m > nowMin) h += 12;
            if (day.offset >= 0) {
                target = today + day.offset;
                if (day.weekday && day.offset == 0 && h * 60 + m <= nowMin) target += 7;
            } else if (h * 60 + m <= nowMin) {
                target = today + 1;
            }
        } else {
            int s = Duration(net, t);
            if (s <= 0 || (!HasWord(t, "tra") && !HasWord(t, "fra"))) return Fail(c, "quando");
            long long minutes = now.hour * 60 + now.minute + (s + 59) / 60;
            target = today + minutes / (24 * 60);
            h = static_cast<int>((minutes / 60) % 24);
            m = static_cast<int>(minutes % 60);
        }
        Set(c, "testo", text);
        Set(c, "ora", HourText(h, m));
        Set(c, "giorno", DateText(target));
        return true;
    }

    if (n == "prendi_nota") {
        int k = First(net, t, "note_n");
        if (k < 0) k = First(net, t, "note_v");
        static const char* const lead[] = {"di", "che", "a", "veloce", "velocemente", "rapida", "rapido", 0};
        size_t a = static_cast<size_t>(k + 1);
        // "una nota CON SCRITTO ...", "una nota su onenote E SCRIVI ...": the text starts after the first write or
        // label word behind the note word (cortesi-a.tsv: the text kept "su onenote e scrivere")
        if (k >= 0)
            for (size_t i = a; i < t.size(); ++i)
                if (net.Has(t[i], "write_v") || net.Has(t[i], "label_w")) { a = i + 1; break; }
        while (a < t.size() && IsAny(t[a], lead)) ++a;
        if (k >= 0 && a < t.size()) {
            Set(c, "testo", Original(r, a, t.size() - 1));
            return true;
        }
        // nothing after "note": "mi salvi di scrivere LE MELE nelle note" = between the last write verb and it
        int w = -1;
        for (int i = 0; i < k; ++i)
            if (net.Has(t[i], "write_v") || net.Has(t[i], "add_v")) w = i;
        static const char* const lead2[] = {"di", "che", "a", "mi", "me", "per", "anche", 0};
        static const char* const trail[] = {"nelle", "nella", "nel", "sulle", "sul", "in", "alle", "tra", "fra", "le", 0};
        size_t b2 = static_cast<size_t>(k);
        size_t a2 = static_cast<size_t>(w + 1);
        while (a2 < b2 && IsAny(t[a2], lead2)) ++a2;
        while (b2 > a2 && IsAny(t[b2 - 1], trail)) --b2;
        if (w < 0 || a2 >= b2) return Fail(c, "testo");
        Set(c, "testo", Original(r, a2, b2 - 1));
        return true;
    }

    if (n == "riproduci_musica") {
        std::string what = Span(net, r, true), kind;
        if (HasWord(t, "album")) kind = "album";
        else if (HasWord(t, "playlist")) kind = "playlist";
        else if (HasWord(t, "canzone") || HasWord(t, "brano")) kind = "brano";
        else if (HasWord(t, "artista")) kind = "artista";
        if (what.empty()) kind = "casuale";
        Set(c, "richiesta", what);
        Set(c, "tipo", kind);
        return true;
    }

    if (n == "controlla_musica") {
        // "rimetti la precedente": the direction wins over "rimetti"
        const char* a = First(net, t, "pause") >= 0 ? "pausa" : First(net, t, "prev") >= 0 ? "indietro"
                        : First(net, t, "next") >= 0 ? "avanti" : First(net, t, "resume") >= 0 ? "riprendi"
                        : First(net, t, "basta_w") >= 0 ? "pausa" : 0;  // "basta musica" (not "... e basta")
        if (!a) return Fail(c, "azione");
        Set(c, "azione", a);
        return true;
    }

    if (n == "chiama" || n == "cerca_sul_web") {
        int k = First(net, t, n == "chiama" ? "call_v" : "web_v");
        if (n == "chiama" && k < 0)  // "telefono a Francesca"
            for (size_t i = 0; i < t.size() && k < 0; ++i)
                if (Is(t[i], "telefono")) k = static_cast<int>(i);
        // the last call verb with a name after it: "il telefono di casa, puoi CHIAMARE casa", but "devo TELEFONARE
        // a papà, lo puoi chiamare tu"
        for (size_t i = 0; n == "chiama" && i + 1 < t.size(); ++i)
            if (net.Has(t[i], "call_v") && !net.Has(t[i + 1], "filler") && !net.Has(t[i + 1], "coda")) k = static_cast<int>(i);
        if (n == "chiama" && k >= 0 && k + 1 < static_cast<int>(t.size()) && (net.Has(t[k + 1], "filler") || net.Has(t[k + 1], "coda"))) {
            for (size_t i = 0; i + 1 < t.size(); ++i)  // "chiama a casa", "chiamami la nonna": lead words are skipped below
                if (net.Has(t[i], "call_v")) { k = static_cast<int>(i); break; }
        }
        size_t a = static_cast<size_t>(k + 1);
        static const char* const callLead[] = {"a", "ad", "al", "alla", "allo", "ai", "con", "subito", "adesso", 0};
        static const char* const webLead[] = {"su", "in", "google", "internet", "rete", "online", "sul", "web", "sulla",
                                              "sui", "sulle", 0};  // "una ricerca su internet SULLA ricetta"
        size_t b = t.size() - 1;
        if (n == "chiama") {
            if (a + 1 < t.size() && Is(t[a], "il") && Is(t[a + 1], "numero")) a += 2;  // "componi il numero di Anna"
            if (a + 1 < t.size() && (Is(t[a], "il") || Is(t[a], "al")) && t[a + 1].word == "<num>") ++a;  // "chiama il 112"
            while (a < t.size() && (IsAny(t[a], callLead) || Is(t[a], "di"))) ++a;
            if (a < t.size()) b = NameEnd(net, r, a);  // "chiama mamma, lo puoi fare?"
        } else {
            while (a < t.size() && IsAny(t[a], webLead)) ++a;
        }
        if (k < 0 || a >= t.size()) return Fail(c, "chi");
        Set(c, n == "chiama" ? "chi" : "domanda", Original(r, a, b));
        return true;
    }

    if (n == "apri") {
        int k = First(net, t, "settings_n");  // "vai nelle impostazioni del bluetooth": after the last opener word
        if (k < 0) k = First(net, t, "open_v");
        if (k < 0) k = First(net, t, "go");
        if (k < 0) k = First(net, t, "list_v");
        if (k < 0) k = First(net, t, "toggle_v");
        static const char* const skip[] = {"l", "app", "applicazione", "le", "impostazioni", "impostazione", "del",
                                           "della", "dello", "dei", "delle", "il", "la", "lo", "i", "gli", "su", "in",
                                           "di", "nelle", "nella", "nel", "negli", "sul", "sulle", "sulla", "sullo", 0};
        size_t a = static_cast<size_t>(k + 1);
        while (a < t.size() && IsAny(t[a], skip)) ++a;
        static const char* const synonyms[][2] = {
            {"wifi", "wi-fi"}, {"gps", "posizione"}, {"dati", "rete cellulare"}, {"batteria", "risparmio batteria"},
            {"aereo", "modalita aereo"}, {"blocco", "schermata di blocco"}, {"email", "posta"}, {"e-mail", "posta"}, {"le email", "posta"},
            {"account", "account email"}, {"mail", "posta"}, {"perle", "perle tv"}, {"perletv", "perle tv"}, {"rotazione", "rotazione schermo"},
            {"wi fi", "wi-fi"}, {"impostazioni", ""}, {0, 0}};
        // the longest run of words from a that names a target: "apri il bluetooth SUBITO PER FAVORE"
        std::string want, found;
        for (size_t i = a; i < t.size(); ++i) {
            want += (want.empty() ? "" : " ") + (t[i].fuzzy ? t[i].word : t[i].folded);
            std::string name = want;
            for (int s = 0; synonyms[s][0]; ++s)
                if (name == synonyms[s][0]) name = synonyms[s][1];
            for (size_t j = 0; j < targets.size(); ++j)
                if (Wsp::Fold(targets[j]) == name) found = targets[j];
        }
        if (found.empty()) return Fail(c, "cosa");
        Set(c, "cosa", found);
        return true;
    }

    if (n == "meteo") {
        Set(c, "luogo", Span(net, r));
        Set(c, "giorno", day.offset > 0 ? DateText(today + day.offset) : std::string());
        return true;
    }

    if (n == "aggiungi_evento") {
        int h = 0, m = 0;
        bool part = false;
        bool timed = FindTime(net, t, day, h, m, part, &used);
        std::string title = FreeText(net, r, EventAnchor, false, &used);
        if (title.empty()) return Fail(c, "titolo");
        if (timed && !part && h >= 1 && h <= 7) h += 12;  // "giovedì alle quattro dal dentista": the afternoon
        Set(c, "titolo", title);
        Set(c, "giorno", DateText(today + (day.offset > 0 ? day.offset : 0)));
        Set(c, "ora", timed ? HourText(h, m) : std::string());
        return true;
    }

    if (n == "leggi_calendario") {
        Set(c, "dal", DateText(today + (day.offset > 0 ? day.offset : 0)));
        Set(c, "giorni", day.week ? "7" : day.weekend ? "2" : "1");
        return true;
    }

    if (n == "ora") {
        char b[48];
        if (now.hour == 1) snprintf(b, sizeof(b), "\xc3\x88 l'una e %d.", now.minute);
        else snprintf(b, sizeof(b), "Sono le %d:%02d.", now.hour, now.minute);
        c.say = now.hour == 1 && now.minute == 0 ? std::string("\xc3\x88 l'una.") : std::string(b);
        return true;
    }

    if (n == "data") {
        int off = day.offset > 0 ? day.offset : 0;
        int y = 0, mo = 0, d = 0;
        CivilFromDays(today + off, y, mo, d);
        std::string when = off == 0 ? "Oggi" : off == 1 ? "Domani" : off == 2 ? "Dopodomani" : "Quel giorno";
        c.say = when + " \xc3\xa8 " + ItalianDate(y, mo, d) + ".";
        if (off > 0) Set(c, "giorno", DateText(today + off));
        return true;
    }

    return Fail(c, "intento sconosciuto");
}

}  // namespace assist
