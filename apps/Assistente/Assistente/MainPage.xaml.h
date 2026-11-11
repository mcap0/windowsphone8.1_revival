// MainPage: listen -> command network (Core/Wsp) -> do the action, or a Google search -> answer (system Italian voice).
#pragma once

#include <string>
#include <vector>

#include "MainPage.g.h"
#include "Call.h"

namespace assist { struct Command; }

namespace Assistente {

public ref class MainPage sealed {
public:
    MainPage();
    void Listen(Platform::String^ reason);
internal:
    // set when the call screen was shown: coming back from the call must not start listening
    static bool NoListenOnResume;

public:

protected:
    virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

private:
    void Info_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
    void Root_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
    void Send_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Send_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
    void Send_PointerPressed(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
    void Send_PointerReleased(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
    void Tilt(bool down);
    void Box_KeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
    void Box_GotFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Box_LostFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Box_TextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
    void ShowCallChoices();
    void Examples_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);

    void FindMicrophone(std::function<void()> then);
    void StartCapture();
    void WhenVoskReady(std::function<void()> then);
    void SendShot();
    void OnHeard(const std::vector<int16_t>& pcm, bool heard, const std::wstring& error);
    void TryOffline(const std::wstring& heard, const std::string& wav, const std::wstring& typed);
    void RunOffline(const assist::Command& cmd, const std::wstring& text, const std::wstring& detail);
    void Answer(const std::wstring& say, const wchar_t* state, const std::wstring& detail);
    concurrency::task<Windows::Data::Json::JsonObject^> RunInfo(const ToolCall& call);
    void Fail(const wchar_t* where, Platform::Exception^ ex);
    concurrency::task<std::wstring> RunCall(const ToolCall& call);
    void Speak(const std::wstring& text);
    void SetState(const std::wstring& text);
    int CountToday(const wchar_t* what, bool increment);
    // recording mode for the hand-written recognizer (tools/wsr): "registra [nome]" in the box
    void StartRecording(const std::wstring& speaker);
    void RecordNext();
    void RecordSaved(const std::vector<int16_t>& pcm, bool heard, const std::wstring& error);
    void StopRecordingMode(const std::wstring& why);
    void StartLongRecording(const std::wstring& speaker);
    void StartTextRecording(const std::wstring& typed, const std::wstring& kind);
    void SaveTake(const std::wstring& file, const std::wstring& say, const std::vector<int16_t>& pcm);

    bool _busy;
    bool _listening;
    bool _discard;  // listening was interrupted to type: drop the audio
    std::wstring _micId;
    bool _micFound;  // _micId is known (this run or LocalSettings)
    ULONGLONG _lastTurn;
    bool _silent;  // the answer is an app opening: do not speak
    Windows::Media::SpeechSynthesis::SpeechSynthesizer^ _synth;
    bool _vosk;           // this listen goes to Vosk (free speech-to-text), its final text to the command network
    bool _rec;            // recording mode: prompts read aloud are saved as WAV
    bool _recStop;        // the arrow was pressed: end the mode, drop the current take
    size_t _recIndex;
    int _recRetries;
    int _recSaved;
    std::wstring _recSession;  // folder name: date-time[-speaker]
    std::vector<std::pair<std::wstring, std::wstring>> _recList;  // shown, transcript
    bool _recText;             // "registra testo|comandi": a list of lines from Assets, one line per take
    std::wstring _recKind;     // "testo" or "comandi": the Assets file and the session name
    bool _recLong;             // "registra lungo": one continuous capture saved in 30 s pieces
    int _recPieces;
    Windows::System::Display::DisplayRequest^ _keepOn;  // the screen must not go off (the app would be suspended)
};

}
