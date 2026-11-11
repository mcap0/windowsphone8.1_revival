#ifndef TG_CORE_CLIENT_H
#define TG_CORE_CLIENT_H

// Telegram client logic on top of Engine, portable C++11, UTF-8 strings. Everything runs on the engine's
// thread. The UI gets plain structs (ChatItem, MsgItem) and listener calls.

#include "engine.h"

#include <functional>
#include <map>
#include <set>
#include <stdint.h>
#include <string>
#include <vector>

namespace tg {

struct ChatItem {
    std::string key;        // "u<id>", "c<id>" (basic group), "h<id>" (channel/supergroup)
    std::string title;
    std::string lastText;   // one line preview, media as "[Foto]" etc.
    int32_t lastDate;
    int32_t topId;
    int32_t readInboxMax;
    int32_t readOutboxMax;
    int unread;
    bool muted, pinned, isGroup, isChannel, lastOut;
    bool isForum;           // supergroup with topics: opens on the topic list
    int64_t photoId;        // small chat photo, 0 if none (download with photoKey)
    std::string photoKey;
    ChatItem() : lastDate(0), topId(0), readInboxMax(0), readOutboxMax(0), unread(0), muted(false), pinned(false),
                 isGroup(false), isChannel(false), lastOut(false), isForum(false), photoId(0) {}
};

struct Reaction {
    std::string emoji;      // "★" for custom emoji, "⭐" for paid reactions
    int32_t count;
    bool mine;
    Reaction() : count(0), mine(false) {}
};

// A forum topic (channels.getForumTopics). id 1 is "General".
struct TopicItem {
    int32_t id;
    std::string title;
    std::string lastText;
    int32_t lastDate;
    int unread;
    bool pinned, closed;
    TopicItem() : id(0), lastDate(0), unread(0), pinned(false), closed(false) {}
};

struct MsgItem {
    enum Kind { Text, Photo, Voice, Audio, Video, VideoNote, Document, Sticker, Gif, Location, Contact, Poll, Service, Other };
    int32_t id;
    std::string chatKey;
    std::string fromKey;
    std::string fromName;
    bool out;
    int32_t date;
    int32_t editDate;
    std::string text;       // message text / caption / service text
    Kind kind;
    std::string mediaKey;   // for downloadMedia; "" if no downloadable media
    std::string thumbKey;   // small preview (photo "m" size / document thumb)
    std::string strippedJpeg; // tiny inline preview (already a full JPEG), may be empty
    int32_t duration;
    int32_t width, height;
    int64_t size;
    std::string fileName, mime, emoji;
    std::string waveform;   // 5-bit packed waveform of voice notes
    int32_t replyTo;        // quoted message (0: none; a message that only starts a topic/thread is not a quote)
    int32_t threadId;       // forum topic or comment thread the message belongs to (0: none or General)
    std::string replyFrom;  // quoted message from another chat: its author; else filled by the page
    std::string replyText;  // the quoted part when the sender picked one (quote_text)
    std::vector<Reaction> reactions;
    int32_t replies;        // replies / comments count (messageReplies)
    bool hasComments;       // channel post with a discussion group: "N commenti" opens it
    std::string forwardFrom;
    MsgItem() : id(0), out(false), date(0), editDate(0), kind(Text), duration(0), width(0), height(0), size(0), replyTo(0),
                threadId(0), replies(0), hasComments(false) {}
};

struct ContactItem {
    std::string key;        // chat key, same as ChatItem::key
    std::string name;
    std::string username;   // without @, may be empty
    std::string phone;      // without +, may be empty
};

class IClientListener {
public:
    virtual ~IClientListener() {}
    virtual void onAuthState(int state) {}                       // Client::AuthState
    virtual void onChatsChanged() {}
    virtual void onNewMessage(const MsgItem& m) {}
    virtual void onMessageEdited(const MsgItem& m) {}
    virtual void onMessagesDeleted(const std::string& chatKey, const std::vector<int32_t>& ids) {}  // chatKey "" = any private/basic
    virtual void onReadOutbox(const std::string& chatKey, int32_t maxId) {}
    virtual void onMessageReactions(const std::string& chatKey, int32_t id, const std::vector<Reaction>& reactions) {}
    virtual void onOnline(bool online) {}
    virtual void onError(const std::string& what) {}
};

class Client {
public:
    enum AuthState { AuthUnknown, WaitPhone, WaitCode, WaitPassword, Ready, LoggingOut };

    Client(Engine* engine, int apiId, const std::string& apiHash);
    void setListener(IClientListener* l) { listener_ = l; }

    std::string save() const;
    void load(const std::string& blob);

    // Call after Engine::start(): checks the session and syncs updates.
    void start();
    void onUpdates(const TlValue& updates);     // from IEngineHost::onUpdates
    void onSessionReset();                      // from IEngineHost::onSessionReset
    void onAuthLost();

    // ---- login
    AuthState authState() const { return auth_; }
    std::string codeInfo() const { return codeInfo_; }    // "app" / "sms" / ... where the code was sent
    std::string passwordHint() const { return hint_; }
    void sendCode(const std::string& phone, const std::function<void(const RpcErr&)>& done);
    void signIn(const std::string& code, const std::function<void(const RpcErr&)>& done);
    // 2FA. The SRP math (PBKDF2 100000 rounds) is slow: it runs inside this call.
    void checkPassword(const std::string& password, const std::function<void(const RpcErr&)>& done);
    void logOut(const std::function<void()>& done);

    // ---- chats
    const std::vector<ChatItem>& chats() const { return chats_; }
    const ChatItem* chat(const std::string& key) const;
    std::string selfKey() const;
    // limit: how many dialogs (the background poll of Avvisi asks for few)
    void loadChats(const std::function<void(const RpcErr&)>& done, int limit = 100);
    // Unread count, read marks and top message of some chats (messages.getPeerDialogs): supergroups and channels,
    // whose messages never come in updates.getDifference. A newer top message goes to the listener (onNewMessage).
    void refreshChats(const std::vector<std::string>& keys, const std::function<void(const RpcErr&)>& done);
    // History, newest first in the server answer; returned oldest first. beforeId 0 = latest.
    void loadHistory(const std::string& chatKey, int32_t beforeId, int limit,
                     const std::function<void(const std::vector<MsgItem>&, const RpcErr&)>& done);
    void markRead(const std::string& chatKey, int32_t maxId);
    typedef std::function<void(const std::vector<MsgItem>&, const RpcErr&)> MessagesCallback;
    // Some messages by id (the quoted ones not loaded yet); oldest first, missing ones left out.
    void getMessages(const std::string& chatKey, const std::vector<int32_t>& ids, const MessagesCallback& done);
    // Threads: the comments of a channel post (in its discussion group) and the topics of a forum share the
    // same calls. loadThread is loadHistory for one thread (topic 1 = General: the messages outside topics).
    void loadThread(const std::string& chatKey, int32_t threadId, int32_t beforeId, int limit, const MessagesCallback& done);
    // The discussion of a channel post: the group's chat key and the id of the thread's first message there.
    void openComments(const std::string& chatKey, int32_t msgId,
                      const std::function<void(const std::string&, int32_t, const RpcErr&)>& done);
    void loadTopics(const std::string& chatKey, const std::function<void(const std::vector<TopicItem>&, const RpcErr&)>& done);
    // Reaction of the user on a message: emoji "" removes it. The new counts come back as onMessageReactions.
    void sendReaction(const std::string& chatKey, int32_t msgId, const std::string& emoji,
                      const std::function<void(const RpcErr&)>& done);
    // Messages sent from now on go to this thread (topic or comments), 0 = the chat itself. Set by the chat page.
    void setSendThread(int32_t threadId) { sendThread_ = threadId; }
    // Read mark inside one topic / comment thread (General: the chat's own read mark).
    void markThreadRead(const std::string& chatKey, int32_t threadId, int32_t maxId);
    // ---- new chat: address book (sorted by name) and global search by name / @username
    typedef std::function<void(const std::vector<ContactItem>&, const RpcErr&)> ContactsCallback;
    void loadContacts(const ContactsCallback& done);
    void searchPeople(const std::string& query, const ContactsCallback& done);
    std::string titleOf(const std::string& key) const { return nameOf(key); }
    void setMuted(const std::string& chatKey, bool muted, const std::function<void(const RpcErr&)>& done);

    // ---- sending (done gets the sent message as the server stored it)
    typedef std::function<void(const MsgItem&, const RpcErr&)> SentCallback;
    void sendText(const std::string& chatKey, const std::string& text, int32_t replyTo, const SentCallback& done);
    void sendPhoto(const std::string& chatKey, const std::string& jpeg, const std::string& caption, const SentCallback& done);
    // ogg: OGG/Opus voice note; waveform: 5-bit packed (may be empty)
    void sendVoice(const std::string& chatKey, const std::string& ogg, int32_t seconds, const std::string& waveform,
                   const SentCallback& done);
    void sendFile(const std::string& chatKey, const std::string& data, const std::string& fileName,
                  const std::string& mime, const std::string& caption, const SentCallback& done);
    void deleteMessages(const std::string& chatKey, const std::vector<int32_t>& ids, bool forAll,
                        const std::function<void(const RpcErr&)>& done);
    // messages.forwardMessages fromKey -> toKey; done gets the first forwarded message as stored in toKey
    void forwardMessages(const std::string& fromKey, const std::vector<int32_t>& ids, const std::string& toKey,
                         const SentCallback& done);

    // ---- media. progress(received, total) may be called several times; done gets the whole file.
    void downloadMedia(const std::string& mediaKey,
                       const std::function<void(int64_t, int64_t)>& progress,
                       const std::function<void(const std::string&, const RpcErr&)>& done);
    int64_t mediaSize(const std::string& mediaKey) const;

    // Text of a message for previews / notifications
    static std::string previewText(const MsgItem& m);

    // Exposed for tests and the PC tool
    MsgItem convertMessage(const TlValue& m);
    void absorbUsersChats(const TlValue& container);

private:
    struct Peer {
        int type;            // 1 user, 2 basic group, 3 channel
        int64_t id;
        int64_t accessHash;
        std::string name;
        bool isBroadcast, isForum;
        Peer() : type(0), id(0), accessHash(0), isBroadcast(false), isForum(false) {}
    };
    struct Media {
        TlValue location;    // InputFileLocation
        int dc;
        int64_t size;
        std::string chatKey; // for refreshing the file reference
        int32_t msgId;
        Media() : dc(0), size(0), msgId(0) {}
    };

    void setAuth(AuthState s);
    std::string peerKey(const TlValue& peer) const;
    TlValue inputPeer(const std::string& key) const;
    TlValue inputUser(const std::string& key) const;
    std::string nameOf(const std::string& key) const;
    void absorbUser(const TlValue& u);
    void absorbChat(const TlValue& c);
    void applyMessage(const TlValue& m, bool isNew);
    void applyUpdate(const TlValue& u);
    bool checkPts(int32_t pts, int32_t ptsCount);
    void getDifference();
    void getState();
    void updateChatFromMessage(const MsgItem& m, bool isNew);
    void sortChats();
    ChatItem* chatMut(const std::string& key);
    void sendMediaCommon(const std::string& chatKey, const TlValue& media, const std::string& caption, const SentCallback& done);
    void upload(const std::string& data, const std::string& name,
                const std::function<void(const TlValue& inputFile, const RpcErr&)>& done);
    void handleSentUpdates(const TlValue& updates, const std::string& chatKey, int64_t randomId, const SentCallback& done);
    std::string registerPhoto(const TlValue& photo, const std::string& want, const std::string& chatKey, int32_t msgId,
                              int64_t* sizeOut);
    std::string registerDocument(const TlValue& doc, const std::string& chatKey, int32_t msgId);
    std::string registerDocumentThumb(const TlValue& doc, const std::string& chatKey, int32_t msgId);
    void downloadPart(const std::string& key, std::string* buf, int64_t offset,
                      const std::function<void(int64_t, int64_t)>& progress,
                      const std::function<void(const std::string&, const RpcErr&)>& done, int refreshes);
    int64_t randomId();
    void error(const std::string& what);

    Engine* engine_;
    IClientListener* listener_;
    int apiId_;
    std::string apiHash_;
    AuthState auth_;
    std::string phone_, phoneCodeHash_, codeInfo_, hint_;
    int64_t selfId_;
    int32_t pts_, qts_, date_, seq_;
    bool syncing_;
    bool chatsLoaded_;
    std::map<std::string, Peer> peers_;
    std::vector<ChatItem> chats_;
    std::map<std::string, Media> media_;
    std::map<int64_t, std::pair<std::string, SentCallback> > pendingSends_;   // random_id -> chat, callback
    std::map<int32_t, int64_t> sentIds_;   // message id -> random_id (updateMessageID)
    uint64_t rnd_;
    int32_t sendThread_;
    TlValue replyHeader(int32_t replyTo) const;
};

// SRP for auth.checkPassword (core/srp.cpp). Returns false on bad parameters.
bool computeSrp(const std::string& password, const std::string& salt1, const std::string& salt2, int g,
                const std::string& p, const std::string& srpB, const uint8_t random256[256],
                std::string& A, std::string& M1);

// Expands a photoStrippedSize payload into a displayable JPEG.
std::string strippedToJpeg(const std::string& stripped);

} // namespace tg

#endif
