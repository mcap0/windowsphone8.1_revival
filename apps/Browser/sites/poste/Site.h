#pragma once

// Poste: poste.it as a thin site app (PLAN §64.1). Same shell as Internet (apps/Browser), built by tools/siteapp poste.
namespace Site
{
    const bool app = true;
    const wchar_t* const name = L"Poste";
    const wchar_t* const start = L"https://www.poste.it/";
    const wchar_t* const hosts = L"poste.it posteitaliane.it postepay.it";
    const unsigned int color = 0xFF0047BB; // PIN screen (0xAARRGGBB), the icon colour
    const wchar_t* const log = L"poste";
}
