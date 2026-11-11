// Tile map control: our own OSM raster viewer (no MapControl, no HERE services).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pch.h"
#include "Core/LruCache.h"
#include "Core/Viewport.h"

namespace MappeCpp {

namespace detail {

// One Image in the tile layer and what it currently shows.
struct Slot {
    Windows::UI::Xaml::Controls::Image^ img;
    std::int64_t source;  // key of the bitmap shown, -1 when none
    int depth;            // 0 = own tile, n = ancestor n levels up
};

struct Stats {
    int requested;
    int ok;
    int failed;
    int diskHits;
    int netFetches;
    std::wstring lastError;
    std::wstring lastHttp;
    std::wstring lastTile;
    Stats() : requested(0), ok(0), failed(0), diskHits(0), netFetches(0) {}
};

struct DiskEntry {
    Windows::Storage::StorageFile^ file;
    unsigned long long size;
    long long modified;
};

}  // namespace detail

// Arguments of LongPressed: the spot under the finger.
public ref class LatLonArgs sealed {
public:
    property double Lat;
    property double Lon;
};

public ref class TileMapView sealed : public Windows::UI::Xaml::Controls::Grid {
public:
    TileMapView();

    event Windows::Foundation::EventHandler<LatLonArgs^>^ LongPressed;

    void CenterOn(double lat, double lon, double zoom);
    double CenterLat();
    double CenterLon();
    double CurrentZoom();

    void SetUserLocation(double lat, double lon, double accuracy);
    void SetSearchPin(double lat, double lon, Platform::String^ title);
    void ClearSearchPin();
    void SetPressPin(double lat, double lon);
    void ClearPressPin();

    // Lines for Diagnostica: tile counters, last error, cache size.
    Platform::String^ StatsText();
    // Short state for the status line: tiles waiting or loading.
    Platform::String^ LoadingStatus();

private:
    void OnManipulationStarted(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationStartedRoutedEventArgs^ e);
    void OnManipulationDelta(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e);
    void OnManipulationCompleted(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs^ e);
    void OnDoubleTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::DoubleTappedRoutedEventArgs^ e);
    void OnHolding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
    void OnSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);
    void OnAnimTick(Platform::Object^ sender, Platform::Object^ e);

    void Relayout();
    void PlaceOverlays();
    Windows::UI::Xaml::Controls::Image^ TakeImage();
    void RequestLoad(std::int64_t key);
    void PumpLoads();
    void StartLoad(std::int64_t key);
    void FailLoad(std::int64_t key, const std::wstring& msg);

    void StartAnimation(double zoom, double px, double py);
    void StopAnimation();

    concurrency::task<void> LoadTileAsync(std::int64_t key);
    concurrency::task<Platform::Array<unsigned char>^> ReadDiskAsync(const std::wstring& name);
    concurrency::task<Platform::Array<unsigned char>^> DownloadAsync(int z, int x, int y);
    concurrency::task<Windows::UI::Xaml::Media::Imaging::BitmapImage^> DecodeAsync(Platform::Array<unsigned char>^ bytes);
    void SaveToDisk(const std::wstring& name, Platform::Array<unsigned char>^ bytes);
    void ScanAndEvict();

    Windows::UI::Xaml::Controls::Canvas^ _tileLayer;
    Windows::UI::Xaml::Controls::Canvas^ _overlayLayer;
    Windows::UI::Xaml::Shapes::Ellipse^ _userDot;
    Windows::UI::Xaml::Shapes::Ellipse^ _userAccuracy;
    Windows::UI::Xaml::Shapes::Ellipse^ _pressDot;
    Windows::UI::Xaml::Shapes::Ellipse^ _searchDot;
    Windows::UI::Xaml::Controls::Border^ _searchLabel;
    Windows::UI::Xaml::Controls::TextBlock^ _searchText;
    Windows::UI::Xaml::DispatcherTimer^ _animTimer;
    concurrency::task<Windows::Storage::StorageFolder^> _folderTask;

    mappe::Viewport _vp;
    std::unordered_map<std::int64_t, detail::Slot> _slots;
    std::vector<Windows::UI::Xaml::Controls::Image^> _freeImages;
    mappe::LruCache<std::int64_t, Windows::UI::Xaml::Media::Imaging::BitmapImage^> _decoded;
    std::vector<std::int64_t> _wanted;
    std::unordered_set<std::int64_t> _inflight;
    std::unordered_map<std::int64_t, long long> _retryAfter;
    int _activeLoads;
    detail::Stats _stats;
    long long _diskBytes;  // -1 until the first scan
    bool _evicting;

    bool _hasUser;
    double _userLat, _userLon, _userAcc;
    bool _hasSearch;
    double _searchLat, _searchLon;
    bool _hasPress;
    double _pressLat, _pressLon;

    bool _pinching;
    double _pivotX, _pivotY;
    bool _animating;
    double _animFrom, _animTo, _animPx, _animPy;
    long long _animStart;
};

}  // namespace MappeCpp
