// convertMessage on messages shaped like the server's: quotes, forum topics, comment threads, reactions.
#include "../core/client.h"

#include <iostream>

using namespace tg;

namespace {
int fails = 0;
void check(bool ok, const char* what)
{
    std::cout << (ok ? "  ok " : "  FAIL ") << what << "\n";
    if (!ok) ++fails;
}

TlValue base(int id)
{
    return tlObj("message").set("id", tlInt(id)).set("peer_id", tlObj("peerChannel").set("channel_id", tlLong(77)))
        .set("from_id", tlObj("peerUser").set("user_id", tlLong(5))).set("date", tlInt(1000)).set("message", tlStr("hi"));
}
}

int runClientTests()
{
    Client c(0, 0, "");

    TlValue m = base(10);
    m.set("reply_to", tlObj("messageReplyHeader").set("reply_to_msg_id", tlInt(7)).set("quote_text", tlStr("parte")));
    MsgItem a = c.convertMessage(m);
    check(a.replyTo == 7 && a.threadId == 0 && a.replyText == "parte", "plain reply with quote text");

    m = base(11);
    m.set("reply_to", tlObj("messageReplyHeader").set("forum_topic", tlBool(true)).set("reply_to_msg_id", tlInt(3)));
    a = c.convertMessage(m);
    check(a.replyTo == 0 && a.threadId == 3, "message in a topic is not a quote of the topic");

    m = base(12);
    m.set("reply_to", tlObj("messageReplyHeader").set("forum_topic", tlBool(true)).set("reply_to_msg_id", tlInt(9))
        .set("reply_to_top_id", tlInt(3)));
    a = c.convertMessage(m);
    check(a.replyTo == 9 && a.threadId == 3, "reply inside a topic");

    m = base(13);
    TlValue results = TlValue::vec();
    results.push(tlObj("reactionCount").set("reaction", tlObj("reactionEmoji").set("emoticon", tlStr("\xF0\x9F\x91\x8D")))
        .set("count", tlInt(4)).set("chosen_order", tlInt(0)));
    results.push(tlObj("reactionCount").set("reaction", tlObj("reactionCustomEmoji").set("document_id", tlLong(1)))
        .set("count", tlInt(2)));
    m.set("reactions", tlObj("messageReactions").set("results", results).set("recent_reactions", TlValue::vec()));
    m.set("replies", tlObj("messageReplies").set("comments", tlBool(true)).set("replies", tlInt(12)).set("replies_pts", tlInt(1))
        .set("channel_id", tlLong(88)));
    a = c.convertMessage(m);
    check(a.reactions.size() == 2 && a.reactions[0].count == 4 && a.reactions[0].mine && !a.reactions[1].mine
          && a.reactions[1].emoji == "\xE2\x98\x85", "reactions with counts and mine");
    check(a.replies == 12 && a.hasComments, "comment count of a channel post");

    // the same objects survive a serialize/parse round trip (flags computed from the fields set)
    TlWriter w;
    tlWrite(w, m, "Message");
    TlReader r(w.data().data(), w.data().size());
    TlValue back = tlRead(r, "Message");
    a = c.convertMessage(back);
    check(a.reactions.size() == 2 && a.replies == 12 && a.hasComments, "round trip through the wire format");
    return fails;
}
