#ifndef TG_CORE_TLOBJ_H
#define TG_CORE_TLOBJ_H

// Schema-driven TL values: any constructor of the API layer (core/tl_schema.inc) can be built, serialized
// and parsed without hand-written structs. Objects keep one slot per schema parameter; fields are read by
// name ("message", "peer", ...). Flags (#) are computed on write from the fields that are set.

#include "tl.h"

#include <stdint.h>
#include <string>
#include <vector>

namespace tg {

struct TlParamDef { const char* name; const char* type; };
struct TlCtorDef { uint32_t id; const char* name; const char* type; int first; int count; int isFunction; };

int tlLayer();
const TlCtorDef* tlFind(uint32_t id);
const TlCtorDef* tlFind(const std::string& name);

class TlValue {
public:
    enum Kind { Null, Int, Long, Double, Bool, Bytes, Vector, Object };

    TlValue();
    static TlValue i32(int32_t v);
    static TlValue i64(int64_t v);
    static TlValue dbl(double v);
    static TlValue boolean(bool v);
    static TlValue bytes(const std::string& v);
    static TlValue bytes(const std::vector<uint8_t>& v);
    static TlValue vec();
    static TlValue obj(const std::string& ctorName);   // throws if the name is not in the schema
    static TlValue obj(const TlCtorDef* ctor);

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Null; }
    int32_t asInt() const;       // Int/Long/Bool converted; 0 for Null
    int64_t asLong() const;
    double asDouble() const;
    bool asBool() const;         // true for Bool true and for any non-null non-Bool value
    const std::string& str() const;   // Bytes; empty for others

    // Vector
    std::size_t size() const { return items_.size(); }
    const TlValue& at(std::size_t i) const { return items_[i]; }
    TlValue& push(const TlValue& v) { items_.push_back(v); return items_.back(); }
    const std::vector<TlValue>& items() const { return items_; }

    // Object
    const TlCtorDef* ctor() const { return ctor_; }
    std::string name() const;              // constructor name, "" if not an object
    bool is(const char* ctorName) const;   // exact constructor name
    const TlValue& operator[](const char* field) const;   // Null when absent or not an object
    bool has(const char* field) const { return !(*this)[field].isNull(); }
    TlValue& set(const char* field, const TlValue& v);     // throws on an unknown field
    TlValue& field(const char* field);                    // mutable slot (throws on an unknown field)
    std::vector<TlValue>& slots() { return items_; }       // object slots in schema order

    // Short debug text, e.g. message{id=5 message="hi" ...}
    std::string dump(int depth = 3) const;

private:
    Kind kind_;
    int64_t i_;
    double d_;
    std::string s_;
    std::vector<TlValue> items_;
    const TlCtorDef* ctor_;
};

TlValue tlInt(int32_t v);
TlValue tlLong(int64_t v);
TlValue tlStr(const std::string& v);
TlValue tlBool(bool v);
TlValue tlObj(const std::string& ctorName);

// Serialization. A function call is a boxed Object; "!X" parameters take an Object (written boxed) or
// Bytes (written raw, already serialized).
void tlWrite(TlWriter& w, const TlValue& v, const std::string& type);
std::vector<uint8_t> tlSerialize(const TlValue& boxedObject);

// Parsing. type is a schema type ("Updates", "Vector<User>", "int", ...); "Object" means any boxed object.
// gzip_packed is unpacked transparently wherever a boxed value is expected.
TlValue tlRead(TlReader& r, const std::string& type);
TlValue tlParse(const uint8_t* data, std::size_t size, const std::string& type);
// Parses an rpc_result body using the function's declared result type.
TlValue tlParseResult(const TlValue& function, const uint8_t* data, std::size_t size);

// zlib inflate of gzip data (core/gzip.cpp).
bool gunzipBytes(const std::string& in, std::string& out);

} // namespace tg

#endif
