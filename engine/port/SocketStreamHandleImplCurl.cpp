#include "config.h"
#include "SocketStreamHandleImplCurl.h"

#include "SocketStreamHandleClient.h"
#include <WebCore/CookieRequestHeaderFieldProxy.h>
#include <WebCore/CurlContext.h>
#include <WebCore/CurlStreamScheduler.h>
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/SocketStreamError.h>
#include <wtf/UniqueArray.h>

namespace WebCore {

// What a page may hand the socket before send() fails: messages beyond this wait in the page, not in the phone's memory.
static constexpr size_t maximumBufferedAmount = 16 * 1024 * 1024;

static CurlStreamScheduler& scheduler()
{
    return CurlContext::singleton().streamScheduler();
}

// Every socket of every tab, for failAllForNetworkChange. Web thread.
static HashSet<SocketStreamHandleImpl*>& liveHandles()
{
    static NeverDestroyed<HashSet<SocketStreamHandleImpl*>> handles;
    return handles;
}

void SocketStreamHandleImpl::failAllForNetworkChange()
{
    Vector<Ref<SocketStreamHandleImpl>> handles;
    for (auto* handle : liveHandles())
        handles.append(*handle);
    for (auto& handle : handles) {
        if (handle->m_state == Closed || handle->m_streamID == invalidCurlStreamID)
            continue;
        handle->m_errorMessage = "The phone moved to another network"_s;
        handle->didFail(handle->m_streamID, CURLE_RECV_ERROR, { });
    }
}

SocketStreamHandleImpl::SocketStreamHandleImpl(const URL& url, SocketStreamHandleClient& client, const StorageSessionProvider* provider, bool shouldAcceptInsecureCertificates)
    : SocketStreamHandle(url, client)
    , m_storageSessionProvider(provider)
{
    liveHandles().add(this);
    // ws: and wss: become http: and https: inside CurlStream, which connects (TLS included) on curl's thread.
    m_streamID = scheduler().createStream(url, *this,
        shouldAcceptInsecureCertificates ? CurlStream::ServerTrustEvaluation::Disable : CurlStream::ServerTrustEvaluation::Enable,
        CurlStream::LocalhostAlias::Disable);
}

SocketStreamHandleImpl::~SocketStreamHandleImpl()
{
    liveHandles().remove(this);
    destroyStream();
}

// The pieces go to curl's thread as one buffer, written in order when the socket can take them.
bool SocketStreamHandleImpl::queue(Vector<std::span<const uint8_t>, 2>&& pieces)
{
    if (m_streamID == invalidCurlStreamID)
        return false;
    size_t length = 0;
    for (auto& piece : pieces)
        length += piece.size();
    if (!length)
        return true;
    if (m_bufferedAmount + length > maximumBufferedAmount)
        return false;
    auto buffer = makeUniqueArray<uint8_t>(length);
    size_t offset = 0;
    for (auto& piece : pieces) {
        memcpySpan(std::span { buffer.get() + offset, piece.size() }, piece);
        offset += piece.size();
    }
    scheduler().send(m_streamID, WTF::move(buffer), length);
    m_bufferedAmount += length;
    m_client.didUpdateBufferedAmount(*this, m_bufferedAmount);
    return true;
}

void SocketStreamHandleImpl::platformSend(std::span<const uint8_t> data, Function<void(bool)>&& completionHandler)
{
    completionHandler(queue({ data }));
}

// The handshake goes out in one piece with the site's cookies, as CFNetwork's version sends it: the client's message
// ends with an empty line, and the Cookie header goes in before it.
void SocketStreamHandleImpl::platformSendHandshake(std::span<const uint8_t> data, const std::optional<CookieRequestHeaderFieldProxy>& headerFieldProxy, Function<void(bool, bool)>&& completionHandler)
{
    CString cookieHeader;
    bool secureCookiesAccessed = false;
    if (headerFieldProxy) {
        auto* session = m_storageSessionProvider ? m_storageSessionProvider->storageSession() : nullptr;
        if (!session)
            return completionHandler(false, false);
        auto [cookies, secureAccessed] = session->cookieRequestHeaderFieldValue(*headerFieldProxy);
        secureCookiesAccessed = secureAccessed;
        if (!cookies.isEmpty())
            cookieHeader = makeString("Cookie: "_s, cookies, "\r\n\r\n"_s).utf8();
    }
    if (!cookieHeader.isNull()) {
        ASSERT(data.size() > 2 && data[data.size() - 2] == '\r' && data[data.size() - 1] == '\n');
        data = data.first(data.size() - 2);
    }
    Vector<std::span<const uint8_t>, 2> pieces { data };
    if (!cookieHeader.isNull())
        pieces.append(byteCast<uint8_t>(cookieHeader.span()));
    completionHandler(queue(WTF::move(pieces)), secureCookiesAccessed);
}

// Called once by SocketStreamHandle::disconnect(): no more callbacks from curl, and the client hears the close once.
void SocketStreamHandleImpl::platformClose()
{
    destroyStream();
    if (std::exchange(m_notifiedClose, true))
        return;
    m_client.didCloseSocketStream(*this);
}

void SocketStreamHandleImpl::destroyStream()
{
    if (m_streamID == invalidCurlStreamID)
        return;
    scheduler().destroyStream(std::exchange(m_streamID, invalidCurlStreamID));
}

void SocketStreamHandleImpl::didOpen(CurlStreamID)
{
    if (m_state != Connecting)
        return;
    m_state = Open;
    m_client.didOpenSocketStream(*this);
}

void SocketStreamHandleImpl::didSendData(CurlStreamID, size_t length)
{
    ASSERT(length <= m_bufferedAmount);
    m_bufferedAmount -= std::min(length, m_bufferedAmount);
    m_client.didUpdateBufferedAmount(*this, m_bufferedAmount);
    // close() waits for what was already queued: the closing frame goes out before the socket does.
    if (m_state == Closing && !m_bufferedAmount)
        disconnect();
}

void SocketStreamHandleImpl::didReceiveData(CurlStreamID, const SharedBuffer& buffer)
{
    if (m_state != Open && m_state != Closing)
        return;
    // CurlStream reports the server closing the connection as an empty read.
    if (!buffer.size()) {
        Ref protectedThis { *this };
        disconnect();
        return;
    }
    m_client.didReceiveSocketStreamData(*this, buffer.span());
}

void SocketStreamHandleImpl::didReceiveErrorMessage(CurlStreamID, String&& message)
{
    m_errorMessage = WTF::move(message);
}

// The channel logs the error for the page's console and disconnects (platformClose).
void SocketStreamHandleImpl::didFail(CurlStreamID, CURLcode errorCode, CertificateInfo&&)
{
    if (m_state == Closed)
        return;
    String description = !m_errorMessage.isEmpty() ? m_errorMessage : String::fromLatin1(curl_easy_strerror(errorCode));
    m_client.didFailSocketStream(*this, SocketStreamError(static_cast<int>(errorCode), m_url.string(), description));
}

} // namespace WebCore
