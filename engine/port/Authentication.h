// HTTP authentication: a server answered 401 and neither the address nor the session had a password for it
// (ResourceHandleCurl.cpp). The app asks the user (LW_EVENT_PASSWORD) and answers with lw_password_reply. Web thread only.
#pragma once

#include "lumiaweb.h"

namespace WebCore {
class AuthenticationChallenge;
}

namespace Lumia {

// The challenge's load waits; one question serves every load that waits for the same server and realm.
void askForPassword(lw_view&, const WebCore::AuthenticationChallenge&);
// The view is going away: its questions end without a password.
void cancelPasswordQuestions(lw_view&);

} // namespace Lumia
