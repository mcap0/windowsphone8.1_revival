# Live notifications on Windows Phone 8.1 with your own push server

Windows Phone 8.1 apps cannot run in the background freely: the only way to show a notification *the moment
something happens*, with the app closed and the phone locked, is the system push client (WNS, and MPNS for
Silverlight apps). That client keeps one connection to Microsoft open and hands incoming notifications to the shell.
Microsoft's side is no longer something you can register apps with, so new apps get nothing.

The fix here: **leave the phone's push client untouched and give it a different server**. Your apps keep using the
normal, documented push APIs; the channel URI they get simply points to your server, and a notification is one HTTP
POST to that URI.

```
your service ──HTTP POST /push/<channel>──▶ push server ══ persistent TCP/TLS ══▶ phone push client (WPNPRV)
 (anything)      X-WNS-Type: wns/toast       wnsfake / WnsServer                    └▶ toast / tile / badge / raw to the app
```

Measured on a Lumia 1020: a toast is acknowledged by the phone 130–700 ms after the POST, on Wi-Fi and on 4G,
locked or not.

Contents: [requirements](#1-requirements) · [server](#2-run-the-server) · [phone](#3-point-the-phone-at-it) ·
[app](#4-in-your-app-get-a-channel) · [sending](#5-send-a-notification) · [4G](#6-away-from-home-4g) ·
[raw pushes and calls](#7-raw-pushes-and-ringing-like-a-phone-call) · [protocol](#8-the-protocol) ·
[pitfalls](#9-pitfalls-we-hit) · [security](#10-security)

## 1. Requirements

- A phone where you can write to `HKLM` (interop unlock + capability unlock, or any registry editor with that
  access). The repo's `apps/Esplora` has a remote console with `regset`/`regdel`; any tool works.
- A machine that is always on and reachable by the phone: a PC on the LAN for a first test, then something public
  (section 6).
- Python 3 for `tools/wnsfake`. No dependencies.

One thing to accept first: the setting is **global**. Every WNS/MPNS client on the phone talks to your server from
then on, and nothing from Microsoft's push service arrives any more.

## 2. Run the server

```sh
tools/wnsfake --bind <LAN address of the PC> --port 8799 --http-port 8800 \
              --public-url http://<name that resolves to the PC>:8800 --data ./wns-data
```

- port 8799: where the phone connects (plain TCP here; `--tls CERT KEY` for TLS)
- port 8800: the HTTP API you push to
- `--public-url`: the base of the channel URIs handed to apps. Whatever sends pushes must be able to reach it.
- `--token SECRET`: require an `X-Token` header on pushes. Use it whenever the API port is not loopback.

`GET http://<pc>:8800/status` shows connections and channels.

## 3. Point the phone at it

Registry key `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\PushNotifications`:

| Value | Type | Meaning |
|---|---|---|
| `Server` | REG_SZ | `<dns name>:<port>` — replaces Microsoft's host unconditionally (default port 443) |
| `DisableTLS` | DWORD | `1` = plain TCP |
| `DisableTLSCertCheck` | DWORD | `1` = TLS without checking the certificate |

For the LAN test: `Server = 192-168-1-20.sslip.io:8799`, `DisableTLS = 1`. Then **reboot**: the value is read once
when the push service starts.

**`Server` must be a DNS name, never an IP literal.** The client resolves it with an asynchronous
`GetAddrInfoExW` and only treats "pending" as progress; for a numeric address Winsock answers synchronously, the
completion routine never runs, and the client waits forever without sending a single packet. A wildcard DNS
service (`<a-b-c-d>.sslip.io` resolves to `a.b.c.d`) or your router's local DNS solves it.

The client connects on demand: expect the first connection when an app first asks for a channel, not at boot.

To undo: delete the three values and reboot.

## 4. In your app: get a channel

Nothing special. Use the stock API for your app model; the URI you get back starts with your `--public-url`.

**Windows Runtime app (C++/CX; C# is the same call)** — declare the *Internet* capability and mark the app
*Toast capable* in the manifest:

```cpp
using namespace Windows::Networking::PushNotifications;

create_task(PushNotificationChannelManager::CreatePushNotificationChannelForApplicationAsync())
    .then([](PushNotificationChannel^ channel)
{
    // keep `channel` alive for the run; send channel->Uri to whatever will push to this phone
    String^ uri = channel->Uri;      // http://<public-url>/push/<32 hex digits>
});
```

**Silverlight 8.1 app (C#)** — capability `ID_CAP_PUSH_NOTIFICATION`:

```csharp
var ch = HttpNotificationChannel.Find("main") ?? new HttpNotificationChannel("main");
ch.ChannelUriUpdated += (s, e) => Upload(e.ChannelUri);
if (ch.ChannelUri == null) ch.Open(); else Upload(ch.ChannelUri);
if (!ch.IsShellToastBound) ch.BindToShellToast();
```

Then get the URI to your sender: that step is yours (your app's own connection to your service, a settings page,
anything). Three practical rules:

- Ask for the channel on every start and re-send the URI if it changed. Channels are issued for 30 days and renewed
  by the system; the server gives the same app the same channel while it knows it.
- Store the URI in `LocalSettings` so a background task can re-send it after a phone reboot without the user
  opening the app.
- While pushes work, stop making the same toasts from a timer background task, or they show twice.

## 5. Send a notification

```sh
curl -X POST "http://<public-url>/push/<channel id>" \
  -H 'X-WNS-Type: wns/toast' -H 'X-WNS-Tag: chat42' -H 'X-WNS-Group: msg' -H 'X-TTL: 3600' \
  --data-binary '<toast launch="chat?id=42"><visual><binding template="ToastText02">
    <text id="1">Anna</text><text id="2">Are you coming tonight?</text></binding></visual></toast>'
```

| Header | Meaning |
|---|---|
| `X-WNS-Type` | `wns/toast`, `wns/tile`, `wns/badge`, `wns/raw` (default raw) |
| `X-WNS-Tag`, `X-WNS-Group` | at most 16 characters each; a newer toast with the same tag and group replaces the older one in the action centre (one toast per conversation instead of a pile) |
| `X-TTL` | seconds to keep the push if the phone is away (default 60) |
| `X-Token` | the server's `--token`, if set |

Answer: `200 {"stato":"ack","ms":N}` when the phone acknowledged it, `202` when it was queued.

Payloads are the standard ones: the WNS toast/tile/badge XML schemas for Runtime apps (`launch` is the argument your
app receives in `OnLaunched` when the toast is tapped), the MPNS toast XML (`<wp:Notification><wp:Toast>…`) for
Silverlight channels. Escape `& < > " '` in text.

## 6. Away from home (4G)

The push client is a system service: it cannot use an in-app VPN or the Tailscale library in this repo. It needs
a name it can reach from the mobile network. What is used here costs nothing and opens no router port:

1. Run the server where it is always on (here: an old Android phone, `ponte/agent/.../WnsServer.java`, the same
   protocol in Java, listening on `127.0.0.1:8799`, API on `127.0.0.1:8800`).
2. Put that machine on a tailnet and publish the phone port with **Tailscale Funnel**, TLS ended by tailscaled:
   `tailscale funnel --bg --tls-terminated-tcp=443 tcp://127.0.0.1:8799`
3. Phone registry: `Server = <node>.<tailnet>.ts.net:443`, `DisableTLS = 0`, `DisableTLSCertCheck = 1`, reboot.
   WP8.1 negotiates TLS 1.2 ECDHE-ECDSA-AES128-GCM with Funnel's Let's Encrypt certificate; the check is off because
   the phone's root store predates the current roots (installing ISRG Root X1 and leaving the check on is better).

Any public host with a DNS name does the same job (a VPS with `--tls`). The API port stays private: only your
sender needs it.

## 7. Raw pushes and ringing like a phone call

A toast needs nothing from the app. A **raw** push runs your code, and where it is delivered depends on the app:

| App | Raw push goes to |
|---|---|
| Runtime app, running | `PushNotificationReceived` |
| Runtime app, closed | a background task with a `PushNotificationTrigger` |
| Silverlight app in the foreground | `HttpNotificationReceived` |
| Silverlight app registered as a **phone VoIP app** | the VoIP agent (`VoipHttpIncomingCallTask`), even closed and locked |

The last row is how a locked phone rings with the system incoming-call screen (`apps/ProvaVoip` is the working
test). The points that took time:

- `VoipCallCoordinator` only works in a Silverlight 8.1 package.
- The package must declare `AgHost.BackgroundVoipTask` (a `controlChannel` background task), and that requires an
  out-of-process WinRT server (`ServerName`), or the build fails with APPX1684.
- That server exe must not import the C runtime: a Silverlight package has no VCLibs framework dependency, so an
  exe that imports `msvcr120_app.dll` never starts and `VoipBackgroundProcess.Launch` times out
  (0x80131509 / 0x80070102). `apps/ProvaVoip/VoipHost/Host.cpp` is a 3.5 KB CRT-free host
  (`CoreApplication::RunWithActivationFactories`, nothing else).
- In the agent, call `RequestNewIncomingCall` and **do not** call `NotifyComplete` until the call is answered,
  rejected or timed out: completing early ends the VoIP process and the ringing with it.

## 8. The protocol

Read from `WPNPRV.DLL` (with Microsoft's public symbols). Every message, both ways:

```
<VERB> <transaction id> <target> <length>\r\n
<length> bytes: MIME-like headers, blank line, body (XML)
```

| From phone | Purpose | Server answers |
|---|---|---|
| `CNT n CON` | connect, device info; a fast reconnect carries `<session><id>` | same verb/id, `<connect-response><session><id/><timeout/>` |
| `ATH n CON\…` | account ticket (meant for Microsoft; ignored, never stored) | same verb/id |
| `BND n CON\WNS` or `CON\QOS` | bind; the phone opens a second, `QOS` connection that never acknowledges pushes | same verb/id |
| `PNG n CON 0` | keep-alive (about every 15 minutes) | same |
| `GET n WNS\CHANNEL` | channel request: `<channel><appkey>…` | `<channel><id/><url/><exp/></channel>` — `url` is what the app receives |
| `PUT` / `DEL n WNS\CHANNEL` | renew / revoke | same shape |
| `ACK n WNS\NOTIF` | acknowledges a push (`Msg-Id`) | — |

Server to phone: `NFY 0 WNS\NOTIF` with headers `Context`, `Msg-Id`, `Ack: true`, `Time`, `Type` (`wns/raw|toast|tile|badge`),
`Channel`, optional `Tag`/`Group`, then the body.

Details that matter when writing your own server:

- The server never has to send a `CHALLENGE`, so Microsoft's challenge algorithm is not needed.
- A fast reconnect inside the session timeout sends **only** `CNT` with the session id: remember each session's role
  (`WNS` or `QOS`) from its earlier `BND`.
- The phone echoes `Msg-Id` in hex without leading zeros: compare by value. It reports the last one it saw in `CNT`
  (`Last-Msg-Id`).
- App keys look like `mpn::windowsphonedefault_<product id>` for Silverlight channels; Runtime apps use the same
  `GET WNS\CHANNEL`.
- Send pushes one at a time on the `WNS` connection, each after the previous `ACK`. A push with no `ACK` in ~10 s
  should go back to the queue and the connection be closed: half-open connections happen when the phone changes
  network.

## 9. Pitfalls we hit

- IP literal in `Server`: the client never connects (section 3).
- Changed the registry, nothing happens: reboot.
- Behind a TLS terminator, **write each message with one `write`**. Header and body in separate writes become
  separate TLS records; the phone took ~24 s to react and then dropped the connection.
- Raw push acknowledged but the app saw nothing: see the table in section 7; a Silverlight app only gets raw pushes
  in the foreground unless it is a registered VoIP app.
- After a phone reboot the session is new: make the app (or its background task) hand its channel URI to your
  sender again.
- The phone's clock can be off by tens of seconds; do not compare its timestamps with the server's.

## 10. Security

`tools/wnsfake` assumes one phone on a network you trust: it pushes to every connected client. Do not expose it to
the internet as it is.

`WnsServer.java` is the version meant for a public port. Since anyone can connect and speak the protocol, it sends
pushes only to **trusted** connections, and never reveals an existing channel to a stranger:

- a session id it issued to an already trusted connection (fast reconnect);
- a full connect whose `Last-Msg-Id` is a push that a trusted connection acknowledged (ids advance by random steps);
- a renewal of a channel issued to a trusted session (channel ids are 128 random bits);
- `POST /trust/<channel>` on the private API: your sender calls it when the app has handed over that channel through
  its own authenticated link, which proves the session the channel was issued on is your phone.

An untrusted connection that asks for a channel gets a fresh one, useless until trusted. Message text is never
logged. Expect scanners within minutes of the TLS certificate appearing in certificate-transparency logs.

Notification text crosses your server in clear inside the TLS tunnel you control; with `DisableTLS = 1` it crosses
the network in clear, so keep that for a LAN test.
