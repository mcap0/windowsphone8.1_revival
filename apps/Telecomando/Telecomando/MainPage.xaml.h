#pragma once

#include "MainPage.g.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace Telecomando
{
    // The TV connection, on its own thread: connects when the app is on screen, sends the queued keys, keeps the
    // link alive (answers pings) and reconnects when it drops. The page only queues keys and shows the state.
    // LocalFolder\run.log (and the debugger): every state, error and crash of the app
    void Log(const std::string& line);

    class Link
    {
    public:
        typedef std::function<void(const std::string& state, bool connected)> StateFn;
        explicit Link(StateFn onState);
        ~Link();
        void Configure(const std::string& host, const std::string& token);
        void Key(const std::string& key);
        void Wake(const std::string& mac);
        void Start();      // app on screen
        void Pause();      // app in the background: close the link
        void Reconnect();
        std::string Token();
    private:
        void Run();
        void State(const std::string& state, bool connected);   // to the page only when it changes
        StateFn onState_;
        std::string shown_;
        bool shownConnected_;
        int64_t wakeUntil_;   // after a Wake-on-LAN: probe the TV every second while it boots
        std::mutex mu_;
        std::condition_variable cv_;
        std::deque<std::string> keys_;
        std::string host_, token_;
        bool active_, stop_, reconnect_, pause_;
        int64_t retryAt_;
        void* remote_;     // tv::Remote, kept out of this header (it pulls in Winsock)
        std::thread thread_;
    };

    public ref class MainPage sealed
    {
    public:
        MainPage();
        bool HandleBack();
        void Suspend();
        void Resume();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        Link* link_;
        bool playing_;
        ULONGLONG lastBuzz_;
        void Send(const std::string& key);
        void Buzz();
        void StopBuzz();
        Windows::UI::Xaml::DispatcherTimer^ buzzStop_;   // the motor is stopped by us, not left to the driver
        void ShowState(const std::string& state, bool connected);
        void Key_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Power_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Play_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Numbers_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Settings_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Wake_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Reconnect_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        bool connected_;
    };
}
