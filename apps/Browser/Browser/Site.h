#pragma once

// What this build of the shell is. Internet itself (here) is the full browser. A thin site app (PLAN §64.1) is the same
// shell and engine built by tools/siteapp with the Site.h of apps/Browser/sites/<name>/: no address bar, tabs,
// favourites or history, its own data folder (cookies), session cookies kept, and the login filled from the PIN vault
// (Vault.h).
namespace Site
{
    const bool app = false;
    // Shown in the bar and on the PIN screen.
    const wchar_t* const name = L"Internet";
    // First page when there is no session to restore.
    const wchar_t* const start = L"";
    // Hosts that belong to the app, space-separated; each covers its subdomains. Links to other hosts with
    // target=_blank open in Internet, and the vault fills only pages of these hosts, over https.
    const wchar_t* const hosts = L"";
    // The PIN screen's colour (band, OK key, buttons), 0xAARRGGBB: the app's icon colour.
    const unsigned int color = 0xFF1976D2;
    // share/inbox/<log>.txt on the PC (tools/inbox).
    const wchar_t* const log = L"internet";
}
