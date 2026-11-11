// WebSockets (plan §55, 4b): the byte stream under WebKitLegacy's WebSocketChannel, which does the protocol itself
// (handshake, frames, permessage-deflate, closing handshake, the inspector). The stream is WebCore's CurlStream: curl
// connects (TLS with our CA bundle), and its scheduler thread moves the bytes; every callback comes on the Web thread.
// Included by WebKitLegacy/WebCoreSupport/SocketStreamHandleImpl.h, whose CFNetwork class this replaces.
#pragma once

#include "SocketStreamHandle.h"
#include <WebCore/CurlStream.h>
#include <WebCore/StorageSessionProvider.h>
#include <pal/SessionID.h>

namespace WebCore {

class SocketStreamHandleClient;

class SocketStreamHandleImpl final : public SocketStreamHandle, public CurlStream::Client {
public:
    // The arguments WebSocketChannel passes to the CFNetwork version; the session and audit token have no meaning here.
    static Ref<SocketStreamHandleImpl> create(const URL& url, SocketStreamHandleClient& client, PAL::SessionID, const String&,
        SourceApplicationAuditToken&&, const StorageSessionProvider* provider, bool shouldAcceptInsecureCertificates)
    {
        return adoptRef(*new SocketStreamHandleImpl(url, client, provider, shouldAcceptInsecureCertificates));
    }

    ~SocketStreamHandleImpl();

    size_t bufferedAmount() final { return m_bufferedAmount; }

    // lw_network_changed, Web thread: the phone moved to another network, and every socket's connection stayed on the
    // old one. Each fails now as a dropped connection does (the page's onclose, 1006), so the page reconnects at once
    // instead of waiting for its own heartbeat to time out.
    static void failAllForNetworkChange();

private:
    SocketStreamHandleImpl(const URL&, SocketStreamHandleClient&, const StorageSessionProvider*, bool shouldAcceptInsecureCertificates);

    void platformSend(std::span<const uint8_t>, Function<void(bool)>&&) final;
    void platformSendHandshake(std::span<const uint8_t>, const std::optional<CookieRequestHeaderFieldProxy>&, Function<void(bool, bool)>&&) final;
    void platformClose() final;

    // CurlStream::Client, on the Web thread.
    void didOpen(CurlStreamID) final;
    void didSendData(CurlStreamID, size_t) final;
    void didReceiveData(CurlStreamID, const SharedBuffer&) final;
    void didFail(CurlStreamID, CURLcode, CertificateInfo&&) final;
    void didReceiveErrorMessage(CurlStreamID, String&&) final;

    bool queue(Vector<std::span<const uint8_t>, 2>&&);
    void destroyStream();

    CurlStreamID m_streamID { invalidCurlStreamID };
    RefPtr<const StorageSessionProvider> m_storageSessionProvider;
    // Handed to curl's thread and not yet written to the socket: what the page sees as WebSocket.bufferedAmount.
    size_t m_bufferedAmount { 0 };
    String m_errorMessage;
    bool m_notifiedClose { false };
};

} // namespace WebCore
