#include "tlobj.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace tg {

#include "tl_schema.inc"

namespace {

const uint32_t kGzipPacked = 0x3072cfa1u;
const std::size_t kCtorCount = sizeof(kTlCtors) / sizeof(kTlCtors[0]);
const std::size_t kParamCount = sizeof(kTlParams) / sizeof(kTlParams[0]);

struct TType {
    enum K { Int, Long, Double, String, Int128, Int256, Hash, BoolT, TrueT, VecBoxed, VecBare, Boxed, Bare, Any };
    K k;
    TType* inner;            // vectors
    const TlCtorDef* bare;   // Bare
    TType() : k(Any), inner(0), bare(0) {}
};

struct PType {
    TType* t;
    int flagParam;   // index within the constructor's params of the "#" field, -1 if unconditional
    int bit;
    PType() : t(0), flagParam(-1), bit(0) {}
};

struct Registry {
    std::unordered_map<uint32_t, const TlCtorDef*> byId;
    std::unordered_map<std::string, const TlCtorDef*> byName;
    std::unordered_map<std::string, const TlCtorDef*> bareByType;   // "%Type" -> first constructor
    std::map<std::string, TType*> types;
    std::vector<PType> params;
};

Registry* g_reg = 0;
std::once_flag g_once;
std::mutex g_typeMutex;   // compileType after start-up (tlWrite/tlRead with a new type string)

TType* compileType(Registry& r, const std::string& t)
{
    std::map<std::string, TType*>::iterator it = r.types.find(t);
    if (it != r.types.end()) return it->second;
    TType* x = new TType();
    r.types[t] = x;
    if (t == "int") x->k = TType::Int;
    else if (t == "long") x->k = TType::Long;
    else if (t == "double") x->k = TType::Double;
    else if (t == "string" || t == "bytes") x->k = TType::String;
    else if (t == "int128") x->k = TType::Int128;
    else if (t == "int256") x->k = TType::Int256;
    else if (t == "#") x->k = TType::Hash;
    else if (t == "Bool") x->k = TType::BoolT;
    else if (t == "true") x->k = TType::TrueT;
    else if (t == "Object" || t == "X" || t == "!X") x->k = TType::Any;
    else if (t.compare(0, 7, "Vector<") == 0 || t.compare(0, 7, "vector<") == 0) {
        x->k = t[0] == 'V' ? TType::VecBoxed : TType::VecBare;
        x->inner = compileType(r, t.substr(7, t.size() - 8));
    } else if (t[0] == '%') {
        std::unordered_map<std::string, const TlCtorDef*>::iterator b = r.bareByType.find(t.substr(1));
        if (b == r.bareByType.end()) throw std::runtime_error("TL: no constructor for bare type " + t);
        x->k = TType::Bare;
        x->bare = b->second;
    } else if (t[0] >= 'a' && t[0] <= 'z' && t.find('.') == std::string::npos && r.byName.count(t)) {
        x->k = TType::Bare;
        x->bare = r.byName[t];
    } else {
        x->k = TType::Boxed;
    }
    return x;
}

void buildRegistry()
{
    Registry* r = new Registry();
    for (std::size_t i = 0; i < kCtorCount; ++i) {
        const TlCtorDef* c = &kTlCtors[i];
        r->byId[c->id] = c;
        r->byName[c->name] = c;
        if (!c->isFunction && !r->bareByType.count(c->type)) r->bareByType[c->type] = c;
    }
    r->params.resize(kParamCount);
    for (std::size_t i = 0; i < kCtorCount; ++i) {
        const TlCtorDef* c = &kTlCtors[i];
        for (int p = 0; p < c->count; ++p) {
            std::string t = kTlParams[c->first + p].type;
            PType& pt = r->params[c->first + p];
            std::size_t q = t.find('?');
            if (q != std::string::npos) {
                std::size_t dot = t.find('.');
                std::string flagName = t.substr(0, dot);
                pt.bit = std::atoi(t.substr(dot + 1, q - dot - 1).c_str());
                for (int f = 0; f < c->count; ++f)
                    if (flagName == kTlParams[c->first + f].name) pt.flagParam = f;
                if (pt.flagParam < 0) throw std::runtime_error(std::string("TL: bad flag in ") + c->name);
                t = t.substr(q + 1);
            }
            pt.t = compileType(*r, t);
        }
    }
    g_reg = r;
}

Registry& reg()
{
    std::call_once(g_once, buildRegistry);
    return *g_reg;
}

int paramIndex(const TlCtorDef* c, const char* field)
{
    for (int p = 0; p < c->count; ++p)
        if (std::strcmp(kTlParams[c->first + p].name, field) == 0) return p;
    return -1;
}

const TlValue& nullValue()
{
    static const TlValue* v = new TlValue();
    return *v;
}

TlValue readType(TlReader& rd, const TType* t);

TlValue readParams(TlReader& rd, const TlCtorDef* c)
{
    Registry& r = reg();
    TlValue v = TlValue::obj(c);
    std::vector<TlValue>& s = v.slots();
    for (int p = 0; p < c->count; ++p) {
        const PType& pt = r.params[c->first + p];
        if (pt.flagParam >= 0) {
            uint32_t flags = static_cast<uint32_t>(s[pt.flagParam].asInt());
            if (!(flags & (1u << pt.bit))) continue;
            if (pt.t->k == TType::TrueT) { s[p] = TlValue::boolean(true); continue; }
        }
        s[p] = readType(rd, pt.t);
    }
    return v;
}

TlValue readBoxed(TlReader& rd, const TType* t, uint32_t id)
{
    if (id == kGzipPacked) {
        std::string packed = rd.readBytes(), plain;
        if (!gunzipBytes(packed, plain)) throw std::runtime_error("TL: gzip_packed inflate failed");
        TlReader inner(reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
        return readType(inner, t);
    }
    if (id == kBoolTrue) return TlValue::boolean(true);
    if (id == kBoolFalse) return TlValue::boolean(false);
    if (id == kVector) {
        std::size_t n = rd.readUInt32();
        TlValue v = TlValue::vec();
        TType any;
        const TType* inner = (t && t->k == TType::VecBoxed) ? t->inner : &any;
        for (std::size_t i = 0; i < n; ++i) v.push(readType(rd, inner));
        return v;
    }
    const TlCtorDef* c = tlFind(id);
    if (!c) {
        char buf[64];
        std::sprintf(buf, "TL: unknown constructor 0x%08x", id);
        throw std::runtime_error(buf);
    }
    return readParams(rd, c);
}

TlValue readType(TlReader& rd, const TType* t)
{
    switch (t->k) {
    case TType::Int: return TlValue::i32(rd.readInt32());
    case TType::Hash: return TlValue::i32(rd.readInt32());
    case TType::Long: return TlValue::i64(rd.readInt64());
    case TType::Double: return TlValue::dbl(rd.readDouble());
    case TType::String: return TlValue::bytes(rd.readBytes());
    case TType::Int128:
    case TType::Int256: {
        uint8_t b[32];
        std::size_t n = t->k == TType::Int128 ? 16 : 32;
        rd.readRaw(b, n);
        return TlValue::bytes(std::string(reinterpret_cast<char*>(b), n));
    }
    case TType::TrueT: return TlValue::boolean(true);
    case TType::VecBare: {
        std::size_t n = rd.readUInt32();
        TlValue v = TlValue::vec();
        for (std::size_t i = 0; i < n; ++i) v.push(readType(rd, t->inner));
        return v;
    }
    case TType::Bare: return readParams(rd, t->bare);
    case TType::BoolT:
    case TType::VecBoxed:
    case TType::Boxed:
    case TType::Any:
    default:
        return readBoxed(rd, t, rd.readUInt32());
    }
}

void writeType(TlWriter& w, const TlValue& v, const TType* t);

void writeParams(TlWriter& w, const TlValue& v)
{
    Registry& r = reg();
    const TlCtorDef* c = v.ctor();
    const std::vector<TlValue>& s = v.items();
    // compute every "#" field from the optional fields that are present
    std::vector<uint32_t> flags(c->count, 0);
    for (int p = 0; p < c->count; ++p) {
        const PType& pt = r.params[c->first + p];
        if (pt.flagParam < 0 || s[p].isNull()) continue;
        if (pt.t->k == TType::TrueT && !s[p].asBool()) continue;
        flags[pt.flagParam] |= 1u << pt.bit;
    }
    for (int p = 0; p < c->count; ++p) {
        const PType& pt = r.params[c->first + p];
        if (pt.t->k == TType::Hash) { w.writeUInt32(flags[p]); continue; }
        if (pt.flagParam >= 0) {
            if (s[p].isNull()) continue;
            if (pt.t->k == TType::TrueT) continue;
        }
        if (s[p].isNull() && pt.t->k != TType::Hash)
            throw std::runtime_error(std::string("TL: missing field ") + c->name + "." + kTlParams[c->first + p].name);
        writeType(w, s[p], pt.t);
    }
}

void writeType(TlWriter& w, const TlValue& v, const TType* t)
{
    switch (t->k) {
    case TType::Int:
    case TType::Hash: w.writeInt32(v.asInt()); break;
    case TType::Long: w.writeInt64(v.asLong()); break;
    case TType::Double: w.writeDouble(v.asDouble()); break;
    case TType::String: w.writeString(v.str()); break;
    case TType::Int128:
    case TType::Int256: {
        std::size_t n = t->k == TType::Int128 ? 16 : 32;
        if (v.str().size() != n) throw std::runtime_error("TL: int128/int256 of wrong size");
        w.writeRaw(reinterpret_cast<const uint8_t*>(v.str().data()), n);
        break;
    }
    case TType::BoolT: w.writeBool(v.asBool()); break;
    case TType::TrueT: break;
    case TType::VecBoxed:
    case TType::VecBare:
        if (t->k == TType::VecBoxed) w.writeConstructor(kVector);
        w.writeUInt32(static_cast<uint32_t>(v.size()));
        for (std::size_t i = 0; i < v.size(); ++i) writeType(w, v.at(i), t->inner);
        break;
    case TType::Bare:
        writeParams(w, v);
        break;
    case TType::Boxed:
    case TType::Any:
    default:
        if (v.kind() == TlValue::Bytes && t->k == TType::Any) {
            w.writeRaw(reinterpret_cast<const uint8_t*>(v.str().data()), v.str().size());
        } else if (v.kind() == TlValue::Bool) {
            w.writeBool(v.asBool());
        } else {
            if (v.kind() != TlValue::Object) throw std::runtime_error("TL: object expected");
            w.writeConstructor(v.ctor()->id);
            writeParams(w, v);
        }
        break;
    }
}

void dumpTo(std::string& out, const TlValue& v, int depth)
{
    char buf[64];
    switch (v.kind()) {
    case TlValue::Null: out += "null"; break;
    case TlValue::Int: std::sprintf(buf, "%d", v.asInt()); out += buf; break;
    case TlValue::Long: std::sprintf(buf, "%lld", static_cast<long long>(v.asLong())); out += buf; break;
    case TlValue::Double: std::sprintf(buf, "%g", v.asDouble()); out += buf; break;
    case TlValue::Bool: out += v.asBool() ? "true" : "false"; break;
    case TlValue::Bytes: {
        const std::string& s = v.str();
        bool text = true;
        for (std::size_t i = 0; i < s.size() && text; ++i)
            if (static_cast<unsigned char>(s[i]) < 32) text = false;
        if (text) out += "\"" + (s.size() > 60 ? s.substr(0, 60) + "..." : s) + "\"";
        else { std::sprintf(buf, "<%u bytes>", static_cast<unsigned>(s.size())); out += buf; }
        break;
    }
    case TlValue::Vector:
        if (depth <= 0) { std::sprintf(buf, "[%u]", static_cast<unsigned>(v.size())); out += buf; break; }
        out += "[";
        for (std::size_t i = 0; i < v.size(); ++i) { if (i) out += ", "; dumpTo(out, v.at(i), depth - 1); }
        out += "]";
        break;
    case TlValue::Object: {
        out += v.ctor()->name;
        if (depth <= 0) { out += "{..}"; break; }
        out += "{";
        bool first = true;
        for (int p = 0; p < v.ctor()->count; ++p) {
            const TlValue& f = v.items()[p];
            if (f.isNull()) continue;
            if (!first) out += " ";
            first = false;
            out += kTlParams[v.ctor()->first + p].name;
            out += "=";
            dumpTo(out, f, depth - 1);
        }
        out += "}";
        break;
    }
    }
}

} // namespace

int tlLayer() { return TL_SCHEMA_LAYER; }

const TlCtorDef* tlFind(uint32_t id)
{
    Registry& r = reg();
    std::unordered_map<uint32_t, const TlCtorDef*>::const_iterator it = r.byId.find(id);
    return it == r.byId.end() ? 0 : it->second;
}

const TlCtorDef* tlFind(const std::string& name)
{
    Registry& r = reg();
    std::unordered_map<std::string, const TlCtorDef*>::const_iterator it = r.byName.find(name);
    return it == r.byName.end() ? 0 : it->second;
}

TlValue::TlValue() : kind_(Null), i_(0), d_(0), ctor_(0) {}

TlValue TlValue::i32(int32_t v) { TlValue x; x.kind_ = Int; x.i_ = v; return x; }
TlValue TlValue::i64(int64_t v) { TlValue x; x.kind_ = Long; x.i_ = v; return x; }
TlValue TlValue::dbl(double v) { TlValue x; x.kind_ = Double; x.d_ = v; return x; }
TlValue TlValue::boolean(bool v) { TlValue x; x.kind_ = Bool; x.i_ = v ? 1 : 0; return x; }
TlValue TlValue::bytes(const std::string& v) { TlValue x; x.kind_ = Bytes; x.s_ = v; return x; }
TlValue TlValue::bytes(const std::vector<uint8_t>& v)
{
    return bytes(v.empty() ? std::string() : std::string(reinterpret_cast<const char*>(&v[0]), v.size()));
}
TlValue TlValue::vec() { TlValue x; x.kind_ = Vector; return x; }

TlValue TlValue::obj(const std::string& ctorName)
{
    const TlCtorDef* c = tlFind(ctorName);
    if (!c) throw std::runtime_error("TL: unknown constructor " + ctorName);
    return obj(c);
}

TlValue TlValue::obj(const TlCtorDef* c)
{
    TlValue x;
    x.kind_ = Object;
    x.ctor_ = c;
    x.items_.resize(c->count);
    return x;
}

int32_t TlValue::asInt() const { return static_cast<int32_t>(i_); }
int64_t TlValue::asLong() const { return i_; }
double TlValue::asDouble() const { return kind_ == Double ? d_ : static_cast<double>(i_); }
bool TlValue::asBool() const { return kind_ == Bool ? i_ != 0 : kind_ != Null; }

const std::string& TlValue::str() const
{
    static const std::string* empty = new std::string();
    return kind_ == Bytes ? s_ : *empty;
}

std::string TlValue::name() const { return ctor_ ? std::string(ctor_->name) : std::string(); }

bool TlValue::is(const char* ctorName) const
{
    return ctor_ && std::strcmp(ctor_->name, ctorName) == 0;
}

const TlValue& TlValue::operator[](const char* f) const
{
    if (kind_ != Object) return nullValue();
    int p = paramIndex(ctor_, f);
    return p < 0 ? nullValue() : items_[p];
}

TlValue& TlValue::field(const char* f)
{
    int p = kind_ == Object ? paramIndex(ctor_, f) : -1;
    if (p < 0) throw std::runtime_error(std::string("TL: no field ") + f + " in " + name());
    return items_[p];
}

TlValue& TlValue::set(const char* f, const TlValue& v)
{
    field(f) = v;
    return *this;
}

std::string TlValue::dump(int depth) const
{
    std::string out;
    dumpTo(out, *this, depth);
    return out;
}

TlValue tlInt(int32_t v) { return TlValue::i32(v); }
TlValue tlLong(int64_t v) { return TlValue::i64(v); }
TlValue tlStr(const std::string& v) { return TlValue::bytes(v); }
TlValue tlBool(bool v) { return TlValue::boolean(v); }
TlValue tlObj(const std::string& n) { return TlValue::obj(n); }

void tlWrite(TlWriter& w, const TlValue& v, const std::string& type)
{
    TType* t;
    { std::lock_guard<std::mutex> lock(g_typeMutex); t = compileType(reg(), type); }
    writeType(w, v, t);
}

std::vector<uint8_t> tlSerialize(const TlValue& v)
{
    TlWriter w;
    tlWrite(w, v, "Object");
    return w.data();
}

TlValue tlRead(TlReader& r, const std::string& type)
{
    TType* t;
    { std::lock_guard<std::mutex> lock(g_typeMutex); t = compileType(reg(), type); }
    return readType(r, t);
}

TlValue tlParse(const uint8_t* data, std::size_t size, const std::string& type)
{
    TlReader r(data, size);
    return tlRead(r, type);
}

TlValue tlParseResult(const TlValue& function, const uint8_t* data, std::size_t size)
{
    // wrappers (invokeWithLayer, initConnection, ...) return their inner query's type
    const TlValue* f = &function;
    while (f->kind() == TlValue::Object && std::strcmp(f->ctor()->type, "X") == 0 && f->has("query"))
        f = &(*f)["query"];
    std::string type = f->kind() == TlValue::Object ? f->ctor()->type : "Object";
    return tlParse(data, size, type);
}

} // namespace tg
