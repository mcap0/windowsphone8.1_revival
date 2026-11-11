#pragma once

#include "ChatPage.g.h"
#include "ViewModels.h"

#include <collection.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Telegram
{
    class ChatListener;

    public ref class ChatPage sealed
    {
    public:
        ChatPage();
        bool HandleBack();
        void ContinueFileOpenPicker(Windows::ApplicationModel::Activation::FileOpenPickerContinuationEventArgs^ args);

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    internal:
        Platform::Collections::Vector<MsgVM^>^ messages_;
        std::vector<tg::MsgItem> items_;
        std::string chatKey_;
        int32_t threadId_;                          // topic or comment thread shown (0: the whole chat)
        std::map<int32_t, tg::MsgItem> quoted_;     // quoted messages fetched because they were not loaded
        std::set<int32_t> quoteAsked_;
        ChatListener* listener_;
        Windows::UI::Xaml::Controls::ScrollViewer^ scrollViewer_;
        Windows::Media::Capture::MediaCapture^ capture_;
        Windows::Storage::StorageFile^ recordingFile_;
        Windows::UI::Xaml::DispatcherTimer^ recordingTimer_;
        Windows::Foundation::DateTime recordingStarted_;
        int32_t replyTo_;
        bool stick_;
        std::wstring replyPreview_;
        int32_t lastReadId_;
        bool active_;
        bool loading_;
        bool recording_;
        bool audioPlaying_;
        MsgVM^ playingVM_;
        std::vector<std::wstring> attachmentPaths_;
        size_t attachmentIndex_;
        unsigned int attachmentPreviewGeneration_;

        void LoadMessages(int32_t beforeId, bool older);
        void LoadOlderIfNeeded();
        void AddOrUpdateMessage(const tg::MsgItem& message, bool scrollToBottom);
        void UpdateMessageAt(size_t index);
        bool InThread(const tg::MsgItem& message) const;
        Platform::String^ QuoteLine(const tg::MsgItem& message);
        void RefreshQuotes();
        void FetchQuotes();
        void ApplyReactions(int32_t id, const std::vector<tg::Reaction>& reactions);
        void React(MsgVM^ vm, const std::string& emoji);
        void ShowReactionPicker(MsgVM^ vm, Windows::UI::Xaml::FrameworkElement^ at);
        void Reaction_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void Comments_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void ScrollToBottom();
        void SaveToDownloads(MsgVM^ vm);
        void File_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void SendFileAtPath(const std::wstring& path);
        void SendPhotoBytes(const std::string& bytes);
        void SendPhotoStream(Windows::Storage::Streams::IRandomAccessStream^ input);
        void MarkRead();
        void SetReply(int32_t id);
        void ClearReply();
        void Message_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void Message_Holding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void Page_Loaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void MessageList_Loaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Send_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Photo_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Voice_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Mute_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void CancelReply_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void StopRecording_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void CancelRecording_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void AudioPlayer_MediaEnded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);

        void LoadPhotoPreview(MsgVM^ vm);
        void OpenPhoto(MsgVM^ vm);
        void PlayMedia(MsgVM^ vm);
        void OpenDocumentOrVideo(MsgVM^ vm);
        void OpenLocation(MsgVM^ vm);
        void StartRecording();
        void StopRecording(bool send);
        void SendRecordedFile(Windows::Storage::StorageFile^ file);
        void SendPickedPhoto(Windows::Storage::StorageFile^ file);
        void ShowAttachmentPreview(const std::vector<std::wstring>& paths);
        void PreviewPrevious_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void PreviewNext_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void PreviewCancel_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void PreviewSend_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void UpdateAttachmentPreview();
        void ShowError(Platform::String^ message);
    };
}
