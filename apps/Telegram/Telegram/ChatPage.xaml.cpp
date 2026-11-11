#include "pch.h"
#include <cstdlib>
#include "ChatPage.xaml.h"
#include "NewChatPage.xaml.h"
#include "ImagePage.xaml.h"
#include "FilePage.xaml.h"
#include "TgService.h"
#include "voice.h"
#include "AudioDecode.h"

#include <windows.graphics.imaging.h>
#include <windows.storage.h>
#include <windows.storage.pickers.h>
#include <windows.storage.streams.h>
#include <windows.system.h>
#include <windows.ui.xaml.media.h>

using namespace Telegram;
using namespace Platform;
using namespace concurrency;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Capture;
using namespace Windows::Media::MediaProperties;
using namespace Windows::System;
using namespace Windows::UI::Xaml::Media::Imaging;
using Windows::Foundation::Uri;
using Windows::Foundation::EventHandler;

namespace
{
    Windows::Foundation::DateTime NowDT()
    {
        auto c = ref new Windows::Globalization::Calendar();
        c->SetToNow();
        return c->GetDateTime();
    }
}

namespace
{
    DependencyObject^ FindScrollViewer(DependencyObject^ root)
    {
        if (dynamic_cast<ScrollViewer^>(root) != nullptr)
            return root;
        int count = VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < count; ++i)
        {
            DependencyObject^ child = FindScrollViewer(VisualTreeHelper::GetChild(root, i));
            if (child != nullptr)
                return child;
        }
        return nullptr;
    }

    Array<unsigned char>^ ToArray(const std::string& bytes)
    {
        auto result = ref new Array<unsigned char>(static_cast<unsigned int>(bytes.size()));
        for (unsigned int i = 0; i < result->Length; ++i)
            result[i] = static_cast<unsigned char>(bytes[i]);
        return result;
    }

    std::string FromArray(Array<unsigned char>^ bytes)
    {
        std::string result;
        result.resize(bytes->Length);
        for (unsigned int i = 0; i < bytes->Length; ++i)
            result[i] = static_cast<char>(bytes[i]);
        return result;
    }

    String^ DisplayFileName(const std::string& name)
    {
        std::wstring value = Telegram::Utf16(name);
        return ref new String(value.empty() ? L"file.bin" : value.c_str());
    }
}

namespace Telegram
{
    class ChatListener : public tg::IClientListener
    {
    public:
        explicit ChatListener(ChatPage^ page) : page_(page) {}
        virtual void onNewMessage(const tg::MsgItem& message)
        {
            if (page_ != nullptr)
                page_->AddOrUpdateMessage(message, true);
        }
        virtual void onMessageEdited(const tg::MsgItem& message)
        {
            if (page_ != nullptr)
                page_->AddOrUpdateMessage(message, false);
        }
        virtual void onMessagesDeleted(const std::string& key, const std::vector<int32_t>& ids)
        {
            if (page_ == nullptr)
                return;
            std::string current = page_->chatKey_;
            if (key != current && !(key.empty() && !current.empty() && current[0] != 'h'))
                return;

            for (size_t n = 0; n < ids.size(); ++n)
            {
                for (size_t i = 0; i < page_->items_.size(); ++i)
                {
                    if (page_->items_[i].id == ids[n])
                    {
                        page_->items_.erase(page_->items_.begin() + i);
                        page_->messages_->RemoveAt(static_cast<unsigned int>(i));
                        break;
                    }
                }
            }
        }
        virtual void onMessageReactions(const std::string& key, int32_t id, const std::vector<tg::Reaction>& reactions)
        {
            if (page_ != nullptr && key == page_->chatKey_)
                page_->ApplyReactions(id, reactions);
        }
        virtual void onReadOutbox(const std::string& key, int32_t maxId)
        {
            if (page_ == nullptr || key != page_->chatKey_)
                return;
            const tg::ChatItem* chat = TgService::Get().Client().chat(page_->chatKey_);
            if (chat == nullptr)
                return;

            tg::ChatItem copy = *chat;
            if (maxId > copy.readOutboxMax)
                copy.readOutboxMax = maxId;
            for (size_t i = 0; i < page_->items_.size(); ++i)
                page_->messages_->GetAt(static_cast<unsigned int>(i))->Update(page_->items_[i], &copy);
        }
    private:
        ChatPage^ page_;
    };
}

ChatPage::ChatPage() : messages_(ref new Platform::Collections::Vector<MsgVM^>()),
    threadId_(0), listener_(nullptr), scrollViewer_(nullptr), replyTo_(0), stick_(true), lastReadId_(0),
    active_(false), loading_(false), recording_(false), audioPlaying_(false), playingVM_(nullptr),
    attachmentIndex_(0), attachmentPreviewGeneration_(0)
{
    auto pane = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
    pane->Showing += ref new Windows::Foundation::TypedEventHandler<Windows::UI::ViewManagement::InputPane^,
        Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>([this](Windows::UI::ViewManagement::InputPane^,
            Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e)
    {
        e->EnsuredFocusedElementInView = true;
        Root->Margin = Thickness(0, 0, 0, e->OccludedRect.Height);
        ScrollToBottom();
    });
    pane->Hiding += ref new Windows::Foundation::TypedEventHandler<Windows::UI::ViewManagement::InputPane^,
        Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>([this](Windows::UI::ViewManagement::InputPane^,
            Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e)
    {
        e->EnsuredFocusedElementInView = true;
        Root->Margin = Thickness(0);
    });

    InitializeComponent();
    MessageList->ItemsSource = messages_;
}

void ChatPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    active_ = true;
    // "key", or "key\nthread\ntitle" for a forum topic / the comments of a channel post
    std::string param = FromP(dynamic_cast<String^>(e->Parameter));
    std::string threadTitle;
    threadId_ = 0;
    size_t nl = param.find('\n');
    if (nl != std::string::npos)
    {
        size_t nl2 = param.find('\n', nl + 1);
        threadId_ = std::atoi(param.substr(nl + 1, nl2 == std::string::npos ? std::string::npos : nl2 - nl - 1).c_str());
        if (nl2 != std::string::npos) threadTitle = param.substr(nl2 + 1);
        param = param.substr(0, nl);
    }
    chatKey_ = param;
    TgService::Get().openChat = chatKey_;
    TgService::Get().Client().setSendThread(threadId_);
    quoted_.clear();
    quoteAsked_.clear();
    items_.clear();
    messages_->Clear();
    replyTo_ = 0;
    replyPreview_.clear();
    ReplyBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;

    listener_ = new ChatListener(this);
    TgService::Get().AddListener(listener_);

    std::vector<std::wstring> picked = TgService::Get().pendingAttachments;
    TgService::Get().pendingAttachments.clear();
    if (!TgService::Get().pendingAttach.empty())
    {
        std::wstring path;
        path.swap(TgService::Get().pendingAttach);
        picked.push_back(path);
    }
    if (!picked.empty()) ShowAttachmentPreview(picked);

    const tg::ChatItem* chat = TgService::Get().Client().chat(chatKey_);
    ChatTitle->Text = chat == nullptr ? PStr(TgService::Get().Client().titleOf(chatKey_)) : PStr(chat->title);
    if (!threadTitle.empty()) ChatTitle->Text = PStr(threadTitle);
    if (chat != nullptr && chat->muted)
    {
        MuteButton->Label = L"riattiva";
        MuteButton->Icon = ref new SymbolIcon(Symbol::Volume);
    }
    else
    {
        MuteButton->Label = L"silenzia";
        MuteButton->Icon = ref new SymbolIcon(Symbol::Mute);
    }

    lastReadId_ = 0;
    LoadMessages(0, false);
}

void ChatPage::OnNavigatedFrom(NavigationEventArgs^)
{
    active_ = false;
    TgService::Get().openChat.clear();
    TgService::Get().Client().setSendThread(0);
    if (listener_ != nullptr)
    {
        TgService::Get().RemoveListener(listener_);
        delete listener_;
        listener_ = nullptr;
    }
    if (recording_)
        StopRecording(false);
    if (AudioPlayer != nullptr)
        AudioPlayer->Stop();
}

bool ChatPage::HandleBack()
{
    if (AttachmentPreview->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        PreviewCancel_Click(nullptr, nullptr);
        return true;
    }
    if (recording_)
    {
        StopRecording(false);
        return true;
    }
    if (ReplyBar->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        ClearReply();
        return true;
    }
    if (audioPlaying_)
    {
        AudioPlayer->Stop();
        audioPlaying_ = false;
        if (playingVM_ != nullptr)
            playingVM_->MediaLabel = L"\u25B6";
        return false;
    }
    return false;
}

void ChatPage::Page_Loaded(Object^, RoutedEventArgs^)
{
    LoadOlderIfNeeded();
}

void ChatPage::MessageList_Loaded(Object^, RoutedEventArgs^)
{
    if (scrollViewer_ != nullptr)
        return;
    scrollViewer_ = dynamic_cast<ScrollViewer^>(FindScrollViewer(MessageList));
    if (scrollViewer_ != nullptr)
    {
        scrollViewer_->ViewChanged += ref new Windows::Foundation::EventHandler<ScrollViewerViewChangedEventArgs^>(
            [this](Object^, ScrollViewerViewChangedEventArgs^)
        {
            stick_ = scrollViewer_->VerticalOffset >= scrollViewer_->ScrollableHeight - 120;
            LoadOlderIfNeeded();
        });
    }
}

void ChatPage::LoadMessages(int32_t beforeId, bool older)
{
    if (!active_ || loading_)
        return;
    loading_ = true;

    auto onPage = [this, older](const std::vector<tg::MsgItem>& page, const tg::RpcErr&)
    {
        if (!active_)
        {
            loading_ = false;
            return;
        }

        if (older)
        {
            for (size_t i = 0; i < page.size(); ++i)
                AddOrUpdateMessage(page[i], false);
        }
        else
        {
            for (size_t i = 0; i < page.size(); ++i)
                AddOrUpdateMessage(page[i], false);
            ScrollToBottom();
            MarkRead();
        }
        loading_ = false;
        FetchQuotes();
    };
    if (threadId_) TgService::Get().Client().loadThread(chatKey_, threadId_, beforeId, 30, onPage);
    else TgService::Get().Client().loadHistory(chatKey_, beforeId, 30, onPage);
}

bool ChatPage::InThread(const tg::MsgItem& message) const
{
    if (!threadId_) return true;
    if (threadId_ == 1) return message.threadId <= 1;   // General
    return message.threadId == threadId_ || message.replyTo == threadId_ || message.id == threadId_;
}

// "Author: text" of the message this one quotes. Looked up among the loaded messages, then among the ones
// fetched for this; missing ones are fetched (FetchQuotes) and the line filled in when they come.
String^ ChatPage::QuoteLine(const tg::MsgItem& message)
{
    if (!message.replyTo || message.replyTo == threadId_) return L"";
    const tg::MsgItem* src = nullptr;
    for (size_t i = 0; i < items_.size() && src == nullptr; ++i)
        if (items_[i].id == message.replyTo) src = &items_[i];
    if (src == nullptr)
    {
        std::map<int32_t, tg::MsgItem>::const_iterator q = quoted_.find(message.replyTo);
        if (q != quoted_.end()) src = &q->second;
    }
    std::string author = src != nullptr ? (src->out ? std::string("Tu") : src->fromName) : message.replyFrom;
    std::string text = !message.replyText.empty() ? message.replyText : src != nullptr ? tg::Client::previewText(*src) : "";
    if (author.empty() && text.empty()) text = "\xE2\x80\xA6";   // ... until it is fetched
    std::wstring line = Utf16(author.empty() ? text : author + ": " + text);
    if (line.size() > 140) line = line.substr(0, 140) + L"\u2026";
    return ref new String(line.c_str());
}

void ChatPage::RefreshQuotes()
{
    for (size_t i = 0; i < items_.size() && i < messages_->Size; ++i)
        if (items_[i].replyTo)
            messages_->GetAt(static_cast<unsigned int>(i))->Reply = QuoteLine(items_[i]);
}

void ChatPage::FetchQuotes()
{
    std::vector<int32_t> ids;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        int32_t id = items_[i].replyTo;
        if (!id || id == threadId_ || quoted_.count(id) || quoteAsked_.count(id)) continue;
        bool loaded = false;
        for (size_t j = 0; j < items_.size() && !loaded; ++j) loaded = items_[j].id == id;
        if (loaded) continue;
        quoteAsked_.insert(id);
        ids.push_back(id);
    }
    if (ids.empty()) return;
    std::string key = chatKey_;
    TgService::Get().Client().getMessages(chatKey_, ids, [this, key](const std::vector<tg::MsgItem>& found, const tg::RpcErr&)
    {
        if (!active_ || key != chatKey_) return;
        for (size_t i = 0; i < found.size(); ++i) quoted_[found[i].id] = found[i];
        RefreshQuotes();
    });
}

void ChatPage::ApplyReactions(int32_t id, const std::vector<tg::Reaction>& reactions)
{
    for (size_t i = 0; i < items_.size() && i < messages_->Size; ++i)
    {
        if (items_[i].id != id) continue;
        items_[i].reactions = reactions;
        messages_->GetAt(static_cast<unsigned int>(i))->SetReactions(reactions);
        return;
    }
}

// Adds my reaction, or takes it away when it is the one I already have.
void ChatPage::React(MsgVM^ vm, const std::string& emoji)
{
    std::string mine;
    for (unsigned int i = 0; i < vm->Reactions->Size; ++i)
        if (vm->Reactions->GetAt(i)->mine) mine = vm->Reactions->GetAt(i)->emoji;
    std::string want = mine == emoji ? std::string() : emoji;
    TgService::Get().Client().sendReaction(chatKey_, vm->Id, want, [this](const tg::RpcErr& e)
    {
        if (!e.ok()) ShowError(PStr(e.message == "REACTION_INVALID" ? "reazione non permessa in questa chat" : e.message));
    });
}

void ChatPage::ShowReactionPicker(MsgVM^ vm, FrameworkElement^ at)
{
    static const char* emojis[] = { "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\xAE",
                                    "\xF0\x9F\x98\xA2", "\xF0\x9F\x99\x8F", "\xF0\x9F\x94\xA5", "\xF0\x9F\x91\x8E" };
    auto flyout = ref new Flyout();
    auto row = ref new StackPanel();
    row->Orientation = Orientation::Horizontal;
    for (size_t i = 0; i < sizeof(emojis) / sizeof(emojis[0]); ++i)
    {
        std::string e = emojis[i];
        auto b = ref new Button();
        b->Content = PStr(e);
        b->FontSize = 22;
        b->MinWidth = 0;
        b->Padding = Thickness(6, 2, 6, 2);
        b->BorderThickness = Thickness(0);
        b->Click += ref new RoutedEventHandler([this, vm, e, flyout](Object^, RoutedEventArgs^)
        {
            flyout->Hide();
            React(vm, e);
        });
        row->Children->Append(b);
    }
    flyout->Content = row;
    flyout->ShowAt(at);
}

void ChatPage::Reaction_Tapped(Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;   // not the bubble's tap (photo, voice...)
    FrameworkElement^ el = dynamic_cast<FrameworkElement^>(sender);
    ReactionVM^ r = el == nullptr ? nullptr : dynamic_cast<ReactionVM^>(el->DataContext);
    if (r == nullptr) return;
    for (unsigned int i = 0; i < messages_->Size; ++i)
        if (messages_->GetAt(i)->Id == r->msgId) { React(messages_->GetAt(i), r->emoji); return; }
}

// "N commenti" under a channel post: its discussion, in the linked group, opens as a thread.
void ChatPage::Comments_Tapped(Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    FrameworkElement^ el = dynamic_cast<FrameworkElement^>(sender);
    MsgVM^ vm = el == nullptr ? nullptr : dynamic_cast<MsgVM^>(el->DataContext);
    if (vm == nullptr) return;
    TgService::Get().Client().openComments(chatKey_, vm->Id,
        [this](const std::string& key, int32_t thread, const tg::RpcErr& err)
    {
        if (!active_) return;
        if (!err.ok() || key.empty()) { ShowError(PStr("commenti non disponibili: " + err.message)); return; }
        this->Frame->Navigate(ChatPage::typeid, PStr(key + "\n" + std::to_string(static_cast<long long>(thread)) + "\nCommenti"));
    });
}

void ChatPage::AddOrUpdateMessage(const tg::MsgItem& message, bool scrollToBottom)
{
    if (!active_ || message.chatKey != chatKey_ || !InThread(message))
        return;

    bool nearBottom = true;
    if (scrollViewer_ != nullptr)
        nearBottom = scrollViewer_->ScrollableHeight - scrollViewer_->VerticalOffset < 100.0;

    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].id == message.id)
        {
            items_[i] = message;
            UpdateMessageAt(i);
            if (!message.out)
                MarkRead();
            return;
        }
    }

    size_t index = items_.size();
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (message.date < items_[i].date ||
            (message.date == items_[i].date && message.id < items_[i].id))
        {
            index = i;
            break;
        }
    }
    items_.insert(items_.begin() + index, message);
    const tg::ChatItem* chat = TgService::Get().Client().chat(chatKey_);
    messages_->InsertAt(static_cast<unsigned int>(index), MsgVM::From(message, chat));
    if (message.replyTo) messages_->GetAt(static_cast<unsigned int>(index))->Reply = QuoteLine(message);
    if (scrollToBottom && message.replyTo) FetchQuotes();

    if (!message.thumbKey.empty() && (message.kind == tg::MsgItem::Photo ||
        message.kind == tg::MsgItem::Video || message.kind == tg::MsgItem::Sticker))
        LoadPhotoPreview(messages_->GetAt(static_cast<unsigned int>(index)));

    if (!message.out)
        MarkRead();
    if (scrollToBottom && nearBottom)
        ScrollToBottom();
}

void ChatPage::UpdateMessageAt(size_t index)
{
    if (index >= items_.size() || index >= messages_->Size)
        return;
    const tg::ChatItem* chat = TgService::Get().Client().chat(chatKey_);
    messages_->GetAt(static_cast<unsigned int>(index))->Update(items_[index], chat);
    messages_->GetAt(static_cast<unsigned int>(index))->Reply = QuoteLine(items_[index]);
    if (!items_[index].thumbKey.empty() && items_[index].kind == tg::MsgItem::Photo)
        LoadPhotoPreview(messages_->GetAt(static_cast<unsigned int>(index)));
}

void ChatPage::LoadOlderIfNeeded()
{
    if (!active_ || loading_ || items_.empty() || scrollViewer_ == nullptr)
        return;
    if (scrollViewer_->VerticalOffset <= 1.0)
        LoadMessages(items_.front().id, true);
}

void ChatPage::ScrollToBottom()
{
    if (MessageList->Items->Size == 0)
        return;
    MessageList->ScrollIntoView(MessageList->Items->GetAt(MessageList->Items->Size - 1));
    stick_ = true;
    if (scrollViewer_ != nullptr)
    {
        // item heights are only known after layout (photos, wrapped text): scroll again once measured
        MessageList->UpdateLayout();
        scrollViewer_->ChangeView(nullptr, scrollViewer_->ScrollableHeight, nullptr, true);
    }
}

void ChatPage::MarkRead()
{
    if (!active_ || items_.empty())
        return;
    int32_t id = items_.back().id;
    if (id > lastReadId_)
    {
        lastReadId_ = id;
        if (threadId_) TgService::Get().Client().markThreadRead(chatKey_, threadId_, id);
        else TgService::Get().Client().markRead(chatKey_, id);
    }
}

void ChatPage::SetReply(int32_t id)
{
    replyTo_ = id;
    replyPreview_.clear();
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].id == id)
        {
            std::string text = tg::Client::previewText(items_[i]);
            replyPreview_ = Utf16(text);
            if (replyPreview_.size() > 100)
                replyPreview_ = replyPreview_.substr(0, 100) + L"\u2026";
            break;
        }
    }
    if (replyPreview_.empty())
        replyPreview_ = L"Messaggio";
    ReplyText->Text = ref new String(replyPreview_.c_str());
    ReplyBar->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void ChatPage::ClearReply()
{
    replyTo_ = 0;
    replyPreview_.clear();
    ReplyBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void ChatPage::Message_Tapped(Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
{
    FrameworkElement^ border = dynamic_cast<FrameworkElement^>(sender);
    MsgVM^ vm = border == nullptr ? nullptr : dynamic_cast<MsgVM^>(border->DataContext);
    if (vm == nullptr)
        return;

    if (vm->kind == tg::MsgItem::Photo)
        OpenPhoto(vm);
    else if (vm->kind == tg::MsgItem::Voice || vm->kind == tg::MsgItem::Audio)
        PlayMedia(vm);
    else if (vm->kind == tg::MsgItem::Video || vm->kind == tg::MsgItem::VideoNote ||
             vm->kind == tg::MsgItem::Gif || vm->kind == tg::MsgItem::Document)
        OpenDocumentOrVideo(vm);
    else if (vm->kind == tg::MsgItem::Location)
        OpenLocation(vm);
}

void ChatPage::Message_Holding(Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;
    FrameworkElement^ border = dynamic_cast<FrameworkElement^>(sender);
    MsgVM^ vm = border == nullptr ? nullptr : dynamic_cast<MsgVM^>(border->DataContext);
    if (vm == nullptr)
        return;

    auto flyout = ref new MenuFlyout();
    auto reply = ref new MenuFlyoutItem();
    reply->Text = L"Rispondi";
    reply->Click += ref new RoutedEventHandler([this, vm](Object^, RoutedEventArgs^) { SetReply(vm->Id); });
    flyout->Items->Append(reply);

    auto react = ref new MenuFlyoutItem();
    react->Text = L"Reagisci";
    react->Click += ref new RoutedEventHandler([this, vm, border](Object^, RoutedEventArgs^) { ShowReactionPicker(vm, border); });
    flyout->Items->Append(react);

    if (vm->Text != nullptr && !vm->Text->IsEmpty())
    {
        auto copy = ref new MenuFlyoutItem();
        copy->Text = L"Copia";
        // WP8.1 has no clipboard API: show the text selected, the system copy button does the rest
        copy->Click += ref new RoutedEventHandler([vm](Object^, RoutedEventArgs^)
        {
            auto box = ref new TextBox();
            // the XAML TextBox (RichEdit inside) breaks lines on \r and cuts the text at a bare \n
            std::wstring text;
            const wchar_t* src = vm->Text->Data();
            for (unsigned i = 0; i < vm->Text->Length(); ++i)
            {
                if (src[i] == L'\r') { if (i + 1 < vm->Text->Length() && src[i + 1] == L'\n') ++i; text += L'\r'; }
                else if (src[i] == L'\n') text += L'\r';
                else text += src[i];
            }
            box->AcceptsReturn = true;
            box->TextWrapping = TextWrapping::Wrap;
            box->Text = ref new String(text.c_str(), (unsigned)text.size());
            box->IsReadOnly = true;
            box->MaxHeight = 420;
            ScrollViewer::SetVerticalScrollBarVisibility(box, ScrollBarVisibility::Auto);
            box->Loaded += ref new RoutedEventHandler([box](Object^, RoutedEventArgs^)
            {
                box->Focus(Windows::UI::Xaml::FocusState::Programmatic);
                box->SelectAll();
            });
            auto dialog = ref new ContentDialog();
            dialog->Title = L"Copia";
            dialog->Content = box;
            dialog->PrimaryButtonText = L"Chiudi";
            dialog->ShowAsync();
        });
        flyout->Items->Append(copy);
    }

    auto forward = ref new MenuFlyoutItem();
    forward->Text = L"Inoltra";
    forward->Click += ref new RoutedEventHandler([this, vm](Object^, RoutedEventArgs^)
    {
        TgService& svc = TgService::Get();
        svc.pendingForwardFrom = chatKey_;
        svc.pendingForwardIds.assign(1, vm->Id);
        this->Frame->Navigate(NewChatPage::typeid, L"inoltra");
    });
    flyout->Items->Append(forward);

    auto remove = ref new MenuFlyoutItem();
    remove->Text = L"Elimina";
    remove->Click += ref new RoutedEventHandler([this, vm](Object^, RoutedEventArgs^)
    {
        std::vector<int32_t> ids;
        ids.push_back(vm->Id);
        TgService::Get().Client().deleteMessages(chatKey_, ids, vm->outgoing,
            [this, vm](const tg::RpcErr&)
        {
            for (size_t i = 0; i < items_.size(); ++i)
            {
                if (items_[i].id == vm->Id)
                {
                    items_.erase(items_.begin() + i);
                    messages_->RemoveAt(static_cast<unsigned int>(i));
                    break;
                }
            }
        });
    });
    flyout->Items->Append(remove);

    if (!vm->mediaKey.empty() && vm->kind != tg::MsgItem::Location)
    {
        auto save = ref new MenuFlyoutItem();
        save->Text = L"Salva in Download";
        save->Click += ref new RoutedEventHandler([this, vm](Object^, RoutedEventArgs^) { SaveToDownloads(vm); });
        flyout->Items->Append(save);
    }
    flyout->ShowAt(border);
}

void ChatPage::Send_Click(Object^, RoutedEventArgs^)
{
    std::string text = FromP(Composer->Text);
    if (text.empty())
        return;

    Composer->Text = L"";
    int32_t reply = replyTo_;
    ClearReply();
    TgService::Get().Client().sendText(chatKey_, text, reply,
        [this](const tg::MsgItem& sent, const tg::RpcErr& error)
    {
        if (error.message.empty())
            AddOrUpdateMessage(sent, true);
        else
            ShowError(PStr(error.message));
    });
}

void ChatPage::Photo_Click(Object^, RoutedEventArgs^)
{
    auto picker = ref new Windows::Storage::Pickers::FileOpenPicker();
    picker->ViewMode = Windows::Storage::Pickers::PickerViewMode::Thumbnail;
    picker->SuggestedStartLocation = Windows::Storage::Pickers::PickerLocationId::PicturesLibrary;
    picker->FileTypeFilter->Append(L".jpg");
    picker->FileTypeFilter->Append(L".jpeg");
    picker->FileTypeFilter->Append(L".png");
    picker->PickMultipleFilesAndContinue();
}

void ChatPage::ContinueFileOpenPicker(Windows::ApplicationModel::Activation::FileOpenPickerContinuationEventArgs^ args)
{
    if (args == nullptr || args->Files == nullptr || args->Files->Size == 0) return;
    std::vector<std::wstring> paths;
    for (unsigned int i = 0; i < args->Files->Size; ++i)
    {
        StorageFile^ file = args->Files->GetAt(i);
        if (file != nullptr && file->Path != nullptr) paths.push_back(file->Path->Data());
    }
    ShowAttachmentPreview(paths);
}

void ChatPage::ShowAttachmentPreview(const std::vector<std::wstring>& paths)
{
    attachmentPaths_.clear();
    for (size_t i = 0; i < paths.size(); ++i)
        if (!paths[i].empty()) attachmentPaths_.push_back(paths[i]);
    if (attachmentPaths_.empty()) return;
    attachmentIndex_ = 0;
    AttachmentPreview->Visibility = Windows::UI::Xaml::Visibility::Visible;
    UpdateAttachmentPreview();
}

void ChatPage::UpdateAttachmentPreview()
{
    if (attachmentPaths_.empty() || attachmentIndex_ >= attachmentPaths_.size()) return;
    const unsigned int previewGeneration = ++attachmentPreviewGeneration_;
    const std::wstring path = attachmentPaths_[attachmentIndex_];
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    wchar_t count[64];
    swprintf_s(count, L"Allegato %u di %u", static_cast<unsigned int>(attachmentIndex_ + 1),
               static_cast<unsigned int>(attachmentPaths_.size()));
    AttachmentCount->Text = ref new String(count);
    AttachmentName->Text = ref new String(name.c_str());
    wchar_t sendLabel[48];
    swprintf_s(sendLabel, L"Invia %u", static_cast<unsigned int>(attachmentPaths_.size()));
    PreviewSendButton->Content = ref new String(sendLabel);
    AttachmentImage->Source = nullptr;

    std::wstring ext = name.find_last_of(L'.') == std::wstring::npos ? L"" : name.substr(name.find_last_of(L'.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
    if (ext != L".jpg" && ext != L".jpeg" && ext != L".png") return;

    CREATEFILE2_EXTENDED_PARAMETERS p;
    ZeroMemory(&p, sizeof(p));
    p.dwSize = sizeof(p);
    HANDLE h = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, &p);
    if (h == INVALID_HANDLE_VALUE) return;
    FILE_STANDARD_INFO info = {};
    if (!GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info)) ||
        info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > 20 * 1024 * 1024)
    {
        CloseHandle(h);
        return;
    }
    std::string bytes(static_cast<size_t>(info.EndOfFile.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != bytes.size()) return;
    DecodeImage(bytes, [this, previewGeneration](BitmapImage^ image)
    {
        if (previewGeneration == attachmentPreviewGeneration_ &&
            AttachmentPreview->Visibility == Windows::UI::Xaml::Visibility::Visible)
            AttachmentImage->Source = image;
    });
}

void ChatPage::PreviewPrevious_Click(Object^, RoutedEventArgs^)
{
    if (attachmentPaths_.empty()) return;
    attachmentIndex_ = (attachmentIndex_ + attachmentPaths_.size() - 1) % attachmentPaths_.size();
    UpdateAttachmentPreview();
}

void ChatPage::PreviewNext_Click(Object^, RoutedEventArgs^)
{
    if (attachmentPaths_.empty()) return;
    attachmentIndex_ = (attachmentIndex_ + 1) % attachmentPaths_.size();
    UpdateAttachmentPreview();
}

void ChatPage::PreviewCancel_Click(Object^, RoutedEventArgs^)
{
    ++attachmentPreviewGeneration_;
    AttachmentPreview->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    AttachmentImage->Source = nullptr;
    attachmentPaths_.clear();
}

void ChatPage::PreviewSend_Click(Object^, RoutedEventArgs^)
{
    ++attachmentPreviewGeneration_;
    std::vector<std::wstring> sending;
    sending.swap(attachmentPaths_);
    AttachmentPreview->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    AttachmentImage->Source = nullptr;
    for (size_t i = 0; i < sending.size(); ++i) SendFileAtPath(sending[i]);
}

void ChatPage::SendPickedPhoto(StorageFile^ file)
{
    if (file == nullptr)
        return;
    create_task(file->OpenAsync(FileAccessMode::Read)).then([this](IRandomAccessStream^ input)
    {
        SendPhotoStream(input);
    }, task_continuation_context::use_current());
}

void ChatPage::SendPhotoStream(IRandomAccessStream^ input)
{
    ChatTitle->Text = L"invio foto...";
    create_task(BitmapDecoder::CreateAsync(input)).then([](BitmapDecoder^ decoder)
    {
        unsigned int width = decoder->PixelWidth;
        unsigned int height = decoder->PixelHeight;
        double scale = 1.0;
        unsigned int longest = width > height ? width : height;
        if (longest > 1280)
            scale = 1280.0 / longest;
        unsigned int outWidth = static_cast<unsigned int>(width * scale);
        unsigned int outHeight = static_cast<unsigned int>(height * scale);
        if (outWidth == 0) outWidth = 1;
        if (outHeight == 0) outHeight = 1;

        auto transform = ref new BitmapTransform();
        transform->ScaledWidth = outWidth;
        transform->ScaledHeight = outHeight;
        return create_task(decoder->GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore,
            transform, ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage))
            .then([decoder, outWidth, outHeight](PixelDataProvider^ pixelData)
        {
            auto output = ref new InMemoryRandomAccessStream();
            return create_task(BitmapEncoder::CreateAsync(BitmapEncoder::JpegEncoderId, output))
                .then([decoder, outWidth, outHeight, pixelData, output](BitmapEncoder^ encoder)
            {
                encoder->SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore,
                    outWidth, outHeight, decoder->DpiX, decoder->DpiY, pixelData->DetachPixelData());
                return create_task(encoder->FlushAsync()).then([output]()
                {
                    output->Seek(0);
                    auto reader = ref new DataReader(output);
                    return create_task(reader->LoadAsync(static_cast<unsigned int>(output->Size)))
                        .then([reader](unsigned int loaded)
                    {
                        (void)loaded;
                        auto data = ref new Array<unsigned char>(reader->UnconsumedBufferLength);
                        reader->ReadBytes(data);
                        return FromArray(data);
                    });
                });
            });
        });
    }).then([this](task<std::string> result)
    {
        try
        {
            std::string jpeg = result.get();
            TgService::Get().Client().sendPhoto(chatKey_, jpeg, "",
                [this](const tg::MsgItem& sent, const tg::RpcErr& error)
            {
                ChatTitle->Text = PStr(TgService::Get().Client().chat(chatKey_)
                    ? TgService::Get().Client().chat(chatKey_)->title : chatKey_);
                if (error.message.empty())
                    AddOrUpdateMessage(sent, true);
                else
                    ShowError(PStr(error.message));
            });
        }
        catch (Exception^ error)
        {
            ChatTitle->Text = PStr(TgService::Get().Client().chat(chatKey_)
                ? TgService::Get().Client().chat(chatKey_)->title : chatKey_);
            ShowError(error->Message);
        }
    }, task_continuation_context::use_current());
}

void ChatPage::Voice_Click(Object^, RoutedEventArgs^)
{
    if (!recording_)
        StartRecording();
    else
        StopRecording(true);
}

void ChatPage::StartRecording()
{
    if (recording_)
        return;

    recording_ = true;
    recordingStarted_ = NowDT();
    RecordingBar->Visibility = Windows::UI::Xaml::Visibility::Visible;
    RecordingText->Text = L"\u25CF  Registrazione 0:00";
    capture_ = ref new MediaCapture();
    auto settings = ref new MediaCaptureInitializationSettings();
    settings->StreamingCaptureMode = StreamingCaptureMode::Audio;

    // on the Lumia 1020 only "Enhanced Audio Recording Device" (Nokia Rich Recording) gives sound;
    // the default capture device records silence (found with the Registratore app). A plugged-in headset
    // shows up as "Headset Microphone" and wins.
    create_task(Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(Windows::Devices::Enumeration::DeviceClass::AudioCapture))
        .then([this, settings](task<Windows::Devices::Enumeration::DeviceInformationCollection^> t)
    {
        try
        {
            auto all = t.get();
            String^ headset = nullptr;
            String^ bluetooth = nullptr;
            for (unsigned int i = 0; i < all->Size; ++i)
            {
                auto d = all->GetAt(i);
                if (!d->IsEnabled) continue;
                // a Bluetooth headset: any capture device not from the phone's own QCAUD driver
                if (!wcsstr(d->Name->Data(), L"QCAUD") || wcsstr(d->Name->Data(), L"Bluetooth") || wcsstr(d->Name->Data(), L"Hands-Free")) bluetooth = d->Id;
                else if (wcsncmp(d->Name->Data(), L"Headset Microphone", 18) == 0) headset = d->Id;
                else if (wcsstr(d->Name->Data(), L"Enhanced Audio Recording")) settings->AudioDeviceId = d->Id;
            }
            if (headset != nullptr) settings->AudioDeviceId = headset;
            if (bluetooth != nullptr) settings->AudioDeviceId = bluetooth;
        }
        catch (Exception^) {}
        return create_task(capture_->InitializeAsync(settings));
    }, task_continuation_context::use_current()).then([this]()
    {
        return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(
            L"telegram-voice.m4a", CreationCollisionOption::ReplaceExisting));
    }).then([this](StorageFile^ file)
    {
        recordingFile_ = file;
        return create_task(capture_->StartRecordToStorageFileAsync(
            MediaEncodingProfile::CreateM4a(AudioEncodingQuality::Medium), file));
    }).then([this]()
    {
        recordingStarted_ = NowDT();
        recordingTimer_ = ref new DispatcherTimer();
        Windows::Foundation::TimeSpan second;
        second.Duration = 10000000;
        recordingTimer_->Interval = second;
        recordingTimer_->Tick += ref new EventHandler<Object^>([this](Object^, Object^)
        {
            int seconds = static_cast<int>((NowDT().UniversalTime -
                recordingStarted_.UniversalTime) / 10000000);
            wchar_t time[16];
            swprintf_s(time, 16, L"%d:%02d", seconds / 60, seconds % 60);
            RecordingText->Text = L"\u25CF  Registrazione " + ref new String(time);
        });
        recordingTimer_->Start();
    }, task_continuation_context::use_current()).then([this](task<void> result)
    {
        try { result.get(); }
        catch (Exception^ error)
        {
            recording_ = false;
            Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
                ref new Windows::UI::Core::DispatchedHandler([this, error]()
            {
                RecordingBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                ShowError(error->Message);
            }));
        }
    }, task_continuation_context::use_current());
}

void ChatPage::StopRecording_Click(Object^, RoutedEventArgs^)
{
    StopRecording(true);
}

void ChatPage::CancelRecording_Click(Object^, RoutedEventArgs^)
{
    StopRecording(false);
}

void ChatPage::StopRecording(bool send)
{
    if (!recording_)
        return;

    recording_ = false;
    if (recordingTimer_ != nullptr)
    {
        recordingTimer_->Stop();
        recordingTimer_ = nullptr;
    }
    RecordingBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;

    if (capture_ == nullptr || recordingFile_ == nullptr)
        return;

    MediaCapture^ capture = capture_;
    StorageFile^ file = recordingFile_;
    capture_ = nullptr;
    create_task(capture->StopRecordAsync()).then([this, file, send]()
    {
        if (send)
            SendRecordedFile(file);
        else
            create_task(file->DeleteAsync());
    }, task_continuation_context::use_current());
}

void ChatPage::SendRecordedFile(StorageFile^ file)
{
    // the phone records AAC (M4A); Media Foundation decodes it to PCM, then we encode Telegram's OGG/Opus
    if (file == nullptr)
        return;
    std::wstring path(file->Path->Data());
    ChatTitle->Text = L"invio vocale...";
    create_task([path]()
    {
        std::string wav, err, ogg, waveform;
        int peak = 0, seconds = 0;
        double secs = 0;
        if (!DecodeToPcmWav(path, wav, err, &peak, &secs))
            throw std::runtime_error("decodifica: " + err);
        if (!tg::wavToOggOpus(wav, ogg, &seconds, &waveform))
            throw std::runtime_error("conversione Opus non riuscita");
        return std::make_tuple(ogg, waveform, seconds);
    }).then([this, file](task<std::tuple<std::string, std::string, int> > converted)
    {
        const tg::ChatItem* chat = TgService::Get().Client().chat(chatKey_);
        ChatTitle->Text = PStr(chat ? chat->title : chatKey_);
        create_task(file->DeleteAsync()).then([](task<void> d) { try { d.get(); } catch (Exception^) {} });
        try
        {
            auto result = converted.get();
            TgService::Get().Log("voice: " + std::to_string(static_cast<long long>(std::get<2>(result))) + " s, ogg " +
                                 std::to_string(static_cast<long long>(std::get<0>(result).size())) + " bytes");
            TgService::Get().Client().sendVoice(chatKey_, std::get<0>(result), std::get<2>(result), std::get<1>(result),
                [this](const tg::MsgItem& sent, const tg::RpcErr& error)
            {
                if (error.ok())
                    AddOrUpdateMessage(sent, true);
                else
                    ShowError(PStr(error.message));
            });
        }
        catch (const std::exception& error)
        {
            ShowError(PStr(error.what()));
        }
    }, task_continuation_context::use_current());
}

void ChatPage::Mute_Click(Object^, RoutedEventArgs^)
{
    const tg::ChatItem* chat = TgService::Get().Client().chat(chatKey_);
    if (chat == nullptr)
        return;
    bool mute = !chat->muted;
    TgService::Get().Client().setMuted(chatKey_, mute, [this, mute](const tg::RpcErr&)
    {
        MuteButton->Label = mute ? L"riattiva" : L"silenzia";
        MuteButton->Icon = ref new SymbolIcon(mute ? Symbol::Volume : Symbol::Mute);
    });
}

void ChatPage::CancelReply_Click(Object^, RoutedEventArgs^)
{
    ClearReply();
}

void ChatPage::AudioPlayer_MediaEnded(Object^, RoutedEventArgs^)
{
    audioPlaying_ = false;
    if (playingVM_ != nullptr)
    {
        playingVM_->MediaLabel = L"\u25B6";
        playingVM_ = nullptr;
    }
}

void ChatPage::LoadPhotoPreview(MsgVM^ vm)
{
    if (vm == nullptr || vm->thumbKey.empty())
        return;

    auto show = [this, vm](BitmapImage^ bitmap)
    {
        if (bitmap == nullptr || !active_)
            return;
        vm->Image = bitmap;
        vm->ImageVisibility = Windows::UI::Xaml::Visibility::Visible;
        if (stick_) ScrollToBottom();
    };

    if (vm->kind == tg::MsgItem::Photo)
    {
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].id == vm->Id && !items_[i].strippedJpeg.empty())
            {
                DecodeImage(items_[i].strippedJpeg, show);
                break;
            }
        }
    }

    std::string key = vm->thumbKey;
    TgService::Get().GetMedia(key, [this, show](const std::string& bytes, bool ok)
    {
        if (!ok || bytes.empty() || !active_)
            return;
        DecodeImage(bytes, show);
    });
}

void ChatPage::OpenPhoto(MsgVM^ vm)
{
    if (vm == nullptr || vm->mediaKey.empty())
        return;
    Frame->Navigate(ImagePage::typeid, PStr(vm->mediaKey));
}

void ChatPage::PlayMedia(MsgVM^ vm)
{
    if (vm == nullptr || vm->mediaKey.empty())
        return;
    if (audioPlaying_ && playingVM_ == vm)
    {
        AudioPlayer->Stop();
        audioPlaying_ = false;
        vm->MediaLabel = L"\u25B6";
        return;
    }

    vm->MediaLabel = L"Scarico...";
    std::string key = vm->mediaKey;
    bool voice = vm->kind == tg::MsgItem::Voice;
    TgService::Get().GetMedia(key, [this, vm, voice](const std::string& bytes, bool ok)
    {
        if (!active_)
            return;
        if (!ok)
        {
            vm->MediaLabel = L"Errore download";
            return;
        }
        if (voice)
        {
            create_task([bytes]()
            {
                std::string wav;
                int seconds = 0;
                if (!tg::oggOpusToWav(bytes, wav, &seconds))
                    throw std::runtime_error("Decodifica audio non riuscita");
                return wav;
            }).then([this, vm](task<std::string> decoded)
            {
                try
                {
                    std::string wav = decoded.get();
                    auto stream = ref new InMemoryRandomAccessStream();
                    auto writer = ref new DataWriter(stream);
                    writer->WriteBytes(ToArray(wav));
                    create_task(writer->StoreAsync()).then([this, writer, stream, vm](unsigned int)
                    {
                        writer->DetachStream();
                        stream->Seek(0);
                        AudioPlayer->SetSource(stream, L"audio/wav");
                        AudioPlayer->Visibility = Windows::UI::Xaml::Visibility::Visible;
                        vm->MediaLabel = L"\u23F8";
                        playingVM_ = vm;
                        audioPlaying_ = true;
                    }, task_continuation_context::use_current());
                }
                catch (std::exception const& error)
                {
                    vm->MediaLabel = L"Errore audio";
                    ShowError(ref new String(Utf16(error.what()).c_str()));
                }
            }, task_continuation_context::use_current());
        }
        else
        {
            String^ name = DisplayFileName(vm->fileName);
            create_task(ApplicationData::Current->TemporaryFolder->CreateFileAsync(
                name, CreationCollisionOption::ReplaceExisting)).then([this, bytes, vm](StorageFile^ file)
            {
                auto array = ToArray(bytes);
                return create_task(FileIO::WriteBytesAsync(file, array)).then([this, file, vm]()
                {
                    create_task(file->OpenAsync(FileAccessMode::Read)).then([this, vm](IRandomAccessStream^ stream)
                    {
                        AudioPlayer->SetSource(stream, vm->mime.empty() ? L"audio/mpeg" : PStr(vm->mime));
                        AudioPlayer->Visibility = Windows::UI::Xaml::Visibility::Visible;
                        vm->MediaLabel = L"\u23F8";
                        playingVM_ = vm;
                        audioPlaying_ = true;
                    }, task_continuation_context::use_current());
                });
            }, task_continuation_context::use_current());
        }
    }, [vm](int64_t received, int64_t total)
    {
        if (total > 0)
        {
            int percent = static_cast<int>(received * 100 / total);
            wchar_t text[32];
            swprintf_s(text, 32, L"Scarico %d%%", percent);
            vm->MediaLabel = ref new String(text);
        }
    });
}

void ChatPage::OpenDocumentOrVideo(MsgVM^ vm)
{
    if (vm == nullptr || vm->mediaKey.empty())
        return;

    vm->MediaLabel = L"Scarico...";
    std::string key = vm->mediaKey;
    TgService::Get().GetMedia(key, [this, vm](const std::string& bytes, bool ok)
    {
        if (!active_)
            return;
        if (!ok)
        {
            vm->MediaLabel = L"Errore download";
            return;
        }
        std::wstring name = Utf16(vm->fileName);
        if (name.empty())
        {
            wchar_t generated[40];
            swprintf_s(generated, 40, L"video_%d.mp4", vm->Id);
            name = generated;
        }
        TgService::Get().OpenWithSystem(bytes, name);
    }, [vm](int64_t received, int64_t total)
    {
        if (total > 0)
        {
            wchar_t text[32];
            swprintf_s(text, 32, L"Scarico %d%%", static_cast<int>(received * 100 / total));
            vm->MediaLabel = ref new String(text);
        }
    });
}

void ChatPage::OpenLocation(MsgVM^ vm)
{
    if (vm == nullptr)
        return;
    std::wstring location = Utf16(vm->mediaKey);
    size_t colon = location.find(L':');
    size_t comma = location.find(L',');
    if (colon == std::wstring::npos || comma == std::wstring::npos)
        return;
    std::wstring lat = location.substr(colon + 1, comma - colon - 1);
    std::wstring lon = location.substr(comma + 1);
    try
    {
        Launcher::LaunchUriAsync(ref new Uri(ref new String((L"bingmaps:?collection=point." + lat + L"_" + lon).c_str())));
    }
    catch (...) {}
}

void ChatPage::ShowError(String^ message)
{
    TgService::Get().Log("chat error: " + FromP(message));
    auto dialog = ref new Windows::UI::Popups::MessageDialog(message, L"Telegram");
    create_task(dialog->ShowAsync());
}

void ChatPage::SaveToDownloads(MsgVM^ vm)
{
    String^ before = vm->MediaLabel;
    TgService::Get().GetMedia(vm->mediaKey, [this, vm, before](const std::string& bytes, bool ok)
    {
        std::wstring name = Utf16(vm->fileName);
        if (name.empty())
        {
            const wchar_t* ext = vm->kind == tg::MsgItem::Photo ? L".jpg" : vm->kind == tg::MsgItem::Voice ? L".ogg" :
                vm->kind == tg::MsgItem::Video || vm->kind == tg::MsgItem::VideoNote || vm->kind == tg::MsgItem::Gif ? L".mp4" : L"";
            name = L"telegram_" + std::to_wstring(vm->Id) + ext;
        }
        std::wstring path = ok ? TgService::Get().SaveToDownloads(bytes, name) : std::wstring();
        if (active_ && vm->MediaVisibility == Windows::UI::Xaml::Visibility::Visible) vm->MediaLabel = before;
        auto dialog = ref new Windows::UI::Popups::MessageDialog(path.empty() ? L"Salvataggio non riuscito"
            : ref new String((L"Salvato in Download\\Telegram:\n" + path.substr(path.find_last_of(L'\\') + 1)).c_str()), L"Telegram");
        create_task(dialog->ShowAsync());
    }, [vm](int64_t received, int64_t total)
    {
        if (total > 0)
        {
            wchar_t text[32];
            swprintf_s(text, 32, L"Scarico %d%%", static_cast<int>(received * 100 / total));
            vm->MediaLabel = ref new String(text);
            vm->MediaVisibility = Windows::UI::Xaml::Visibility::Visible;
        }
    });
}

void ChatPage::File_Click(Object^, RoutedEventArgs^)
{
    Frame->Navigate(FilePage::typeid);
}

void ChatPage::SendFileAtPath(const std::wstring& path)
{
    CREATEFILE2_EXTENDED_PARAMETERS p;
    ZeroMemory(&p, sizeof(p));
    p.dwSize = sizeof(p);
    HANDLE h = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &p);
    if (h == INVALID_HANDLE_VALUE) { ShowError(L"Impossibile aprire il file"); return; }
    std::string bytes;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) bytes.append(buf, n);
    CloseHandle(h);
    std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
    std::wstring ext = name.find_last_of(L'.') == std::wstring::npos ? L"" : name.substr(name.find_last_of(L'.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
    if (ext == L".jpg" || ext == L".jpeg" || ext == L".png")
    {
        SendPhotoBytes(bytes);
        return;
    }
    const char* mime = ext == L".pdf" ? "application/pdf" : ext == L".mp4" ? "video/mp4" : ext == L".mp3" ? "audio/mpeg" :
        ext == L".m4a" ? "audio/mp4" : ext == L".txt" ? "text/plain" : ext == L".zip" ? "application/zip" :
        ext == L".docx" ? "application/vnd.openxmlformats-officedocument.wordprocessingml.document" :
        ext == L".xlsx" ? "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" : "application/octet-stream";
    ChatTitle->Text = L"invio file...";
    TgService::Get().Client().sendFile(chatKey_, bytes, Utf8(name), mime, "", [this](const tg::MsgItem& sent, const tg::RpcErr& error)
    {
        const tg::ChatItem* chat = TgService::Get().Client().chat(chatKey_);
        ChatTitle->Text = PStr(chat ? chat->title : chatKey_);
        if (error.ok()) AddOrUpdateMessage(sent, true);
        else ShowError(PStr(error.message));
    });
}

void ChatPage::SendPhotoBytes(const std::string& bytes)
{
    auto stream = ref new InMemoryRandomAccessStream();
    auto writer = ref new DataWriter(stream);
    writer->WriteBytes(ToArray(bytes));
    create_task(writer->StoreAsync()).then([this, writer, stream](unsigned int)
    {
        writer->DetachStream();
        stream->Seek(0);
        SendPhotoStream(stream);
    }, task_continuation_context::use_current());
}
