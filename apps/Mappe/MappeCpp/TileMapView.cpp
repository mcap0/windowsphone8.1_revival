#include "pch.h"
#include "TileMapView.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "Core/Geo.h"
#include "Core/TileMath.h"
#include "Services.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::Storage::FileProperties;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;
using namespace Windows::UI::Xaml::Shapes;
using namespace Windows::Web::Http;

namespace MappeCpp {

namespace {
const char* const TileTemplate = "https://basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}.png";
const wchar_t* const UserAgentW = L"DumbPhoneMappe/0.2 (personal use)";
const unsigned long long DiskBudget = 50ULL * 1024ULL * 1024ULL;   // cache limit
const unsigned long long DiskTarget = 40ULL * 1024ULL * 1024ULL;   // after eviction
const int MaxParallelLoads = 2;
const int DecodedCapacity = 80;      // decoded bitmaps kept in memory
const long long RetryDelayMs = 30000;
const int MaxFallbackDepth = 3;
const double ZoomAnimMs = 180.0;

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

String^ Str(const std::wstring& s) { return ref new String(s.c_str()); }

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

Array<unsigned char>^ ToBytes(IBuffer^ buf) {
    Array<unsigned char>^ bytes = ref new Array<unsigned char>(buf->Length);
    DataReader::FromBuffer(buf)->ReadBytes(bytes);
    return bytes;
}

std::wstring MbText(long long bytes) {
    long long tenth = bytes * 10 / (1024LL * 1024LL);
    return std::to_wstring(tenth / 10) + L"," + std::to_wstring(tenth % 10) + L" MB";
}

// Hex HRESULT, the failing phase (added by ThrowAtStage) and the system text.
std::wstring Describe(Platform::Exception^ ex) {
    std::wostringstream os;
    os << L"HRESULT 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0')
       << static_cast<unsigned int>(ex->HResult);
    std::wstring text = ex->Message != nullptr ? std::wstring(ex->Message->Data()) : std::wstring();
    if (!text.empty()) os << L" - " << text;
    return os.str();
}

// Rethrows a WinRT failure keeping its HRESULT, with the phase in the message.
void ThrowAtStage(const wchar_t* stage, Platform::Exception^ ex) {
    std::wstring msg = std::wstring(L"[") + stage + L"] " + Describe(ex);
    throw Platform::Exception::CreateException(ex->HResult, Str(msg));
}

// Copies an IBuffer into a byte array.
Array<unsigned char>^ CopyBytes(IBuffer^ buf) {
    Array<unsigned char>^ bytes = ref new Array<unsigned char>(buf->Length);
    DataReader::FromBuffer(buf)->ReadBytes(bytes);
    return bytes;
}
}  // namespace

TileMapView::TileMapView()
    : _folderTask(),
      _activeLoads(0),
      _decoded(DecodedCapacity),
      _diskBytes(-1),
      _evicting(false),
      _hasUser(false), _userLat(0), _userLon(0), _userAcc(0),
      _hasSearch(false), _searchLat(0), _searchLon(0),
      _hasPress(false), _pressLat(0), _pressLon(0),
      _pinching(false), _pivotX(0), _pivotY(0),
      _animating(false), _animFrom(0), _animTo(0), _animPx(0), _animPy(0), _animStart(0) {
    _vp.Zoom = 6.0;
    _vp.SetCenterLatLon(41.9, 12.5);

    // Transparent background so the control receives touch input over the whole area.
    this->Background = ref new SolidColorBrush(Colors::Transparent);
    this->ManipulationMode = ManipulationModes::Scale | ManipulationModes::TranslateX |
                             ManipulationModes::TranslateY | ManipulationModes::TranslateInertia;

    _tileLayer = ref new Canvas();
    _overlayLayer = ref new Canvas();
    this->Children->Append(_tileLayer);
    this->Children->Append(_overlayLayer);

    Color accFill;
    accFill.A = 0x40; accFill.R = 0x1E; accFill.G = 0x90; accFill.B = 0xFF;
    _userAccuracy = ref new Ellipse();
    _userAccuracy->Fill = ref new SolidColorBrush(accFill);

    _userDot = ref new Ellipse();
    _userDot->Width = 18; _userDot->Height = 18;
    _userDot->Fill = ref new SolidColorBrush(Colors::DodgerBlue);
    _userDot->Stroke = ref new SolidColorBrush(Colors::White);
    _userDot->StrokeThickness = 3;

    _pressDot = ref new Ellipse();
    _pressDot->Width = 12; _pressDot->Height = 12;
    _pressDot->Fill = ref new SolidColorBrush(Colors::Orange);
    _pressDot->Stroke = ref new SolidColorBrush(Colors::White);
    _pressDot->StrokeThickness = 2;

    _searchDot = ref new Ellipse();
    _searchDot->Width = 14; _searchDot->Height = 14;
    _searchDot->Fill = ref new SolidColorBrush(Colors::Red);
    _searchDot->Stroke = ref new SolidColorBrush(Colors::White);
    _searchDot->StrokeThickness = 2;

    Color labelBg;
    labelBg.A = 0xE0; labelBg.R = 0; labelBg.G = 0; labelBg.B = 0;
    _searchText = ref new TextBlock();
    _searchText->FontSize = 14;
    _searchText->Foreground = ref new SolidColorBrush(Colors::White);
    _searchText->TextWrapping = TextWrapping::Wrap;
    _searchText->TextAlignment = TextAlignment::Center;
    _searchLabel = ref new Border();
    _searchLabel->Width = 240;
    _searchLabel->Background = ref new SolidColorBrush(labelBg);
    _searchLabel->Padding = Thickness(6, 2, 6, 2);
    _searchLabel->Child = _searchText;

    UIElement^ overlays[] = { _userAccuracy, _userDot, _pressDot, _searchLabel, _searchDot };
    for (UIElement^ el : overlays) {
        el->IsHitTestVisible = false;
        el->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        _overlayLayer->Children->Append(el);
    }

    _animTimer = ref new DispatcherTimer();
    TimeSpan tick;
    tick.Duration = 160000LL;  // 16 ms
    _animTimer->Interval = tick;
    _animTimer->Tick += ref new EventHandler<Object^>(this, &TileMapView::OnAnimTick);

    this->ManipulationStarted += ref new ManipulationStartedEventHandler(this, &TileMapView::OnManipulationStarted);
    this->ManipulationDelta += ref new ManipulationDeltaEventHandler(this, &TileMapView::OnManipulationDelta);
    this->ManipulationCompleted += ref new ManipulationCompletedEventHandler(this, &TileMapView::OnManipulationCompleted);
    this->DoubleTapped += ref new DoubleTappedEventHandler(this, &TileMapView::OnDoubleTapped);
    this->Holding += ref new HoldingEventHandler(this, &TileMapView::OnHolding);
    this->SizeChanged += ref new SizeChangedEventHandler(this, &TileMapView::OnSizeChanged);

    _folderTask = create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
        ref new String(L"tiles"), CreationCollisionOption::OpenIfExists));
}

// ---- Public API -------------------------------------------------------------

void TileMapView::CenterOn(double lat, double lon, double zoom) {
    StopAnimation();
    _vp.Zoom = _vp.ClampZoom(zoom);
    _vp.SetCenterLatLon(lat, lon);
    Relayout();
}

double TileMapView::CenterLat() {
    double lat, lon;
    _vp.CenterLatLon(lat, lon);
    return lat;
}

double TileMapView::CenterLon() {
    double lat, lon;
    _vp.CenterLatLon(lat, lon);
    return lon;
}

double TileMapView::CurrentZoom() {
    return _vp.Zoom;
}

void TileMapView::SetUserLocation(double lat, double lon, double accuracy) {
    _hasUser = true;
    _userLat = lat;
    _userLon = lon;
    _userAcc = accuracy;
    PlaceOverlays();
}

void TileMapView::SetSearchPin(double lat, double lon, String^ title) {
    _hasSearch = true;
    _searchLat = lat;
    _searchLon = lon;
    _searchText->Text = title;
    PlaceOverlays();
}

void TileMapView::ClearSearchPin() {
    _hasSearch = false;
    PlaceOverlays();
}

void TileMapView::SetPressPin(double lat, double lon) {
    _hasPress = true;
    _pressLat = lat;
    _pressLon = lon;
    PlaceOverlays();
}

void TileMapView::ClearPressPin() {
    _hasPress = false;
    PlaceOverlays();
}

String^ TileMapView::StatsText() {
    std::wstring t;
    t += L"Tile: richieste " + std::to_wstring(_stats.requested) + L", ok " + std::to_wstring(_stats.ok) +
         L", errori " + std::to_wstring(_stats.failed) + L", da disco " + std::to_wstring(_stats.diskHits) +
         L", scaricate " + std::to_wstring(_stats.netFetches) + L"\n";
    t += L"In memoria (decodificate): " + std::to_wstring(_decoded.Size()) + L" di " +
         std::to_wstring(DecodedCapacity) + L", in coda " + std::to_wstring(_wanted.size()) +
         L", in caricamento " + std::to_wstring(_activeLoads) + L"\n";
    t += L"Cache disco: " + (_diskBytes < 0 ? std::wstring(L"non ancora misurata") : MbText(_diskBytes) + L" (limite 50 MB)") + L"\n";
    t += L"Ultimo errore tile: " + (_stats.lastTile.empty() ? std::wstring(L"-") : _stats.lastTile) + L" -> " +
         (_stats.lastError.empty() ? std::wstring(L"-") : _stats.lastError) + L"\n";
    t += L"Ultimo errore HTTP: " + (_stats.lastHttp.empty() ? std::wstring(L"-") : _stats.lastHttp) + L"\n";
    t += L"Ultimo errore scrittura cache: " + (_stats.lastDiskError.empty() ? std::wstring(L"-") : _stats.lastDiskError);
    if (!_probeLog.empty()) t += L"\n" + _probeLog;
    return Str(t);
}

String^ TileMapView::LoadingStatus() {
    return Str(L"tile in coda " + std::to_wstring(_wanted.size()) + L", in caricamento " + std::to_wstring(_activeLoads));
}

// ---- Layout -----------------------------------------------------------------

Image^ TileMapView::TakeImage() {
    if (!_freeImages.empty()) {
        Image^ reused = _freeImages.back();
        _freeImages.pop_back();
        return reused;
    }
    Image^ img = ref new Image();
    img->Stretch = Stretch::Fill;
    img->IsHitTestVisible = false;
    img->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    _tileLayer->Children->Append(img);
    return img;
}

void TileMapView::Relayout() {
    if (_vp.Width <= 0.0 || _vp.Height <= 0.0) return;

    std::vector<mappe::TileCell> cells = _vp.VisibleTiles(1);
    std::unordered_set<std::int64_t> needed;
    for (const mappe::TileCell& c : cells) needed.insert(mappe::TileKey(c.z, c.x, c.y));

    // Recycle the Images of tiles that left the screen.
    for (auto it = _slots.begin(); it != _slots.end();) {
        if (needed.count(it->first) == 0) {
            it->second.img->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
            it->second.img->Source = nullptr;
            _freeImages.push_back(it->second.img);
            it = _slots.erase(it);
        } else {
            ++it;
        }
    }

    _wanted.clear();
    auto ready = [this](int z, int x, int y) { return _decoded.Contains(mappe::TileKey(z, x, y)); };

    for (const mappe::TileCell& c : cells) {
        std::int64_t key = mappe::TileKey(c.z, c.x, c.y);
        auto found = _slots.find(key);
        if (found == _slots.end()) {
            detail::Slot fresh;
            fresh.img = TakeImage();
            fresh.source = -1;
            fresh.depth = 0;
            _slots[key] = fresh;
            found = _slots.find(key);
        }
        detail::Slot& s = found->second;

        // Own tile if decoded; otherwise the nearest cached ancestor, scaled and clipped.
        std::int64_t src = -1;
        int depth = 0, sx = 0, sy = 0;
        if (_decoded.Contains(key)) {
            src = key;
        } else {
            RequestLoad(key);
            mappe::TileFallback fb;
            if (mappe::FindFallback(c.z, c.x, c.y, ready, MaxFallbackDepth, fb)) {
                src = mappe::TileKey(fb.z, fb.x, fb.y);
                depth = fb.depth;
                sx = fb.subX;
                sy = fb.subY;
            }
        }

        if (s.source != src || s.depth != depth) {
            BitmapImage^ bmp = nullptr;
            if (src >= 0 && !_decoded.TryGet(src, bmp)) bmp = nullptr;
            s.img->Source = bmp;
            s.source = (bmp != nullptr) ? src : -1;
            s.depth = depth;
        }

        if (s.source < 0) {
            s.img->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
            continue;
        }
        s.img->Visibility = Windows::UI::Xaml::Visibility::Visible;
        if (s.depth == 0) {
            Canvas::SetLeft(s.img, c.left);
            Canvas::SetTop(s.img, c.top);
            s.img->Width = c.width;
            s.img->Height = c.height;
            s.img->Clip = nullptr;
        } else {
            // The ancestor is 2^depth times bigger; shift it so the child sub-square lands on the cell.
            int factor = 1 << s.depth;
            Canvas::SetLeft(s.img, c.left - sx * c.width);
            Canvas::SetTop(s.img, c.top - sy * c.height);
            s.img->Width = c.width * factor;
            s.img->Height = c.height * factor;
            RectangleGeometry^ clip = ref new RectangleGeometry();
            clip->Rect = Rect(static_cast<float>(sx * c.width), static_cast<float>(sy * c.height),
                              static_cast<float>(c.width), static_cast<float>(c.height));
            s.img->Clip = clip;
        }
    }

    PlaceOverlays();
    PumpLoads();
}

void TileMapView::PlaceOverlays() {
    if (_vp.Width <= 0.0) return;

    if (_hasUser) {
        double sx, sy;
        _vp.ToScreen(_userLat, _userLon, sx, sy);
        double r = _userAcc / _vp.MetersPerPixel(_userLat);
        if (r < 6.0) r = 6.0;
        if (r > 5000.0) r = 5000.0;
        Canvas::SetLeft(_userAccuracy, sx - r);
        Canvas::SetTop(_userAccuracy, sy - r);
        _userAccuracy->Width = 2.0 * r;
        _userAccuracy->Height = 2.0 * r;
        Canvas::SetLeft(_userDot, sx - 9.0);
        Canvas::SetTop(_userDot, sy - 9.0);
        _userAccuracy->Visibility = Windows::UI::Xaml::Visibility::Visible;
        _userDot->Visibility = Windows::UI::Xaml::Visibility::Visible;
    } else {
        _userAccuracy->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        _userDot->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    }

    if (_hasPress) {
        double sx, sy;
        _vp.ToScreen(_pressLat, _pressLon, sx, sy);
        Canvas::SetLeft(_pressDot, sx - 6.0);
        Canvas::SetTop(_pressDot, sy - 6.0);
        _pressDot->Visibility = Windows::UI::Xaml::Visibility::Visible;
    } else {
        _pressDot->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    }

    if (_hasSearch) {
        double sx, sy;
        _vp.ToScreen(_searchLat, _searchLon, sx, sy);
        Canvas::SetLeft(_searchDot, sx - 7.0);
        Canvas::SetTop(_searchDot, sy - 7.0);
        Canvas::SetLeft(_searchLabel, sx - 120.0);
        Canvas::SetTop(_searchLabel, sy - 60.0);
        _searchDot->Visibility = Windows::UI::Xaml::Visibility::Visible;
        _searchLabel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    } else {
        _searchDot->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        _searchLabel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    }
}

// ---- Gestures ---------------------------------------------------------------

void TileMapView::OnManipulationStarted(Object^ sender, ManipulationStartedRoutedEventArgs^ e) {
    StopAnimation();
    _pinching = false;
    _pivotX = e->Position.X;
    _pivotY = e->Position.Y;
}

void TileMapView::OnManipulationDelta(Object^ sender, ManipulationDeltaRoutedEventArgs^ e) {
    Point p = e->Position;
    Windows::UI::Input::ManipulationDelta d = e->Delta;
    if (std::fabs(d.Scale - 1.0f) > 1e-5f) {
        _vp.ZoomAt(p.X, p.Y, _vp.Zoom + std::log(static_cast<double>(d.Scale)) / std::log(2.0));
        _pinching = true;
    }
    _vp.Pan(d.Translation.X, d.Translation.Y);
    _pivotX = p.X;
    _pivotY = p.Y;
    Relayout();
}

void TileMapView::OnManipulationCompleted(Object^ sender, ManipulationCompletedRoutedEventArgs^ e) {
    if (_pinching) {
        // Snap to the nearest whole level, keeping the pinch point fixed.
        StartAnimation(std::floor(_vp.Zoom + 0.5), _pivotX, _pivotY);
    }
    _pinching = false;
}

void TileMapView::OnDoubleTapped(Object^ sender, DoubleTappedRoutedEventArgs^ e) {
    Point p = e->GetPosition(this);
    StartAnimation(std::floor(_vp.Zoom) + 1.0, p.X, p.Y);
}

void TileMapView::OnHolding(Object^ sender, HoldingRoutedEventArgs^ e) {
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started) return;
    Point p = e->GetPosition(this);
    double lat, lon;
    _vp.ScreenToLatLon(p.X, p.Y, lat, lon);
    LatLonArgs^ args = ref new LatLonArgs();
    args->Lat = lat;
    args->Lon = lon;
    LongPressed(this, args);  // no handler: invoking an event with none is a no-op
}

void TileMapView::OnSizeChanged(Object^ sender, SizeChangedEventArgs^ e) {
    _vp.Width = e->NewSize.Width;
    _vp.Height = e->NewSize.Height;
    RectangleGeometry^ clip = ref new RectangleGeometry();
    clip->Rect = Rect(0.0f, 0.0f, static_cast<float>(_vp.Width), static_cast<float>(_vp.Height));
    this->Clip = clip;
    Relayout();
}

void TileMapView::StartAnimation(double zoom, double px, double py) {
    _animFrom = _vp.Zoom;
    _animTo = _vp.ClampZoom(zoom);
    _animPx = px;
    _animPy = py;
    _animStart = NowMs();
    _animating = true;
    _animTimer->Start();
}

void TileMapView::StopAnimation() {
    if (!_animating) return;
    _animating = false;
    _animTimer->Stop();
}

void TileMapView::OnAnimTick(Object^ sender, Object^ e) {
    double t = static_cast<double>(NowMs() - _animStart) / ZoomAnimMs;
    if (t > 1.0) t = 1.0;
    double eased = 1.0 - (1.0 - t) * (1.0 - t);
    _vp.ZoomAt(_animPx, _animPy, _animFrom + (_animTo - _animFrom) * eased);
    Relayout();
    if (t >= 1.0) StopAnimation();
}

// ---- Loading ----------------------------------------------------------------

void TileMapView::RequestLoad(std::int64_t key) {
    if (_decoded.Contains(key) || _inflight.count(key) > 0) return;
    auto r = _retryAfter.find(key);
    if (r != _retryAfter.end() && NowMs() < r->second) return;
    _wanted.push_back(key);
}

void TileMapView::PumpLoads() {
    while (_activeLoads < MaxParallelLoads && !_wanted.empty()) {
        std::int64_t key = _wanted.front();
        _wanted.erase(_wanted.begin());
        if (_decoded.Contains(key) || _inflight.count(key) > 0) continue;
        _inflight.insert(key);
        ++_activeLoads;
        ++_stats.requested;
        StartLoad(key);
    }
}

void TileMapView::StartLoad(std::int64_t key) {
    LoadTileAsync(key).then([this, key](task<void> t) {
        _inflight.erase(key);
        --_activeLoads;
        try {
            t.get();
            ++_stats.ok;
        } catch (Platform::Exception^ ex) {
            FailLoad(key, Describe(ex));
        } catch (...) {
            FailLoad(key, L"eccezione sconosciuta");
        }
        Relayout();
        PumpLoads();
    }, task_continuation_context::use_current());
}

void TileMapView::FailLoad(std::int64_t key, const std::wstring& msg) {
    ++_stats.failed;
    _stats.lastError = msg;
    int z, x, y;
    mappe::DecodeTileKey(key, z, x, y);
    _stats.lastTile = std::to_wstring(z) + L"/" + std::to_wstring(x) + L"/" + std::to_wstring(y);
    _retryAfter[key] = NowMs() + RetryDelayMs;
}

task<void> TileMapView::LoadTileAsync(std::int64_t key) {
    int z, x, y;
    mappe::DecodeTileKey(key, z, x, y);
    std::wstring name = Wide(mappe::TileFileName(z, x, y));
    std::string url = mappe::BuildTileUrl(TileTemplate, z, x, y);
    return ReadDiskAsync(name)
        .then([this, url, name](Array<unsigned char>^ disk) -> task<Array<unsigned char>^> {
            if (disk != nullptr) {
                ++_stats.diskHits;
                return task_from_result<Array<unsigned char>^>(disk);
            }
            return DownloadUrlAsync(url, name);
        }, task_continuation_context::use_current())
        .then([this](Array<unsigned char>^ bytes) -> task<BitmapImage^> {
            return DecodeAsync(bytes);
        }, task_continuation_context::use_current())
        .then([this, key](BitmapImage^ bmp) {
            _decoded.Put(key, bmp);
        }, task_continuation_context::use_current());
}

task<Array<unsigned char>^> TileMapView::ReadDiskAsync(const std::wstring& name) {
    String^ pname = Str(name);
    return _folderTask.then([pname](task<StorageFolder^> ft) {
        StorageFolder^ folder = nullptr;
        try {
            folder = ft.get();
        } catch (Platform::Exception^ ex) {
            ThrowAtStage(L"creazione cartella cache", ex);
        }
        return create_task(folder->GetFileAsync(pname));
    }).then([](task<StorageFile^> opened) -> task<Array<unsigned char>^> {
        StorageFile^ file = nullptr;
        try {
            file = opened.get();
        } catch (Platform::Exception^ ex) {
            if (ex->HResult == static_cast<int>(0x80070002)) {  // not cached yet
                return task_from_result<Array<unsigned char>^>(nullptr);
            }
            ThrowAtStage(L"apertura file cache", ex);
        }
        return create_task(FileIO::ReadBufferAsync(file)).then([](task<IBuffer^> rt) {
            IBuffer^ buf = nullptr;
            try {
                buf = rt.get();
            } catch (Platform::Exception^ ex) {
                ThrowAtStage(L"lettura file cache", ex);
            }
            return CopyBytes(buf);
        });
    });
}

task<Array<unsigned char>^> TileMapView::DownloadUrlAsync(const std::string& url, const std::wstring& diskName) {
    HttpClient^ client = ref new HttpClient();
    client->DefaultRequestHeaders->TryAppendWithoutValidation(L"User-Agent", ref new String(UserAgentW));
    Uri^ uri = ref new Uri(Str(Wide(url)));
    return create_task(client->GetAsync(uri))
        .then([this, client](task<HttpResponseMessage^> rt) -> task<IBuffer^> {
            HttpResponseMessage^ resp = nullptr;
            try {
                resp = rt.get();
            } catch (Platform::Exception^ ex) {
                ThrowAtStage(L"richiesta HTTP", ex);
            }
            if (!resp->IsSuccessStatusCode) {
                std::wstring reason = resp->ReasonPhrase != nullptr ? std::wstring(resp->ReasonPhrase->Data()) : std::wstring();
                _stats.lastHttp = L"OSM-FR" + std::to_wstring(static_cast<int>(resp->StatusCode)) + L" " + reason;
                throw Platform::Exception::CreateException(static_cast<int>(0x80004005), Str(L"[risposta HTTP] " + _stats.lastHttp));
            }
            return create_task(resp->Content->ReadAsBufferAsync());
        }, task_continuation_context::use_current())
        .then([this, diskName](task<IBuffer^> bt) -> Array<unsigned char>^ {
            IBuffer^ buf = nullptr;
            try {
                buf = bt.get();
            } catch (Platform::Exception^ ex) {
                ThrowAtStage(L"lettura corpo HTTP", ex);
            }
            Array<unsigned char>^ bytes = CopyBytes(buf);
            ++_stats.netFetches;
            if (!diskName.empty()) SaveToDisk(diskName, bytes);
            return bytes;
        }, task_continuation_context::use_current());
}

task<BitmapImage^> TileMapView::DecodeAsync(Array<unsigned char>^ bytes) {
    BitmapImage^ bmp = ref new BitmapImage();
    InMemoryRandomAccessStream^ stream = ref new InMemoryRandomAccessStream();
    DataWriter^ writer = ref new DataWriter(stream->GetOutputStreamAt(0));
    writer->WriteBytes(bytes);
    return create_task(writer->StoreAsync()).then([writer](task<unsigned int> st) {
        try {
            st.get();
        } catch (Platform::Exception^ ex) {
            ThrowAtStage(L"scrittura stream", ex);
        }
        return create_task(writer->FlushAsync());
    }, task_continuation_context::use_current()).then([writer, stream, bmp](task<bool> ft) {
        try {
            ft.get();
        } catch (Platform::Exception^ ex) {
            ThrowAtStage(L"flush stream", ex);
        }
        writer->DetachStream();
        stream->Seek(0);
        return create_task(bmp->SetSourceAsync(stream));
    }, task_continuation_context::use_current()).then([bmp](task<void> at) {
        try {
            at.get();
        } catch (Platform::Exception^ ex) {
            ThrowAtStage(L"decodifica (SetSourceAsync)", ex);
        }
        return bmp;
    }, task_continuation_context::use_current());
}

void TileMapView::SaveToDisk(const std::wstring& name, Array<unsigned char>^ bytes) {
    String^ pname = Str(name);
    _folderTask.then([pname](task<StorageFolder^> ft) {
        StorageFolder^ folder = nullptr;
        try {
            folder = ft.get();
        } catch (Platform::Exception^ ex) {
            ThrowAtStage(L"creazione cartella cache (scrittura)", ex);
        }
        return create_task(folder->CreateFileAsync(pname, CreationCollisionOption::ReplaceExisting));
    }).then([bytes](task<StorageFile^> ct) {
        StorageFile^ file = ct.get();
        return create_task(FileIO::WriteBytesAsync(file, bytes));
    }).then([this, bytes](task<void> t) {
        try {
            t.get();
        } catch (Platform::Exception^ ex) {
            // The cache is an optimisation: keep the tile and report the error in Diagnostica.
            _stats.lastDiskError = Describe(ex);
            return;
        }
        if (_diskBytes >= 0) _diskBytes += bytes->Length;
        if (_diskBytes < 0 || _diskBytes > static_cast<long long>(DiskBudget)) ScanAndEvict();
    }, task_continuation_context::use_current());
}

void TileMapView::StartProbe() {
    _probeLog = L"Test tile 6/33/22 (senza cache disco):\n";
    ProbeOne(L"HTTPS", "https://basemaps.cartocdn.com/rastertiles/voyager/6/33/22.png");
    ProbeOne(L"OSM-FR", "https://a.tile.openstreetmap.fr/osmfr/6/33/22.png");
}

void TileMapView::ProbeOne(const std::wstring& label, const std::string& url) {
    DownloadUrlAsync(url, std::wstring()).then([this, label](task<Array<unsigned char>^> dt) -> task<BitmapImage^> {
        try {
            Array<unsigned char>^ b = dt.get();
            _probeLog += label + L" download OK, " + std::to_wstring(b->Length) + L" byte\n";
            return DecodeAsync(b);
        } catch (Platform::Exception^ ex) {
            _probeLog += label + L" download FALLITO: " + Describe(ex) + L"\n";
            return task_from_result<BitmapImage^>(nullptr);
        }
    }, task_continuation_context::use_current()).then([this, label](task<BitmapImage^> bt) {
        try {
            BitmapImage^ bmp = bt.get();
            if (bmp != nullptr) _probeLog += label + L" decodifica OK\n";
        } catch (Platform::Exception^ ex) {
            _probeLog += label + L" decodifica FALLITA: " + Describe(ex) + L"\n";
        }
    }, task_continuation_context::use_current());
}

void TileMapView::ScanAndEvict() {
    if (_evicting) return;
    _evicting = true;
    _folderTask.then([](StorageFolder^ folder) {
        return create_task(folder->GetFilesAsync());
    }).then([](IVectorView<StorageFile^>^ files) -> task<std::vector<detail::DiskEntry>> {
        std::vector<task<detail::DiskEntry>> tasks;
        for each (StorageFile^ f in files) {
            tasks.push_back(create_task(f->GetBasicPropertiesAsync()).then([f](BasicProperties^ props) {
                detail::DiskEntry e;
                e.file = f;
                e.size = props->Size;
                e.modified = props->DateModified.UniversalTime;
                return e;
            }));
        }
        if (tasks.empty()) return task_from_result(std::vector<detail::DiskEntry>());
        return when_all(tasks.begin(), tasks.end());
    }).then([this](task<std::vector<detail::DiskEntry>> t) {
        _evicting = false;
        std::vector<detail::DiskEntry> entries;
        try {
            entries = t.get();
        } catch (...) {
            return;
        }
        std::sort(entries.begin(), entries.end(), [](const detail::DiskEntry& a, const detail::DiskEntry& b) {
            return a.modified < b.modified;  // oldest first
        });
        unsigned long long total = 0;
        for (const detail::DiskEntry& e : entries) total += e.size;
        size_t i = 0;
        while (total > DiskTarget && i < entries.size()) {
            total -= entries[i].size;
            create_task(entries[i].file->DeleteAsync());  // fire and forget
            ++i;
        }
        _diskBytes = static_cast<long long>(total);
    }, task_continuation_context::use_current());
}

}  // namespace MappeCpp
