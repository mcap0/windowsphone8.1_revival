#pragma once

#include "MainPage.g.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ts { class Tailnet; class TcpConn; }

namespace FotoPixel
{
    // LocalFolder\run.log (tools/phonelog 9dce49e3-42b3-43e8-9cd4-5bb5843d3b1c)
    void Log(const std::string& line);
    std::string Narrow(const std::wstring& w);

    // One photo of the Camera Roll; High is its __highres.jpg twin when there is one (that is what gets sent).
    public ref class Photo sealed
    {
    public:
        property Platform::String^ Name;
        property Windows::Storage::StorageFile^ File;
        property Windows::Storage::StorageFile^ High;
        property Windows::UI::Xaml::Media::ImageSource^ Thumb;
        property Platform::String^ Sent;        // file name on the Pixel, null = not sent
        property Platform::String^ Progress;    // text over the thumbnail while it is queued/sending
        property bool ThumbAsked;
        property long long Created;
    };

    public ref class MainPage sealed
    {
    public:
        MainPage();
        bool HandleBack();
        void Resume();     // after a suspend: the Tailscale sessions went stale

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        void OnSend(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnClear(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnRefresh(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTailscale(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnSelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
        void OnContainerContentChanging(Windows::UI::Xaml::Controls::ListViewBase^ sender,
            Windows::UI::Xaml::Controls::ContainerContentChangingEventArgs^ e);

        void Load();                         // worker: Camera Roll + sent registry + Pixel lookup
        void Upload(Windows::Foundation::Collections::IVector<Photo^>^ queue);   // worker
        Platform::String^ SendOne(Photo^ p, int index, int total);              // worker: Pixel file name
        bool FindPixel();                    // worker
        bool Ping(const std::wstring& host);
        std::wstring Broadcast();

        struct Resp { int status; Windows::Data::Json::JsonObject^ json; };
        // LAN: HttpClient to host_; Tailscale: HTTP/1.1 by hand over a TsNet TCP connection to pixel-foto
        Resp Call(Windows::Web::Http::HttpMethod^ method, Platform::String^ path,
            Windows::Storage::Streams::IBuffer^ body, bool json, int timeoutMs);
        Resp TsCall(const std::string& method, const std::string& path, Windows::Storage::Streams::IBuffer^ body,
            bool json, int timeoutMs);
        void StartTailnet();
        bool TsPing();
        std::wstring TsText();

        void Ui(std::function<void()> fn);
        void SetStatus(Platform::String^ text);
        void Paint(Photo^ p);
        void Paint(Windows::UI::Xaml::Controls::ContentControl^ c, Photo^ p);
        void UpdateButtons();

        Platform::Collections::Vector<Platform::Object^>^ items_;
        Windows::Web::Http::HttpClient^ client_;
        Windows::System::Display::DisplayRequest^ display_;
        std::mutex mu_;
        std::wstring host_;
        std::map<std::wstring, std::wstring> sent_;
        bool busy_, loading_;
        bool viaTs_;                              // the Pixel is reached through Tailscale (not on its Wi-Fi)
        std::shared_ptr<ts::Tailnet> tailnet_;
        std::shared_ptr<ts::TcpConn> tsConn_;     // kept open between requests
        std::mutex tsMu_;
        std::string tsAuthUrl_;
    };
}
