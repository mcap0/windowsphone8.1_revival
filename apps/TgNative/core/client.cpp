#include "client.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <sstream>

namespace tg {

namespace {

const int kPartSize = 128 * 1024;      // upload
const int kChunk = 512 * 1024;         // download (divides 1 MB, multiple of 4 KB)

std::string num(int64_t v)
{
    char b[32];
    std::sprintf(b, "%lld", static_cast<long long>(v));
    return b;
}

std::string hexOf(const std::string& s)
{
    static const char* d = "0123456789abcdef";
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        o += d[c >> 4];
        o += d[c & 15];
    }
    return o.empty() ? "-" : o;
}

std::string unhex(const std::string& h)
{
    std::string o;
    if (h == "-") return o;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        o += static_cast<char>(std::strtoul(h.substr(i, 2).c_str(), 0, 16));
    return o;
}

std::string mmss(int s)
{
    char b[16];
    std::sprintf(b, "%d:%02d", s / 60, s % 60);
    return b;
}

std::string textOf(const TlValue& v)
{
    // layer 230 uses TextWithEntities in places that were plain strings
    if (v.kind() == TlValue::Object) return v["text"].str();
    return v.str();
}

std::string serviceText(const TlValue& a, const std::string& who)
{
    std::string n = a.name();
    if (n == "messageActionChatCreate") return who + " ha creato il gruppo " + textOf(a["title"]);
    if (n == "messageActionChannelCreate") return "Canale creato";
    if (n == "messageActionChatEditTitle") return who + " ha cambiato il nome in " + textOf(a["title"]);
    if (n == "messageActionChatEditPhoto") return who + " ha cambiato la foto del gruppo";
    if (n == "messageActionChatDeletePhoto") return who + " ha tolto la foto del gruppo";
    if (n == "messageActionChatAddUser") return who + " ha aggiunto un membro";
    if (n == "messageActionChatDeleteUser") return who + " ha rimosso un membro";
    if (n == "messageActionChatJoinedByLink" || n == "messageActionChatJoinedByRequest") return who + " \xc3\xa8 entrato nel gruppo";
    if (n == "messageActionPinMessage") return who + " ha fissato un messaggio";
    if (n == "messageActionPhoneCall") return "Chiamata";
    if (n == "messageActionContactSignUp") return who + " si \xc3\xa8 iscritto a Telegram";
    if (n == "messageActionHistoryClear") return "Cronologia cancellata";
    if (n == "messageActionScreenshotTaken") return who + " ha fatto uno screenshot";
    return "[evento]";
}

} // namespace

Client::Client(Engine* engine, int apiId, const std::string& apiHash)
    : engine_(engine), listener_(0), apiId_(apiId), apiHash_(apiHash), auth_(AuthUnknown), selfId_(0),
      pts_(0), qts_(0), date_(0), seq_(0), syncing_(false), chatsLoaded_(false), sendThread_(0)
{
    rnd_ = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
           reinterpret_cast<uintptr_t>(this);
}

int64_t Client::randomId()
{
    rnd_ ^= rnd_ << 13;
    rnd_ ^= rnd_ >> 7;
    rnd_ ^= rnd_ << 17;
    return static_cast<int64_t>(rnd_ & 0x7fffffffffffffffULL);
}

void Client::error(const std::string& what)
{
    if (listener_) listener_->onError(what);
}

void Client::setAuth(AuthState s)
{
    auth_ = s;
    if (listener_) listener_->onAuthState(s);
}

// ---------------------------------------------------------------- persistence

std::string Client::save() const
{
    std::ostringstream o;
    o << "v 1\nself " << selfId_ << "\npts " << pts_ << " " << qts_ << " " << date_ << " " << seq_ << "\n";
    for (std::map<std::string, Peer>::const_iterator it = peers_.begin(); it != peers_.end(); ++it) {
        const Peer& p = it->second;
        o << "peer " << p.type << " " << p.id << " " << p.accessHash << " " << hexOf(p.name) << " " << (p.isBroadcast ? 1 : 0) << "\n";
    }
    for (std::size_t i = 0; i < chats_.size(); ++i) {
        const ChatItem& c = chats_[i];
        o << "chat " << c.key << " " << hexOf(c.title) << " " << hexOf(c.lastText) << " " << c.lastDate << " " << c.topId << " "
          << c.unread << " " << c.muted << " " << c.pinned << " " << c.isGroup << " " << c.isChannel << " " << c.lastOut << " "
          << c.readInboxMax << " " << c.readOutboxMax << "\n";
    }
    return o.str();
}

void Client::load(const std::string& blob)
{
    std::istringstream in(blob);
    std::string line;
    chats_.clear();
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string tag;
        l >> tag;
        if (tag == "self") { long long v = 0; l >> v; selfId_ = v; }
        else if (tag == "pts") l >> pts_ >> qts_ >> date_ >> seq_;
        else if (tag == "peer") {
            Peer p;
            long long id = 0, h = 0;
            std::string name;
            int b = 0;
            l >> p.type >> id >> h >> name >> b;
            p.id = id;
            p.accessHash = h;
            p.name = unhex(name);
            p.isBroadcast = b != 0;
            std::string key = (p.type == 1 ? "u" : p.type == 2 ? "c" : "h") + num(p.id);
            peers_[key] = p;
        } else if (tag == "chat") {
            ChatItem c;
            std::string t, last;
            l >> c.key >> t >> last >> c.lastDate >> c.topId >> c.unread >> c.muted >> c.pinned >> c.isGroup >> c.isChannel
              >> c.lastOut >> c.readInboxMax >> c.readOutboxMax;
            c.title = unhex(t);
            c.lastText = unhex(last);
            if (!c.key.empty()) chats_.push_back(c);
        }
    }
}

// ---------------------------------------------------------------- peers

std::string Client::peerKey(const TlValue& p) const
{
    if (p.is("peerUser")) return "u" + num(p["user_id"].asLong());
    if (p.is("peerChat")) return "c" + num(p["chat_id"].asLong());
    if (p.is("peerChannel")) return "h" + num(p["channel_id"].asLong());
    return std::string();
}

std::string Client::selfKey() const { return "u" + num(selfId_); }

TlValue Client::inputPeer(const std::string& key) const
{
    if (key.size() < 2) return tlObj("inputPeerEmpty");
    int64_t id = std::strtoll(key.c_str() + 1, 0, 10);
    std::map<std::string, Peer>::const_iterator it = peers_.find(key);
    int64_t hash = it == peers_.end() ? 0 : it->second.accessHash;
    if (key[0] == 'u') {
        if (id == selfId_) return tlObj("inputPeerSelf");
        return tlObj("inputPeerUser").set("user_id", tlLong(id)).set("access_hash", tlLong(hash));
    }
    if (key[0] == 'c') return tlObj("inputPeerChat").set("chat_id", tlLong(id));
    return tlObj("inputPeerChannel").set("channel_id", tlLong(id)).set("access_hash", tlLong(hash));
}

TlValue Client::inputUser(const std::string& key) const
{
    TlValue p = inputPeer(key);
    if (p.is("inputPeerSelf")) return tlObj("inputUserSelf");
    return tlObj("inputUser").set("user_id", p["user_id"]).set("access_hash", p["access_hash"]);
}

std::string Client::nameOf(const std::string& key) const
{
    std::map<std::string, Peer>::const_iterator it = peers_.find(key);
    if (it != peers_.end() && !it->second.name.empty()) return it->second.name;
    return key.size() > 1 && key[0] == 'u' ? "Utente" : "Chat";
}

void Client::absorbUser(const TlValue& u)
{
    if (!u.is("user")) return;
    std::string key = "u" + num(u["id"].asLong());
    Peer& p = peers_[key];
    p.type = 1;
    p.id = u["id"].asLong();
    if (u.has("access_hash") && (!u["min"].asBool() || p.accessHash == 0)) p.accessHash = u["access_hash"].asLong();
    std::string name = u["first_name"].str();
    if (!u["last_name"].str().empty()) name += (name.empty() ? "" : " ") + u["last_name"].str();
    if (u["deleted"].asBool()) name = "Account eliminato";
    if (name.empty()) name = u["phone"].str().empty() ? "Utente" : "+" + u["phone"].str();
    if (!u["min"].asBool() || p.name.empty()) p.name = name;
    if (u["self"].asBool()) selfId_ = p.id;
}

void Client::absorbChat(const TlValue& c)
{
    std::string n = c.name();
    if (n == "chat" || n == "chatForbidden") {
        Peer& p = peers_["c" + num(c["id"].asLong())];
        p.type = 2;
        p.id = c["id"].asLong();
        p.name = c["title"].str();
    } else if (n == "channel" || n == "channelForbidden") {
        Peer& p = peers_["h" + num(c["id"].asLong())];
        p.type = 3;
        p.id = c["id"].asLong();
        if (c.has("access_hash") && (!c["min"].asBool() || p.accessHash == 0)) p.accessHash = c["access_hash"].asLong();
        p.name = c["title"].str();
        p.isBroadcast = c["broadcast"].asBool();
        if (!c["min"].asBool()) p.isForum = c["forum"].asBool();
    }
}

void Client::absorbUsersChats(const TlValue& r)
{
    const TlValue& users = r["users"];
    for (std::size_t i = 0; i < users.size(); ++i) absorbUser(users.at(i));
    const TlValue& chats = r["chats"];
    for (std::size_t i = 0; i < chats.size(); ++i) absorbChat(chats.at(i));
}

// ---------------------------------------------------------------- messages

std::string Client::registerPhoto(const TlValue& photo, const std::string& want, const std::string& chatKey, int32_t msgId,
                                  int64_t* sizeOut)
{
    if (!photo.is("photo")) return std::string();
    const TlValue& sizes = photo["sizes"];
    const TlValue* best = 0;
    int bestW = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const TlValue& s = sizes.at(i);
        if (!s.is("photoSize") && !s.is("photoSizeProgressive")) continue;
        int w = std::max(s["w"].asInt(), s["h"].asInt());
        bool better;
        if (want == "thumb") better = !best || (w >= 160 && (bestW < 160 || w < bestW));
        else better = !best || (w <= 1280 && w > bestW) || (bestW > 1280 && w < bestW);
        if (better) { best = &s; bestW = w; }
    }
    if (!best) return std::string();
    std::string type = (*best)["type"].str();
    Media m;
    m.location = tlObj("inputPhotoFileLocation")
                     .set("id", photo["id"]).set("access_hash", photo["access_hash"])
                     .set("file_reference", photo["file_reference"]).set("thumb_size", tlStr(type));
    m.dc = photo["dc_id"].asInt();
    if (best->is("photoSize")) m.size = (*best)["size"].asInt();
    else if ((*best)["sizes"].size()) m.size = (*best)["sizes"].at((*best)["sizes"].size() - 1).asInt();
    m.chatKey = chatKey;
    m.msgId = msgId;
    std::string key = "p" + num(photo["id"].asLong()) + "_" + type;
    media_[key] = m;
    if (sizeOut) *sizeOut = m.size;
    return key;
}

std::string Client::registerDocument(const TlValue& doc, const std::string& chatKey, int32_t msgId)
{
    if (!doc.is("document")) return std::string();
    Media m;
    m.location = tlObj("inputDocumentFileLocation")
                     .set("id", doc["id"]).set("access_hash", doc["access_hash"])
                     .set("file_reference", doc["file_reference"]).set("thumb_size", tlStr(""));
    m.dc = doc["dc_id"].asInt();
    m.size = doc["size"].asLong();
    m.chatKey = chatKey;
    m.msgId = msgId;
    std::string key = "d" + num(doc["id"].asLong());
    media_[key] = m;
    return key;
}

std::string Client::registerDocumentThumb(const TlValue& doc, const std::string& chatKey, int32_t msgId)
{
    const TlValue& thumbs = doc["thumbs"];
    const TlValue* best = 0;
    for (std::size_t i = 0; i < thumbs.size(); ++i) {
        const TlValue& s = thumbs.at(i);
        if (s.is("photoSize") || s.is("photoSizeProgressive")) { best = &s; if (s["type"].str() == "m") break; }
    }
    if (!best) return std::string();
    Media m;
    m.location = tlObj("inputDocumentFileLocation")
                     .set("id", doc["id"]).set("access_hash", doc["access_hash"])
                     .set("file_reference", doc["file_reference"]).set("thumb_size", (*best)["type"]);
    m.dc = doc["dc_id"].asInt();
    m.size = best->is("photoSize") ? (*best)["size"].asInt() : 0;
    m.chatKey = chatKey;
    m.msgId = msgId;
    std::string key = "d" + num(doc["id"].asLong()) + "_" + (*best)["type"].str();
    media_[key] = m;
    return key;
}

namespace {
void parseReactions(const TlValue& mr, std::vector<Reaction>& out)
{
    out.clear();
    if (!mr.is("messageReactions")) return;
    const TlValue& rs = mr["results"];
    for (std::size_t i = 0; i < rs.size(); ++i) {
        const TlValue& rc = rs.at(i);
        Reaction x;
        const TlValue& re = rc["reaction"];
        if (re.is("reactionEmoji")) x.emoji = re["emoticon"].str();
        else if (re.is("reactionCustomEmoji")) x.emoji = "\xE2\x98\x85";     // ★
        else if (re.is("reactionPaid")) x.emoji = "\xE2\xAD\x90";            // ⭐
        else continue;
        x.count = rc["count"].asInt();
        x.mine = rc.has("chosen_order");
        out.push_back(x);
    }
}

std::vector<MsgItem> messagesOf(Client* c, const TlValue& r)
{
    std::vector<MsgItem> out;
    const TlValue& msgs = r["messages"];
    for (std::size_t i = msgs.size(); i-- > 0;) {
        if (msgs.at(i).is("messageEmpty")) continue;
        out.push_back(c->convertMessage(msgs.at(i)));
    }
    return out;
}
}

MsgItem Client::convertMessage(const TlValue& m)
{
    MsgItem r;
    r.id = m["id"].asInt();
    r.chatKey = peerKey(m["peer_id"]);
    r.out = m["out"].asBool();
    r.date = m["date"].asInt();
    r.editDate = m["edit_date"].asInt();
    if (m.has("from_id")) r.fromKey = peerKey(m["from_id"]);
    else r.fromKey = r.out ? selfKey() : r.chatKey;
    if (r.chatKey == selfKey()) r.out = true;   // Saved Messages
    r.fromName = r.out ? std::string("Tu") : nameOf(r.fromKey);
    const TlValue& rh = m["reply_to"];
    if (rh.is("messageReplyHeader")) {
        r.replyTo = rh["reply_to_msg_id"].asInt();
        int32_t top = rh["reply_to_top_id"].asInt();
        if (top) r.threadId = top;
        else if (rh["forum_topic"].asBool()) { r.threadId = r.replyTo; r.replyTo = 0; }   // first level of a topic
        r.replyText = rh["quote_text"].str();
        if (rh.has("reply_from")) {
            const TlValue& f = rh["reply_from"];
            r.replyFrom = f.has("from_name") ? f["from_name"].str() : f.has("from_id") ? nameOf(peerKey(f["from_id"])) : "";
        }
        if (r.replyFrom.empty() && rh.has("reply_to_peer_id")) r.replyFrom = nameOf(peerKey(rh["reply_to_peer_id"]));
    }
    parseReactions(m["reactions"], r.reactions);
    if (m["replies"].is("messageReplies")) {
        r.replies = m["replies"]["replies"].asInt();
        r.hasComments = m["replies"]["comments"].asBool();
    }
    if (m.has("fwd_from")) {
        const TlValue& f = m["fwd_from"];
        r.forwardFrom = f.has("from_name") ? f["from_name"].str() : f.has("from_id") ? nameOf(peerKey(f["from_id"])) : "";
        if (r.forwardFrom.empty()) r.forwardFrom = "?";
    }
    if (m.is("messageService")) {
        r.kind = MsgItem::Service;
        r.text = serviceText(m["action"], r.out ? "Tu" : r.fromName);
        return r;
    }
    if (!m.is("message")) {
        r.kind = MsgItem::Other;
        return r;
    }
    r.text = m["message"].str();
    const TlValue& md = m["media"];
    std::string mn = md.name();
    if (mn.empty() || mn == "messageMediaEmpty" || mn == "messageMediaWebPage") return r;
    if (mn == "messageMediaPhoto") {
        r.kind = MsgItem::Photo;
        const TlValue& ph = md["photo"];
        r.mediaKey = registerPhoto(ph, "big", r.chatKey, r.id, &r.size);
        r.thumbKey = registerPhoto(ph, "thumb", r.chatKey, r.id, 0);
        const TlValue& sizes = ph["sizes"];
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            const TlValue& s = sizes.at(i);
            if (s.is("photoStrippedSize")) r.strippedJpeg = strippedToJpeg(s["bytes"].str());
            if ((s.is("photoSize") || s.is("photoSizeProgressive")) && s["w"].asInt() >= r.width) {
                r.width = s["w"].asInt();
                r.height = s["h"].asInt();
            }
        }
        if (!ph.is("photo")) r.kind = MsgItem::Other;
        return r;
    }
    if (mn == "messageMediaDocument") {
        const TlValue& d = md["document"];
        if (!d.is("document")) { r.kind = MsgItem::Other; return r; }
        r.kind = MsgItem::Document;
        r.mediaKey = registerDocument(d, r.chatKey, r.id);
        r.thumbKey = registerDocumentThumb(d, r.chatKey, r.id);
        r.size = d["size"].asLong();
        r.mime = d["mime_type"].str();
        const TlValue& th = d["thumbs"];
        for (std::size_t i = 0; i < th.size(); ++i)
            if (th.at(i).is("photoStrippedSize")) r.strippedJpeg = strippedToJpeg(th.at(i)["bytes"].str());
        bool animated = false;
        const TlValue& at = d["attributes"];
        for (std::size_t i = 0; i < at.size(); ++i) {
            const TlValue& a = at.at(i);
            std::string an = a.name();
            if (an == "documentAttributeFilename") r.fileName = a["file_name"].str();
            else if (an == "documentAttributeAudio") {
                r.kind = a["voice"].asBool() ? MsgItem::Voice : MsgItem::Audio;
                r.duration = a["duration"].asInt();
                r.waveform = a["waveform"].str();
                if (r.kind == MsgItem::Audio && r.fileName.empty()) r.fileName = a["performer"].str() + " - " + a["title"].str();
            } else if (an == "documentAttributeVideo") {
                if (r.kind != MsgItem::Sticker) r.kind = a["round_message"].asBool() ? MsgItem::VideoNote : MsgItem::Video;
                r.duration = static_cast<int32_t>(a["duration"].asDouble());
                r.width = a["w"].asInt();
                r.height = a["h"].asInt();
            } else if (an == "documentAttributeSticker" || an == "documentAttributeCustomEmoji") {
                r.kind = MsgItem::Sticker;
                r.emoji = a["alt"].str();
            } else if (an == "documentAttributeAnimated") {
                animated = true;
            } else if (an == "documentAttributeImageSize") {
                r.width = a["w"].asInt();
                r.height = a["h"].asInt();
            }
        }
        if (animated && r.kind != MsgItem::Sticker) r.kind = MsgItem::Gif;
        if (md["voice"].asBool()) r.kind = MsgItem::Voice;
        if (md["round"].asBool()) r.kind = MsgItem::VideoNote;
        return r;
    }
    if (mn == "messageMediaGeo" || mn == "messageMediaGeoLive" || mn == "messageMediaVenue") {
        r.kind = MsgItem::Location;
        const TlValue& g = md["geo"];
        char b[64];
        std::sprintf(b, "%.6f,%.6f", g["lat"].asDouble(), g["long"].asDouble());
        r.mediaKey = std::string("geo:") + b;
        if (mn == "messageMediaVenue") r.text = md["title"].str() + "\n" + md["address"].str();
        return r;
    }
    if (mn == "messageMediaContact") {
        r.kind = MsgItem::Contact;
        r.text = md["first_name"].str() + " " + md["last_name"].str() + "\n+" + md["phone_number"].str();
        return r;
    }
    if (mn == "messageMediaPoll") {
        r.kind = MsgItem::Poll;
        const TlValue& poll = md["poll"];
        r.text = textOf(poll["question"]);
        const TlValue& ans = poll["answers"];
        for (std::size_t i = 0; i < ans.size(); ++i) r.text += "\n- " + textOf(ans.at(i)["text"]);
        return r;
    }
    if (mn == "messageMediaDice") {
        r.text = md["emoticon"].str() + " " + num(md["value"].asInt());
        return r;
    }
    r.kind = MsgItem::Other;
    return r;
}

std::string Client::previewText(const MsgItem& m)
{
    std::string cap = m.text.empty() ? "" : " " + m.text;
    switch (m.kind) {
    case MsgItem::Photo: return "[Foto]" + cap;
    case MsgItem::Voice: return "[Messaggio vocale " + mmss(m.duration) + "]";
    case MsgItem::Audio: return "[Audio] " + m.fileName;
    case MsgItem::Video: return "[Video]" + cap;
    case MsgItem::VideoNote: return "[Videomessaggio]";
    case MsgItem::Document: return "[File] " + m.fileName + cap;
    case MsgItem::Sticker: return m.emoji + " [Sticker]";
    case MsgItem::Gif: return "[GIF]" + cap;
    case MsgItem::Location: return "[Posizione]";
    case MsgItem::Contact: return "[Contatto]";
    case MsgItem::Poll: return "[Sondaggio] " + m.text.substr(0, m.text.find('\n'));
    case MsgItem::Other: return m.text.empty() ? "[Messaggio non supportato]" : m.text;
    default: return m.text;
    }
}

// ---------------------------------------------------------------- chats

const ChatItem* Client::chat(const std::string& key) const
{
    for (std::size_t i = 0; i < chats_.size(); ++i)
        if (chats_[i].key == key) return &chats_[i];
    return 0;
}

ChatItem* Client::chatMut(const std::string& key)
{
    for (std::size_t i = 0; i < chats_.size(); ++i)
        if (chats_[i].key == key) return &chats_[i];
    return 0;
}

namespace {
bool chatOrder(const ChatItem& a, const ChatItem& b)
{
    if (a.pinned != b.pinned) return a.pinned;
    return a.lastDate > b.lastDate;
}
}

void Client::sortChats()
{
    std::stable_sort(chats_.begin(), chats_.end(), chatOrder);
}

void Client::loadChats(const std::function<void(const RpcErr&)>& done, int limit)
{
    TlValue q = tlObj("messages.getDialogs");
    q.set("offset_date", tlInt(0)).set("offset_id", tlInt(0)).set("offset_peer", tlObj("inputPeerEmpty"))
     .set("limit", tlInt(limit)).set("hash", tlLong(0));
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        if (!e.ok()) { if (done) done(e); return; }
        if (r.is("messages.dialogsNotModified")) { if (done) done(e); return; }
        absorbUsersChats(r);
        std::map<std::string, MsgItem> tops;
        const TlValue& msgs = r["messages"];
        for (std::size_t i = 0; i < msgs.size(); ++i) {
            MsgItem m = convertMessage(msgs.at(i));
            tops[m.chatKey + "/" + num(m.id)] = m;
        }
        std::vector<ChatItem> list;
        int32_t now = static_cast<int32_t>(std::time(0));
        const TlValue& ds = r["dialogs"];
        for (std::size_t i = 0; i < ds.size(); ++i) {
            const TlValue& d = ds.at(i);
            if (!d.is("dialog")) continue;
            ChatItem c;
            c.key = peerKey(d["peer"]);
            c.title = nameOf(c.key);
            c.topId = d["top_message"].asInt();
            c.unread = d["unread_count"].asInt();
            c.readInboxMax = d["read_inbox_max_id"].asInt();
            c.readOutboxMax = d["read_outbox_max_id"].asInt();
            c.pinned = d["pinned"].asBool();
            c.muted = d["notify_settings"]["mute_until"].asInt() > now;
            const Peer* p = peers_.count(c.key) ? &peers_[c.key] : 0;
            c.isChannel = c.key[0] == 'h' && p && p->isBroadcast;
            c.isGroup = c.key[0] == 'c' || (c.key[0] == 'h' && !c.isChannel);
            c.isForum = p && p->isForum;
            std::map<std::string, MsgItem>::iterator t = tops.find(c.key + "/" + num(c.topId));
            if (t != tops.end()) {
                c.lastText = previewText(t->second);
                if (c.isGroup && !t->second.out && t->second.kind != MsgItem::Service)
                    c.lastText = t->second.fromName.substr(0, t->second.fromName.find(' ')) + ": " + c.lastText;
                c.lastDate = t->second.date;
                c.lastOut = t->second.out;
            }
            list.push_back(c);
        }
        chats_.swap(list);
        chatsLoaded_ = true;
        if (listener_) listener_->onChatsChanged();
        if (done) done(e);
    });
}

void Client::refreshChats(const std::vector<std::string>& keys, const std::function<void(const RpcErr&)>& done)
{
    TlValue peers = TlValue::vec();
    for (std::size_t i = 0; i < keys.size(); ++i) peers.push(tlObj("inputDialogPeer").set("peer", inputPeer(keys[i])));
    if (peers.size() == 0) { if (done) done(RpcErr()); return; }
    engine_->invoke(tlObj("messages.getPeerDialogs").set("peers", peers), [this, done](const TlValue& r, const RpcErr& e) {
        if (!e.ok()) { if (done) done(e); return; }
        absorbUsersChats(r);
        std::map<std::string, MsgItem> tops;
        const TlValue& msgs = r["messages"];
        for (std::size_t i = 0; i < msgs.size(); ++i) {
            MsgItem m = convertMessage(msgs.at(i));
            tops[m.chatKey + "/" + num(m.id)] = m;
        }
        int32_t now = static_cast<int32_t>(std::time(0));
        const TlValue& ds = r["dialogs"];
        for (std::size_t i = 0; i < ds.size(); ++i) {
            const TlValue& d = ds.at(i);
            if (!d.is("dialog")) continue;
            std::string key = peerKey(d["peer"]);
            ChatItem* c = chatMut(key);
            if (!c) continue;
            int32_t oldTop = c->topId;
            c->unread = d["unread_count"].asInt();
            c->readInboxMax = d["read_inbox_max_id"].asInt();
            c->readOutboxMax = d["read_outbox_max_id"].asInt();
            c->muted = d["notify_settings"]["mute_until"].asInt() > now;
            std::map<std::string, MsgItem>::iterator t = tops.find(key + "/" + num(d["top_message"].asInt()));
            if (t != tops.end() && t->second.id > oldTop) {
                updateChatFromMessage(t->second, false);
                if (listener_) listener_->onNewMessage(t->second);
            }
        }
        if (listener_) listener_->onChatsChanged();
        if (done) done(e);
    });
}

void Client::loadHistory(const std::string& chatKey, int32_t beforeId, int limit,
                         const std::function<void(const std::vector<MsgItem>&, const RpcErr&)>& done)
{
    TlValue q = tlObj("messages.getHistory");
    q.set("peer", inputPeer(chatKey)).set("offset_id", tlInt(beforeId)).set("offset_date", tlInt(0))
     .set("add_offset", tlInt(0)).set("limit", tlInt(limit)).set("max_id", tlInt(0)).set("min_id", tlInt(0))
     .set("hash", tlLong(0));
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        std::vector<MsgItem> out;
        if (e.ok()) {
            absorbUsersChats(r);
            const TlValue& msgs = r["messages"];
            for (std::size_t i = msgs.size(); i-- > 0;) {
                if (msgs.at(i).is("messageEmpty")) continue;
                out.push_back(convertMessage(msgs.at(i)));
            }
        }
        done(out, e);
    });
}

// reply_to of a message sent now: the quoted message, inside the current thread (topic or comments) if any.
// General (topic 1) is no thread for the server.
TlValue Client::replyHeader(int32_t replyTo) const
{
    int32_t thread = sendThread_ > 1 ? sendThread_ : 0;
    if (!replyTo && !thread) return TlValue();
    TlValue r = tlObj("inputReplyToMessage").set("reply_to_msg_id", tlInt(replyTo ? replyTo : thread));
    if (thread) r.set("top_msg_id", tlInt(thread));
    return r;
}

void Client::getMessages(const std::string& chatKey, const std::vector<int32_t>& ids, const MessagesCallback& done)
{
    TlValue list = TlValue::vec();
    for (std::size_t i = 0; i < ids.size(); ++i) list.push(tlObj("inputMessageID").set("id", tlInt(ids[i])));
    TlValue q;
    if (!chatKey.empty() && chatKey[0] == 'h') {
        TlValue p = inputPeer(chatKey);
        q = tlObj("channels.getMessages").set("channel",
            tlObj("inputChannel").set("channel_id", p["channel_id"]).set("access_hash", p["access_hash"])).set("id", list);
    } else {
        q = tlObj("messages.getMessages").set("id", list);
    }
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        std::vector<MsgItem> out;
        if (e.ok()) { absorbUsersChats(r); out = messagesOf(this, r); }
        done(out, e);
    });
}

void Client::loadThread(const std::string& chatKey, int32_t threadId, int32_t beforeId, int limit, const MessagesCallback& done)
{
    if (threadId <= 1) {
        // General: the chat's history without the messages of the other topics
        loadHistory(chatKey, beforeId, limit, [done](const std::vector<MsgItem>& page, const RpcErr& e) {
            std::vector<MsgItem> out;
            for (std::size_t i = 0; i < page.size(); ++i) if (page[i].threadId <= 1) out.push_back(page[i]);
            done(out, e);
        });
        return;
    }
    TlValue q = tlObj("messages.getReplies");
    q.set("peer", inputPeer(chatKey)).set("msg_id", tlInt(threadId)).set("offset_id", tlInt(beforeId))
     .set("offset_date", tlInt(0)).set("add_offset", tlInt(0)).set("limit", tlInt(limit)).set("max_id", tlInt(0))
     .set("min_id", tlInt(0)).set("hash", tlLong(0));
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        std::vector<MsgItem> out;
        if (e.ok()) { absorbUsersChats(r); out = messagesOf(this, r); }
        done(out, e);
    });
}

void Client::openComments(const std::string& chatKey, int32_t msgId,
                          const std::function<void(const std::string&, int32_t, const RpcErr&)>& done)
{
    TlValue q = tlObj("messages.getDiscussionMessage").set("peer", inputPeer(chatKey)).set("msg_id", tlInt(msgId));
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        if (!e.ok()) { done("", 0, e); return; }
        absorbUsersChats(r);
        const TlValue& msgs = r["messages"];
        std::string key;
        int32_t first = 0;
        for (std::size_t i = 0; i < msgs.size(); ++i) {   // an album is several messages: the thread is the first
            int32_t id = msgs.at(i)["id"].asInt();
            if (!first || id < first) { first = id; key = peerKey(msgs.at(i)["peer_id"]); }
        }
        RpcErr none;
        if (!first) { none.code = 400; none.message = "nessuna discussione"; }
        done(key, first, none);
    });
}

void Client::loadTopics(const std::string& chatKey, const std::function<void(const std::vector<TopicItem>&, const RpcErr&)>& done)
{
    TlValue q = tlObj("messages.getForumTopics");
    q.set("peer", inputPeer(chatKey)).set("offset_date", tlInt(0)).set("offset_id", tlInt(0)).set("offset_topic", tlInt(0)).set("limit", tlInt(100));
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        std::vector<TopicItem> out;
        if (e.ok()) {
            absorbUsersChats(r);
            std::map<int32_t, MsgItem> tops;
            std::vector<MsgItem> msgs = messagesOf(this, r);
            for (std::size_t i = 0; i < msgs.size(); ++i) tops[msgs[i].id] = msgs[i];
            const TlValue& ts = r["topics"];
            for (std::size_t i = 0; i < ts.size(); ++i) {
                const TlValue& t = ts.at(i);
                if (!t.is("forumTopic")) continue;
                TopicItem x;
                x.id = t["id"].asInt();
                x.title = t["title"].str();
                x.unread = t["unread_count"].asInt();
                x.pinned = t["pinned"].asBool();
                x.closed = t["closed"].asBool();
                x.lastDate = t["date"].asInt();
                std::map<int32_t, MsgItem>::iterator m = tops.find(t["top_message"].asInt());
                if (m != tops.end()) {
                    x.lastText = previewText(m->second);
                    if (!m->second.out && m->second.kind != MsgItem::Service && !m->second.fromName.empty())
                        x.lastText = m->second.fromName.substr(0, m->second.fromName.find(' ')) + ": " + x.lastText;
                    x.lastDate = m->second.date;
                }
                out.push_back(x);
            }
        }
        done(out, e);
    });
}

void Client::markThreadRead(const std::string& chatKey, int32_t threadId, int32_t maxId)
{
    if (threadId <= 1) { markRead(chatKey, maxId); return; }
    engine_->invoke(tlObj("messages.readDiscussion").set("peer", inputPeer(chatKey)).set("msg_id", tlInt(threadId))
                    .set("read_max_id", tlInt(maxId)), [](const TlValue&, const RpcErr&) {});
}

void Client::sendReaction(const std::string& chatKey, int32_t msgId, const std::string& emoji,
                          const std::function<void(const RpcErr&)>& done)
{
    TlValue q = tlObj("messages.sendReaction").set("peer", inputPeer(chatKey)).set("msg_id", tlInt(msgId));
    if (!emoji.empty()) {   // no reaction field = take mine away
        TlValue list = TlValue::vec();
        list.push(tlObj("reactionEmoji").set("emoticon", tlStr(emoji)));
        q.set("reaction", list);
    }
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        if (e.ok()) onUpdates(r);
        if (done) done(e);
    });
}

void Client::markRead(const std::string& chatKey, int32_t maxId)
{
    ChatItem* c = chatMut(chatKey);
    if (c) {
        c->unread = 0;
        if (maxId > c->readInboxMax) c->readInboxMax = maxId;
        if (listener_) listener_->onChatsChanged();
    }
    if (chatKey.empty()) return;
    if (chatKey[0] == 'h') {
        TlValue p = inputPeer(chatKey);
        TlValue ch = tlObj("inputChannel").set("channel_id", p["channel_id"]).set("access_hash", p["access_hash"]);
        engine_->invoke(tlObj("channels.readHistory").set("channel", ch).set("max_id", tlInt(maxId)),
                        [](const TlValue&, const RpcErr&) {});
    } else {
        engine_->invoke(tlObj("messages.readHistory").set("peer", inputPeer(chatKey)).set("max_id", tlInt(maxId)),
                        [this](const TlValue& r, const RpcErr& e) {
                            if (e.ok()) checkPts(r["pts"].asInt(), r["pts_count"].asInt());
                        });
    }
}

namespace {
ContactItem contactFromUser(const TlValue& u, const std::string& name)
{
    ContactItem c;
    c.key = "u" + num(u["id"].asLong());
    c.name = name;
    c.username = u["username"].str();
    c.phone = u["phone"].str();
    return c;
}

bool contactLess(const ContactItem& a, const ContactItem& b) { return a.name < b.name; }
}

void Client::loadContacts(const ContactsCallback& done)
{
    engine_->invoke(tlObj("contacts.getContacts").set("hash", tlLong(0)), [this, done](const TlValue& r, const RpcErr& e) {
        std::vector<ContactItem> out;
        if (e.ok() && r.is("contacts.contacts")) {
            absorbUsersChats(r);
            const TlValue& users = r["users"];
            for (std::size_t i = 0; i < users.size(); ++i) {
                const TlValue& u = users.at(i);
                if (!u.is("user") || u["self"].asBool() || u["deleted"].asBool()) continue;
                out.push_back(contactFromUser(u, nameOf("u" + num(u["id"].asLong()))));
            }
            std::sort(out.begin(), out.end(), contactLess);
        }
        if (done) done(out, e);
    });
}

void Client::searchPeople(const std::string& query, const ContactsCallback& done)
{
    std::string q = !query.empty() && query[0] == '@' ? query.substr(1) : query;
    TlValue req = tlObj("contacts.search").set("q", tlStr(q)).set("limit", tlInt(30));
    engine_->invoke(req, [this, done](const TlValue& r, const RpcErr& e) {
        std::vector<ContactItem> out;
        if (e.ok()) {
            absorbUsersChats(r);
            std::map<std::string, ContactItem> users;
            const TlValue& us = r["users"];
            for (std::size_t i = 0; i < us.size(); ++i)
                if (us.at(i).is("user")) {
                    ContactItem c = contactFromUser(us.at(i), nameOf("u" + num(us.at(i)["id"].asLong())));
                    users[c.key] = c;
                }
            const char* lists[] = { "my_results", "results" };
            std::set<std::string> seen;
            for (int l = 0; l < 2; ++l) {
                const TlValue& peers = r[lists[l]];
                for (std::size_t i = 0; i < peers.size(); ++i) {
                    std::string key = peerKey(peers.at(i));
                    if (key.empty() || !seen.insert(key).second) continue;
                    std::map<std::string, ContactItem>::iterator u = users.find(key);
                    if (u != users.end()) { out.push_back(u->second); continue; }
                    ContactItem c;
                    c.key = key;
                    c.name = nameOf(key);
                    out.push_back(c);
                }
            }
        }
        if (done) done(out, e);
    });
}

void Client::setMuted(const std::string& chatKey, bool muted, const std::function<void(const RpcErr&)>& done)
{
    TlValue s = tlObj("inputPeerNotifySettings").set("mute_until", tlInt(muted ? INT_MAX : 0));
    TlValue q = tlObj("account.updateNotifySettings")
                    .set("peer", tlObj("inputNotifyPeer").set("peer", inputPeer(chatKey))).set("settings", s);
    engine_->invoke(q, [this, chatKey, muted, done](const TlValue&, const RpcErr& e) {
        if (e.ok()) {
            ChatItem* c = chatMut(chatKey);
            if (c) c->muted = muted;
            if (listener_) listener_->onChatsChanged();
        }
        if (done) done(e);
    });
}

void Client::updateChatFromMessage(const MsgItem& m, bool isNew)
{
    if (m.chatKey.empty()) return;
    ChatItem* c = chatMut(m.chatKey);
    if (!c) {
        ChatItem n;
        n.key = m.chatKey;
        n.title = nameOf(m.chatKey);
        std::map<std::string, Peer>::iterator p = peers_.find(m.chatKey);
        n.isChannel = m.chatKey[0] == 'h' && p != peers_.end() && p->second.isBroadcast;
        n.isGroup = m.chatKey[0] == 'c' || (m.chatKey[0] == 'h' && !n.isChannel);
        chats_.push_back(n);
        c = &chats_.back();
    }
    if (m.id >= c->topId) {
        c->topId = m.id;
        c->lastText = previewText(m);
        if (c->isGroup && !m.out && m.kind != MsgItem::Service)
            c->lastText = m.fromName.substr(0, m.fromName.find(' ')) + ": " + c->lastText;
        c->lastDate = m.date;
        c->lastOut = m.out;
    }
    if (isNew && !m.out && m.id > c->readInboxMax) c->unread++;
    sortChats();
    if (listener_) listener_->onChatsChanged();
}

// ---------------------------------------------------------------- updates

void Client::start()
{
    if (engine_->authorized()) {
        setAuth(Ready);
        loadChats([this](const RpcErr& e) {
            if (!e.ok()) { error("Elenco chat: " + e.message); return; }
            if (pts_ == 0) getState();
            else getDifference();
        });
    } else {
        setAuth(WaitPhone);
    }
}

void Client::getState()
{
    engine_->invoke(tlObj("updates.getState"), [this](const TlValue& s, const RpcErr& e) {
        if (!e.ok()) return;
        pts_ = s["pts"].asInt();
        qts_ = s["qts"].asInt();
        date_ = s["date"].asInt();
        seq_ = s["seq"].asInt();
    });
}

void Client::getDifference()
{
    if (syncing_ || auth_ != Ready) return;
    if (pts_ == 0) { getState(); return; }
    syncing_ = true;
    TlValue q = tlObj("updates.getDifference");
    q.set("pts", tlInt(pts_)).set("date", tlInt(date_)).set("qts", tlInt(qts_));
    engine_->invoke(q, [this](const TlValue& d, const RpcErr& e) {
        syncing_ = false;
        if (!e.ok()) return;
        if (d.is("updates.differenceEmpty")) {
            date_ = d["date"].asInt();
            seq_ = d["seq"].asInt();
            return;
        }
        if (d.is("updates.differenceTooLong")) {
            pts_ = d["pts"].asInt();
            loadChats(std::function<void(const RpcErr&)>());
            return;
        }
        absorbUsersChats(d);
        const TlValue& msgs = d["new_messages"];
        for (std::size_t i = 0; i < msgs.size(); ++i) applyMessage(msgs.at(i), true);
        syncing_ = true;   // other_updates: no pts checks while applying a difference
        const TlValue& ups = d["other_updates"];
        for (std::size_t i = 0; i < ups.size(); ++i) applyUpdate(ups.at(i));
        syncing_ = false;
        const TlValue& st = d.is("updates.differenceSlice") ? d["intermediate_state"] : d["state"];
        pts_ = st["pts"].asInt();
        qts_ = st["qts"].asInt();
        date_ = st["date"].asInt();
        seq_ = st["seq"].asInt();
        if (d.is("updates.differenceSlice")) getDifference();
    });
}

bool Client::checkPts(int32_t pts, int32_t count)
{
    if (syncing_) return true;
    if (pts_ == 0) { pts_ = pts; return true; }
    if (pts_ + count == pts) { pts_ = pts; return true; }
    if (pts_ + count > pts) return false;   // already applied
    getDifference();
    return false;
}

void Client::onSessionReset() { getDifference(); }

void Client::onAuthLost()
{
    selfId_ = 0;
    pts_ = qts_ = date_ = seq_ = 0;
    chats_.clear();
    peers_.clear();
    media_.clear();
    setAuth(WaitPhone);
}

void Client::applyMessage(const TlValue& m, bool isNew)
{
    if (m.is("messageEmpty")) return;
    MsgItem mi = convertMessage(m);
    if (isNew) {
        updateChatFromMessage(mi, true);
        if (listener_) listener_->onNewMessage(mi);
    } else {
        ChatItem* c = chatMut(mi.chatKey);
        if (c && c->topId == mi.id) { c->lastText = previewText(mi); if (listener_) listener_->onChatsChanged(); }
        if (listener_) listener_->onMessageEdited(mi);
    }
}

void Client::applyUpdate(const TlValue& u)
{
    std::string n = u.name();
    if (n == "updateNewMessage") {
        if (checkPts(u["pts"].asInt(), u["pts_count"].asInt())) applyMessage(u["message"], true);
    } else if (n == "updateEditMessage") {
        if (checkPts(u["pts"].asInt(), u["pts_count"].asInt())) applyMessage(u["message"], false);
    } else if (n == "updateDeleteMessages") {
        if (checkPts(u["pts"].asInt(), u["pts_count"].asInt()) && listener_) {
            std::vector<int32_t> ids;
            for (std::size_t i = 0; i < u["messages"].size(); ++i) ids.push_back(u["messages"].at(i).asInt());
            listener_->onMessagesDeleted("", ids);
        }
    } else if (n == "updateReadHistoryInbox") {
        if (checkPts(u["pts"].asInt(), u["pts_count"].asInt())) {
            ChatItem* c = chatMut(peerKey(u["peer"]));
            if (c) { c->unread = u["still_unread_count"].asInt(); c->readInboxMax = u["max_id"].asInt(); }
            if (listener_) listener_->onChatsChanged();
        }
    } else if (n == "updateReadHistoryOutbox") {
        if (checkPts(u["pts"].asInt(), u["pts_count"].asInt())) {
            std::string key = peerKey(u["peer"]);
            ChatItem* c = chatMut(key);
            if (c) c->readOutboxMax = u["max_id"].asInt();
            if (listener_) listener_->onReadOutbox(key, u["max_id"].asInt());
        }
    } else if (n == "updateMessageReactions") {
        std::vector<Reaction> rs;
        parseReactions(u["reactions"], rs);
        if (listener_) listener_->onMessageReactions(peerKey(u["peer"]), u["msg_id"].asInt(), rs);
    } else if (n == "updateNewChannelMessage") {
        applyMessage(u["message"], true);
    } else if (n == "updateEditChannelMessage") {
        applyMessage(u["message"], false);
    } else if (n == "updateDeleteChannelMessages") {
        std::vector<int32_t> ids;
        for (std::size_t i = 0; i < u["messages"].size(); ++i) ids.push_back(u["messages"].at(i).asInt());
        if (listener_) listener_->onMessagesDeleted("h" + num(u["channel_id"].asLong()), ids);
    } else if (n == "updateReadChannelInbox") {
        ChatItem* c = chatMut("h" + num(u["channel_id"].asLong()));
        if (c) { c->unread = u["still_unread_count"].asInt(); c->readInboxMax = u["max_id"].asInt(); }
        if (listener_) listener_->onChatsChanged();
    } else if (n == "updateReadChannelOutbox") {
        std::string key = "h" + num(u["channel_id"].asLong());
        ChatItem* c = chatMut(key);
        if (c) c->readOutboxMax = u["max_id"].asInt();
        if (listener_) listener_->onReadOutbox(key, u["max_id"].asInt());
    } else if (n == "updateNotifySettings") {
        const TlValue& np = u["peer"];
        if (np.is("notifyPeer")) {
            ChatItem* c = chatMut(peerKey(np["peer"]));
            if (c) {
                c->muted = u["notify_settings"]["mute_until"].asInt() > static_cast<int32_t>(std::time(0));
                if (listener_) listener_->onChatsChanged();
            }
        }
    } else if (n == "updateMessageID") {
        sentIds_[u["id"].asInt()] = u["random_id"].asLong();
    } else if (n == "updatePinnedDialogs" || n == "updateDialogPinned" || n == "updateFolderPeers") {
        loadChats(std::function<void(const RpcErr&)>());
    } else if (n == "updateChannelTooLong") {
        // missed messages in a channel: the chat list will catch up on the next loadChats
    }
}

void Client::onUpdates(const TlValue& u)
{
    if (auth_ != Ready) return;
    std::string n = u.name();
    if (n == "updatesTooLong") {
        getDifference();
    } else if (n == "updateShortMessage" || n == "updateShortChatMessage") {
        // build a full message so the normal path handles it; unknown senders need a difference
        std::string from = n == "updateShortMessage" ? "u" + num(u["user_id"].asLong()) : "u" + num(u["from_id"].asLong());
        if (!peers_.count(from)) { getDifference(); return; }
        if (!checkPts(u["pts"].asInt(), u["pts_count"].asInt())) return;
        TlValue m = tlObj("message");
        m.set("out", u["out"]).set("id", u["id"]).set("date", u["date"]).set("message", u["message"])
         .set("reply_to", u["reply_to"]).set("fwd_from", u["fwd_from"]);
        if (n == "updateShortMessage") {
            m.set("peer_id", tlObj("peerUser").set("user_id", u["user_id"]));
            if (!u["out"].asBool()) m.set("from_id", tlObj("peerUser").set("user_id", u["user_id"]));
        } else {
            m.set("peer_id", tlObj("peerChat").set("chat_id", u["chat_id"]));
            m.set("from_id", tlObj("peerUser").set("user_id", u["from_id"]));
        }
        applyMessage(m, true);
    } else if (n == "updateShort") {
        applyUpdate(u["update"]);
    } else if (n == "updates" || n == "updatesCombined") {
        absorbUsersChats(u);
        const TlValue& ups = u["updates"];
        for (std::size_t i = 0; i < ups.size(); ++i) applyUpdate(ups.at(i));
    }
}

// ---------------------------------------------------------------- login

void Client::sendCode(const std::string& phone, const std::function<void(const RpcErr&)>& done)
{
    phone_.clear();
    for (std::size_t i = 0; i < phone.size(); ++i)
        if (phone[i] >= '0' && phone[i] <= '9') phone_ += phone[i];
    TlValue q = tlObj("auth.sendCode");
    q.set("phone_number", tlStr(phone_)).set("api_id", tlInt(apiId_)).set("api_hash", tlStr(apiHash_))
     .set("settings", tlObj("codeSettings"));
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        if (!e.ok()) { done(e); return; }
        if (r.is("auth.sentCodeSuccess")) {
            const TlValue& a = r["authorization"];
            absorbUser(a["user"]);
            selfId_ = a["user"]["id"].asLong();
            engine_->setAuthorized(true);
            start();
            done(e);
            return;
        }
        phoneCodeHash_ = r["phone_code_hash"].str();
        std::string t = r["type"].name();
        codeInfo_ = t == "auth.sentCodeTypeApp" ? "app" : t == "auth.sentCodeTypeSms" ? "sms" :
                    t == "auth.sentCodeTypeCall" ? "call" : t == "auth.sentCodeTypeEmailCode" ? "email" :
                    t == "auth.sentCodeTypeFragmentSms" ? "fragment" : t;
        setAuth(WaitCode);
        done(e);
    });
}

void Client::signIn(const std::string& code, const std::function<void(const RpcErr&)>& done)
{
    TlValue q = tlObj("auth.signIn");
    q.set("phone_number", tlStr(phone_)).set("phone_code_hash", tlStr(phoneCodeHash_)).set("phone_code", tlStr(code));
    engine_->invoke(q, [this, done](const TlValue& r, const RpcErr& e) {
        if (e.message == "SESSION_PASSWORD_NEEDED") {
            engine_->invoke(tlObj("account.getPassword"), [this, done](const TlValue& p, const RpcErr& e2) {
                hint_ = p["hint"].str();
                setAuth(WaitPassword);
                done(e2);
            });
            return;
        }
        if (!e.ok()) { done(e); return; }
        if (r.is("auth.authorizationSignUpRequired")) { done(RpcErr(400, "SIGN_UP_REQUIRED")); return; }
        absorbUser(r["user"]);
        selfId_ = r["user"]["id"].asLong();
        engine_->setAuthorized(true);
        start();
        done(e);
    });
}

void Client::checkPassword(const std::string& password, const std::function<void(const RpcErr&)>& done)
{
    engine_->invoke(tlObj("account.getPassword"), [this, password, done](const TlValue& p, const RpcErr& e) {
        if (!e.ok()) { done(e); return; }
        const TlValue& algo = p["current_algo"];
        if (!algo.is("passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow")) {
            done(RpcErr(-1, "PASSWORD_ALGO_UNSUPPORTED"));
            return;
        }
        uint8_t a[256];
        for (int i = 0; i < 256; i += 8) {
            int64_t r = randomId() ^ static_cast<int64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            for (int j = 0; j < 8; ++j) a[i + j] = static_cast<uint8_t>(r >> (8 * j));
        }
        std::string A, M1;
        if (!computeSrp(password, algo["salt1"].str(), algo["salt2"].str(), algo["g"].asInt(), algo["p"].str(),
                        p["srp_B"].str(), a, A, M1)) {
            done(RpcErr(-1, "SRP_FAILED"));
            return;
        }
        TlValue srp = tlObj("inputCheckPasswordSRP").set("srp_id", p["srp_id"]).set("A", tlStr(A)).set("M1", tlStr(M1));
        engine_->invoke(tlObj("auth.checkPassword").set("password", srp), [this, done](const TlValue& r, const RpcErr& e2) {
            if (!e2.ok()) { done(e2); return; }
            absorbUser(r["user"]);
            selfId_ = r["user"]["id"].asLong();
            engine_->setAuthorized(true);
            start();
            done(e2);
        });
    });
}

void Client::logOut(const std::function<void()>& done)
{
    engine_->invoke(tlObj("auth.logOut"), [this, done](const TlValue&, const RpcErr&) {
        engine_->logOutLocal();
        onAuthLost();
        if (done) done();
    });
}

// ---------------------------------------------------------------- sending

void Client::handleSentUpdates(const TlValue& u, const std::string& chatKey, int64_t rid, const SentCallback& done)
{
    if (u.is("updateShortSentMessage")) {
        checkPts(u["pts"].asInt(), u["pts_count"].asInt());
        std::map<int64_t, std::pair<std::string, SentCallback> >::iterator it = pendingSends_.find(rid);
        MsgItem m;
        m.id = u["id"].asInt();
        m.chatKey = chatKey;
        m.out = true;
        m.fromKey = selfKey();
        m.fromName = "Tu";
        m.date = u["date"].asInt();
        if (it != pendingSends_.end()) { m.text = it->second.first; pendingSends_.erase(it); }
        updateChatFromMessage(m, false);
        done(m, RpcErr());
        return;
    }
    // full updates: find our message by random_id, then let the normal path apply everything
    int32_t id = 0;
    const TlValue& ups = u["updates"];
    for (std::size_t i = 0; i < ups.size(); ++i)
        if (ups.at(i).is("updateMessageID") && ups.at(i)["random_id"].asLong() == rid) id = ups.at(i)["id"].asInt();
    absorbUsersChats(u);
    MsgItem found;
    for (std::size_t i = 0; i < ups.size(); ++i) {
        const TlValue& x = ups.at(i);
        if ((x.is("updateNewMessage") || x.is("updateNewChannelMessage")) && x["message"]["id"].asInt() == id)
            found = convertMessage(x["message"]);
    }
    pendingSends_.erase(rid);
    onUpdates(u);
    if (found.id == 0) { found.id = id; found.chatKey = chatKey; found.out = true; found.date = static_cast<int32_t>(std::time(0)); }
    done(found, RpcErr());
}

void Client::sendText(const std::string& chatKey, const std::string& text, int32_t replyTo, const SentCallback& done)
{
    int64_t rid = randomId();
    TlValue q = tlObj("messages.sendMessage");
    q.set("peer", inputPeer(chatKey)).set("message", tlStr(text)).set("random_id", tlLong(rid));
    TlValue rt = replyHeader(replyTo);
    if (!rt.isNull()) q.set("reply_to", rt);
    pendingSends_[rid] = std::make_pair(text, done);
    engine_->invoke(q, [this, chatKey, rid, done](const TlValue& u, const RpcErr& e) {
        if (!e.ok()) { pendingSends_.erase(rid); done(MsgItem(), e); return; }
        handleSentUpdates(u, chatKey, rid, done);
    });
}

void Client::sendMediaCommon(const std::string& chatKey, const TlValue& media, const std::string& caption,
                             const SentCallback& done)
{
    int64_t rid = randomId();
    TlValue q = tlObj("messages.sendMedia");
    q.set("peer", inputPeer(chatKey)).set("media", media).set("message", tlStr(caption)).set("random_id", tlLong(rid));
    TlValue rt = replyHeader(0);
    if (!rt.isNull()) q.set("reply_to", rt);
    pendingSends_[rid] = std::make_pair(caption, done);
    engine_->invoke(q, [this, chatKey, rid, done](const TlValue& u, const RpcErr& e) {
        if (!e.ok()) { pendingSends_.erase(rid); done(MsgItem(), e); return; }
        handleSentUpdates(u, chatKey, rid, done);
    });
}

namespace {
struct UploadState {
    std::string data, name;
    int64_t fileId;
    int parts, next, done;
    bool big, failed;
    std::function<void(const TlValue&, const RpcErr&)> cb;
};
}

void Client::upload(const std::string& data, const std::string& name,
                    const std::function<void(const TlValue& inputFile, const RpcErr&)>& done)
{
    std::shared_ptr<UploadState> st = std::make_shared<UploadState>();
    st->data = data;
    st->name = name;
    st->fileId = randomId();
    st->parts = static_cast<int>((data.size() + kPartSize - 1) / kPartSize);
    if (st->parts == 0) st->parts = 1;
    st->next = 0;
    st->done = 0;
    st->big = data.size() > 10 * 1024 * 1024;
    st->failed = false;
    st->cb = done;
    std::shared_ptr<std::function<void()> > sendNext = std::make_shared<std::function<void()> >();
    Engine* eng = engine_;
    std::weak_ptr<std::function<void()> > weak = sendNext;   // in-flight callbacks keep it alive, no cycle
    *sendNext = [st, weak, eng]() {
        std::shared_ptr<std::function<void()> > self = weak.lock();
        if (!self || st->failed || st->next >= st->parts) return;
        int part = st->next++;
        std::string bytes = st->data.substr(static_cast<std::size_t>(part) * kPartSize, kPartSize);
        TlValue q = st->big ? tlObj("upload.saveBigFilePart").set("file_total_parts", tlInt(st->parts))
                            : tlObj("upload.saveFilePart");
        q.set("file_id", tlLong(st->fileId)).set("file_part", tlInt(part)).set("bytes", tlStr(bytes));
        eng->invoke(q, [st, self](const TlValue&, const RpcErr& e) {
            if (st->failed) return;
            if (!e.ok()) { st->failed = true; st->cb(TlValue(), e); return; }
            if (++st->done == st->parts) {
                TlValue f = st->big ? tlObj("inputFileBig") : tlObj("inputFile");
                f.set("id", tlLong(st->fileId)).set("parts", tlInt(st->parts)).set("name", tlStr(st->name));
                if (!st->big) f.set("md5_checksum", tlStr(""));
                st->cb(f, RpcErr());
                return;
            }
            (*self)();
        });
    };
    for (int i = 0; i < 4; ++i) (*sendNext)();
}

void Client::sendPhoto(const std::string& chatKey, const std::string& jpeg, const std::string& caption, const SentCallback& done)
{
    upload(jpeg, "photo.jpg", [this, chatKey, caption, done](const TlValue& f, const RpcErr& e) {
        if (!e.ok()) { done(MsgItem(), e); return; }
        sendMediaCommon(chatKey, tlObj("inputMediaUploadedPhoto").set("file", f), caption, done);
    });
}

void Client::sendVoice(const std::string& chatKey, const std::string& ogg, int32_t seconds, const std::string& waveform,
                       const SentCallback& done)
{
    upload(ogg, "voice.ogg", [this, chatKey, seconds, waveform, done](const TlValue& f, const RpcErr& e) {
        if (!e.ok()) { done(MsgItem(), e); return; }
        TlValue audio = tlObj("documentAttributeAudio").set("voice", tlBool(true)).set("duration", tlInt(seconds));
        if (!waveform.empty()) audio.set("waveform", tlStr(waveform));
        TlValue attrs = TlValue::vec();
        attrs.push(audio);
        TlValue media = tlObj("inputMediaUploadedDocument");
        media.set("file", f).set("mime_type", tlStr("audio/ogg")).set("attributes", attrs);
        sendMediaCommon(chatKey, media, "", done);
    });
}

void Client::sendFile(const std::string& chatKey, const std::string& data, const std::string& fileName,
                      const std::string& mime, const std::string& caption, const SentCallback& done)
{
    upload(data, fileName, [this, chatKey, fileName, mime, caption, done](const TlValue& f, const RpcErr& e) {
        if (!e.ok()) { done(MsgItem(), e); return; }
        TlValue attrs = TlValue::vec();
        attrs.push(tlObj("documentAttributeFilename").set("file_name", tlStr(fileName)));
        TlValue media = tlObj("inputMediaUploadedDocument");
        media.set("file", f).set("mime_type", tlStr(mime.empty() ? "application/octet-stream" : mime)).set("attributes", attrs)
             .set("force_file", tlBool(true));
        sendMediaCommon(chatKey, media, caption, done);
    });
}

void Client::deleteMessages(const std::string& chatKey, const std::vector<int32_t>& ids, bool forAll,
                            const std::function<void(const RpcErr&)>& done)
{
    TlValue v = TlValue::vec();
    for (std::size_t i = 0; i < ids.size(); ++i) v.push(tlInt(ids[i]));
    TlValue q;
    if (!chatKey.empty() && chatKey[0] == 'h') {
        TlValue p = inputPeer(chatKey);
        q = tlObj("channels.deleteMessages")
                .set("channel", tlObj("inputChannel").set("channel_id", p["channel_id"]).set("access_hash", p["access_hash"]))
                .set("id", v);
    } else {
        q = tlObj("messages.deleteMessages").set("revoke", tlBool(forAll)).set("id", v);
    }
    engine_->invoke(q, [this, chatKey, ids, done](const TlValue& r, const RpcErr& e) {
        if (e.ok()) {
            if (chatKey[0] != 'h') checkPts(r["pts"].asInt(), r["pts_count"].asInt());
            if (listener_) listener_->onMessagesDeleted(chatKey, ids);
        }
        if (done) done(e);
    });
}

void Client::forwardMessages(const std::string& fromKey, const std::vector<int32_t>& ids, const std::string& toKey,
                             const SentCallback& done)
{
    if (ids.empty()) return;
    TlValue idv = TlValue::vec(), rids = TlValue::vec();
    int64_t first = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        idv.push(tlInt(ids[i]));
        int64_t rid = randomId();
        if (i == 0) first = rid;
        rids.push(tlLong(rid));
    }
    TlValue q = tlObj("messages.forwardMessages");
    q.set("from_peer", inputPeer(fromKey)).set("id", idv).set("random_id", rids).set("to_peer", inputPeer(toKey));
    engine_->invoke(q, [this, toKey, first, done](const TlValue& u, const RpcErr& e) {
        if (!e.ok()) { done(MsgItem(), e); return; }
        handleSentUpdates(u, toKey, first, done);
    });
}

// ---------------------------------------------------------------- download

int64_t Client::mediaSize(const std::string& key) const
{
    std::map<std::string, Media>::const_iterator it = media_.find(key);
    return it == media_.end() ? 0 : it->second.size;
}

void Client::downloadMedia(const std::string& key, const std::function<void(int64_t, int64_t)>& progress,
                           const std::function<void(const std::string&, const RpcErr&)>& done)
{
    if (!media_.count(key)) { done(std::string(), RpcErr(-1, "MEDIA_UNKNOWN")); return; }
    std::string* buf = new std::string();
    downloadPart(key, buf, 0, progress, done, 0);
}

void Client::downloadPart(const std::string& key, std::string* buf, int64_t offset,
                          const std::function<void(int64_t, int64_t)>& progress,
                          const std::function<void(const std::string&, const RpcErr&)>& done, int refreshes)
{
    Media& m = media_[key];
    TlValue q = tlObj("upload.getFile");
    q.set("location", m.location).set("offset", tlLong(offset)).set("limit", tlInt(kChunk));
    int64_t total = m.size;
    engine_->invoke(q, [this, key, buf, offset, total, progress, done, refreshes](const TlValue& r, const RpcErr& e) {
        if (!e.ok()) {
            if (e.message.compare(0, 14, "FILE_REFERENCE") == 0 && refreshes < 1) {
                // refetch the message to get a fresh file_reference, then retry
                Media& m = media_[key];
                TlValue ids = TlValue::vec();
                ids.push(tlObj("inputMessageID").set("id", tlInt(m.msgId)));
                TlValue q2;
                if (!m.chatKey.empty() && m.chatKey[0] == 'h') {
                    TlValue p = inputPeer(m.chatKey);
                    q2 = tlObj("channels.getMessages")
                             .set("channel", tlObj("inputChannel").set("channel_id", p["channel_id"]).set("access_hash", p["access_hash"]))
                             .set("id", ids);
                } else {
                    q2 = tlObj("messages.getMessages").set("id", ids);
                }
                engine_->invoke(q2, [this, key, buf, offset, progress, done, refreshes](const TlValue& r2, const RpcErr& e2) {
                    if (!e2.ok()) { delete buf; done(std::string(), e2); return; }
                    absorbUsersChats(r2);
                    for (std::size_t i = 0; i < r2["messages"].size(); ++i) convertMessage(r2["messages"].at(i));
                    downloadPart(key, buf, offset, progress, done, refreshes + 1);
                });
                return;
            }
            delete buf;
            done(std::string(), e);
            return;
        }
        const std::string& b = r["bytes"].str();
        buf->append(b);
        if (progress) progress(static_cast<int64_t>(buf->size()), total);
        if (static_cast<int>(b.size()) < kChunk) {
            std::string out;
            out.swap(*buf);
            delete buf;
            done(out, RpcErr());
            return;
        }
        downloadPart(key, buf, offset + kChunk, progress, done, refreshes);
    }, m.dc);
}

} // namespace tg
