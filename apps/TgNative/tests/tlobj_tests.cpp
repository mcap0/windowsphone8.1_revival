#include "../core/tlobj.h"
#include "../core/mtproto_rpc.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace tg;

namespace {

int failures = 0;

void check(const std::string& name, bool ok, const std::string& detail = std::string())
{
    if (ok) std::cout << "  ok " << name << '\n';
    else { ++failures; std::cout << "  FAIL " << name << ": " << detail << '\n'; }
}

std::string hex(const std::vector<uint8_t>& v)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) { s += d[v[i] >> 4]; s += d[v[i] & 15]; }
    return s;
}

} // namespace

int runTlObjTests()
{
    failures = 0;
    try {
        check("layer", tlLayer() >= 200);
        check("find by name", tlFind("messages.sendMessage") != 0);
        check("find by id", tlFind(0x997275b5u) && std::string(tlFind(0x997275b5u)->name) == "boolTrue");

        TlValue ping = tlObj("ping").set("ping_id", tlLong(0x1122334455667788LL));
        check("ping bytes", tlSerialize(ping) == buildPing(0x1122334455667788ULL), hex(tlSerialize(ping)));

        TlValue ids = TlValue::vec();
        ids.push(tlLong(5)); ids.push(tlLong(7));
        TlValue ack = tlObj("msgs_ack").set("msg_ids", ids);
        std::vector<uint64_t> raw; raw.push_back(5); raw.push_back(7);
        check("msgs_ack bytes", tlSerialize(ack) == buildMsgsAck(raw), hex(tlSerialize(ack)));

        // flags are computed from the optional fields
        TlValue send = tlObj("messages.sendMessage");
        send.set("peer", tlObj("inputPeerSelf"))
            .set("message", tlStr("ciao"))
            .set("random_id", tlLong(42))
            .set("silent", tlBool(true));
        std::vector<uint8_t> b = tlSerialize(send);
        TlValue back = tlParse(&b[0], b.size(), "Object");
        check("sendMessage round trip name", back.is("messages.sendMessage"), back.dump());
        check("sendMessage message", back["message"].str() == "ciao");
        check("sendMessage random_id", back["random_id"].asLong() == 42);
        check("sendMessage silent flag", back["silent"].asBool() && !back.has("no_webpage") && !back.has("reply_to"));
        check("sendMessage flags value", (static_cast<uint32_t>(back["flags"].asInt()) & (1u << 5)) != 0, back.dump());
        check("sendMessage peer", back["peer"].is("inputPeerSelf"));

        // a message with nested optional objects and a vector
        TlValue msg = tlObj("message");
        msg.set("id", tlInt(9)).set("peer_id", tlObj("peerUser").set("user_id", tlLong(77)))
           .set("date", tlInt(1700000000)).set("message", tlStr("x"))
           .set("from_id", tlObj("peerUser").set("user_id", tlLong(78)));
        TlValue vec = TlValue::vec(); vec.push(msg); vec.push(msg);
        TlWriter w; tlWrite(w, vec, "Vector<Message>");
        TlValue vback = tlParse(&w.data()[0], w.data().size(), "Vector<Message>");
        check("vector of message", vback.size() == 2 && vback.at(1)["from_id"]["user_id"].asLong() == 78, vback.dump());
        check("absent field is null", vback.at(0)["media"].isNull() && vback.at(0)["media"]["x"].isNull());

        // rpc result type of a wrapped call
        TlValue wrapped = tlObj("invokeWithLayer").set("layer", tlInt(tlLayer())).set("query", tlObj("help.getConfig"));
        TlWriter bw; bw.writeBool(true);
        TlValue r = tlParseResult(tlObj("auth.logOut").set("x", TlValue()), &bw.data()[0], bw.data().size());
        check("bool result", false, "unreachable");
        (void)wrapped; (void)r;
    } catch (const std::exception& e) {
        check("unknown field throws", std::string(e.what()).find("no field x") != std::string::npos, e.what());
    }
    try {
        TlWriter bw; bw.writeBool(true);
        TlValue r = tlParseResult(tlObj("account.updateStatus").set("offline", tlBool(false)), &bw.data()[0], bw.data().size());
        check("bool result", r.kind() == TlValue::Bool && r.asBool());

        static const uint8_t gz[] = {0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0xff,0xdb,0x5a,0x5a,0x34,0x13,0x00,0x9b,0xfb,0x08,0xcd,0x04,0x00,0x00,0x00};
        TlWriter g; g.writeConstructor(0x3072cfa1u); g.writeBytes(gz, sizeof(gz));
        TlValue gr = tlParse(&g.data()[0], g.data().size(), "Bool");
        check("gzip_packed", gr.kind() == TlValue::Bool && gr.asBool(), gr.dump());

        TlValue wrapped = tlObj("invokeWithLayer").set("layer", tlInt(tlLayer()))
            .set("query", tlObj("initConnection").set("api_id", tlInt(1)).set("device_model", tlStr("a"))
                 .set("system_version", tlStr("b")).set("app_version", tlStr("c")).set("system_lang_code", tlStr("it"))
                 .set("lang_pack", tlStr("")).set("lang_code", tlStr("it")).set("query", tlObj("help.getConfig")));
        std::vector<uint8_t> wb = tlSerialize(wrapped);
        check("wrapped call size", wb.size() > 40, hex(wb));
        TlWriter cw; cw.writeConstructor(tlFind("boolFalse")->id);
        TlValue wr = tlParseResult(tlObj("invokeWithLayer").set("layer", tlInt(1)).set("query", tlObj("account.updateStatus").set("offline", tlBool(true))),
                                   &cw.data()[0], cw.data().size());
        check("wrapped result type", wr.kind() == TlValue::Bool && !wr.asBool());
    } catch (const std::exception& e) {
        check("tlobj", false, e.what());
    }
    return failures;
}
