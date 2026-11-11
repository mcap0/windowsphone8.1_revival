#include "tlobj.h"

#include "zlib.h"

namespace tg {

bool gunzipBytes(const std::string& in, std::string& out)
{
    z_stream s = z_stream();
    if (inflateInit2(&s, 16 + MAX_WBITS) != Z_OK) return false;
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    s.avail_in = static_cast<uInt>(in.size());
    out.clear();
    char buf[16384];
    int rc;
    do {
        s.next_out = reinterpret_cast<Bytef*>(buf);
        s.avail_out = sizeof(buf);
        rc = inflate(&s, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&s); return false; }
        out.append(buf, sizeof(buf) - s.avail_out);
    } while (rc != Z_STREAM_END);
    inflateEnd(&s);
    return true;
}

} // namespace tg
