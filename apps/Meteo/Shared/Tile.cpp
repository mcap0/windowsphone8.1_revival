#include <collection.h>
#include <ppltasks.h>
#include <windows.h>
#include <string>
#include "Tile.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Xml::Dom;
using namespace Windows::Foundation;
using namespace Windows::UI::Notifications;
using namespace Windows::UI::StartScreen;

namespace MeteoCore {

namespace {

std::wstring Escape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'&') r += L"&amp;";
        else if (c == L'<') r += L"&lt;";
        else if (c == L'>') r += L"&gt;";
        else if (c == L'\'') r += L"&apos;";
        else if (c == L'"') r += L"&quot;";
        else r += c;
    }
    return r;
}

// Haru's white ("mono") pictures: the tile stays white on the accent colour like the system tiles
std::wstring Picture(const Forecast& f, bool wide) {
    return std::wstring(L"ms-appx:///Assets/Haru/tile-") + Icon(f.symbol, f.day) + (wide ? L"_wide_mono.png" : L"_mono.png");
}

void Send(TileUpdater^ updater, const std::wstring& xml, const wchar_t* tag) {
    auto doc = ref new XmlDocument();
    doc->LoadXml(ref new String(xml.c_str()));
    auto notification = ref new TileNotification(doc);
    notification->Tag = ref new String(tag);
    updater->Update(notification);
}

// front: the picture with the tile's own label; back: temperature, sky, min/max
void Fill(TileUpdater^ updater, const Forecast& f) {
    std::wstring temp = Escape(Degrees(f.temp));
    std::wstring sky = Escape(Describe(f.symbol));
    std::wstring range = Escape(Degrees(f.days[0].tmin) + L" / " + Degrees(f.days[0].tmax));
    std::wstring front =
        L"<tile><visual version='2' branding='name'>"
        L"<binding template='TileSquare150x150Image' fallback='TileSquareImage'>"
        L"<image id='1' src='" + Picture(f, false) + L"'/></binding>"
        L"<binding template='TileWide310x150Image' fallback='TileWideImage'>"
        L"<image id='1' src='" + Picture(f, true) + L"'/></binding>"
        L"</visual></tile>";
    std::wstring back =
        L"<tile><visual version='2' branding='name'>"
        L"<binding template='TileSquare150x150Text01' fallback='TileSquareText01'>"
        L"<text id='1'>" + temp + L"</text><text id='2'>" + sky + L"</text><text id='3'>" + Escape(f.updated) + L"</text></binding>"
        L"<binding template='TileWide310x150Text01' fallback='TileWideText01'>"
        L"<text id='1'>" + temp + L"</text><text id='2'>" + sky + L"</text><text id='3'>" + range + L"</text>"
        L"<text id='4'>" + Escape(f.updated) + L"</text></binding></visual></tile>";
    updater->EnableNotificationQueue(true);
    updater->Clear();
    Send(updater, back, L"retro");
    Send(updater, front, L"fronte");
}

}  // namespace

bool IsPinned(const Place& place) {
    return SecondaryTile::Exists(ref new String(TileId(place).c_str()));
}

// each tile on its own: one that fails ("application identifier provided is invalid") does not stop the other
void UpdateTile(const Forecast& f) {
    if (f.days.empty()) return;
    if (SamePlace(f.place, Home())) {
        try {
            Fill(TileUpdateManager::CreateTileUpdaterForApplication(), f);
        } catch (Exception^ ex) {
            Log(L"tile app: " + std::wstring(ex->Message->Data()) + L" -> riprovo con Id App");
            try {
                Fill(TileUpdateManager::CreateTileUpdaterForApplication(L"App"), f);
            } catch (Exception^ ex2) {
                Log(L"tile app (Id App): " + std::wstring(ex2->Message->Data()));
            }
        }
    }
    std::wstring id = TileId(f.place);
    try {
        if (IsPinned(f.place)) Fill(TileUpdateManager::CreateTileUpdaterForSecondaryTile(ref new String(id.c_str())), f);
    } catch (Exception^ ex) {
        Log(L"tile " + id + L": " + std::wstring(ex->Message->Data()));
    }
}

task<bool> PinTile(const Forecast& f) {
    auto tile = ref new SecondaryTile(ref new String(TileId(f.place).c_str()), ref new String(TileLabel(f.place).c_str()),
                                      ref new String(TileArguments(f.place).c_str()),
                                      ref new Uri(ref new String(Picture(f, false).c_str())), TileSize::Square150x150);
    tile->VisualElements->Wide310x150Logo = ref new Uri(ref new String(Picture(f, true).c_str()));
    tile->VisualElements->ShowNameOnSquare150x150Logo = true;
    tile->VisualElements->ShowNameOnWide310x150Logo = true;
    tile->VisualElements->ForegroundText = ForegroundText::Light;
    tile->RoamingEnabled = false;
    return create_task(tile->RequestCreateAsync());
}

}  // namespace MeteoCore
