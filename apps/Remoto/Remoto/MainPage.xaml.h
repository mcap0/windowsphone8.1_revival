#pragma once

#include "MainPage.g.h"

namespace Remoto
{
    struct Link; // the connection and its thread (MainPage.xaml.cpp)

    public ref class MainPage sealed
    {
    public:
        MainPage();
        void Suspend();
        void Resume();

    internal:
        void ShowStatus(const std::wstring& text, bool timings);
        void ShowApps(Platform::String^ json);
        void ShowPin(bool on);
        void ShowOpen(Platform::String^ json);
        void SendRotation();
        void OnField(Platform::String^ json);
        void OnCopied(Platform::String^ text);
        void OnMicWanted(bool on);

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        void Connect();
        void StartInput();
        void Touch(int action, Windows::UI::Core::PointerEventArgs^ args);
        void OnBack(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);
        void Apps_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OpenApps_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void CloseAll_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ShowOpenPanel(bool show);
        void Orient();
        void Paste_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void CopyBar_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void StartMic();
        void Keyboard_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Stats_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Reconnect_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Server_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Pair_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Restart_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void AppItem_Holding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void Filter();
        void ShowAppList(bool show);
        void OpenApp(const std::wstring& package, bool restart);
        int PackageOf(Platform::String^ label);
        void AppList_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void UpdateServerButton();
        void OpenMenu();
        void TsOnly_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Punch_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void CloseMenu();
        void PinKey_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ShowPinText();
        void MenuPanel_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void MenuBox_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);

        // VS2013 ref classes take no in-class initializers: all set in the constructor
        Link* m_link;
        Windows::UI::Core::CoreIndependentInputSource^ m_input;
        Windows::Foundation::IAsyncAction^ m_inputLoop;
        Platform::Collections::Vector<Platform::String^>^ m_appLabels;
        std::vector<std::wstring> m_appPackages;     // shown items (filtered), same order as m_appLabels
        std::vector<std::wstring> m_allLabels, m_allPackages;
        bool m_showTimings;
        bool m_typing;
        bool m_appsAsked;
        std::atomic<bool> m_listOpen;      // touches are not sent to the Pixel while our list or menu is up
        std::atomic<bool> m_menuOpen;
        std::atomic<bool> m_openOpen;      // the open apps panel
        std::atomic<int> m_rotation;       // how the Lumia is held, as Android's Surface.ROTATION_* ('W')
        std::atomic<bool> m_pinOpen;       // the Pixel's lock prompt: our keypad is up, touches stay here
        std::wstring m_pin;                // digits typed so far (never stored or logged)
        int m_pinTries;
        std::atomic<long long> m_lastVideo; // QPC us of the last video frame: an empty display shows the list
        float m_panelWidth, m_panelHeight; // DIPs, read by the input thread
        float m_startY[10];                // where each pointer went down (stream pixels): the edge swipe
        bool m_edge[10];                   // this pointer became the menu swipe: the Pixel got a cancel
        int m_pointerIds[10];              // Windows pointer id per slot (-1 = free): slot = id sent to the Pixel
        int m_fieldSeq;                    // 'I' messages: a field closing waits a moment for the next one opening
        bool m_fieldOpen;                  // a Pixel field has the Lumia keyboard up
        Platform::String^ m_copied;        // last text copied on the Pixel
        int m_copySeq;
        std::wstring m_micId;              // the Lumia microphone that records (found at the first use)
        bool m_micWanted;
    };
}
