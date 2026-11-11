#pragma once

// Gmail: mail.google.com as a thin site app (PLAN §64.1). Same shell as Internet (apps/Browser), built by
// tools/siteapp gmail. Google's sign-in asks the email first and the password on the next page: both are filled.
namespace Site
{
    const bool app = true;
    const wchar_t* const name = L"Gmail";
    const wchar_t* const start = L"https://mail.google.com/";
    const wchar_t* const hosts = L"google.com gmail.com";
    const unsigned int color = 0xFFC5221F; // PIN screen (0xAARRGGBB), the icon colour
    const wchar_t* const log = L"gmail";
}
