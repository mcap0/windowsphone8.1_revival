#include "pch.h"
#include "ViewModels.h"
#include "TgService.h"

#include <stdio.h>
#include <wctype.h>
#include <time.h>

using namespace Telegram;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Media;

namespace
{
    String^ FormatDateTime(int32_t unixTime, bool showDate)
    {
        if (unixTime <= 0)
            return L"";

        time_t value = static_cast<time_t>(unixTime);
        tm local;
        memset(&local, 0, sizeof(local));
        localtime_s(&local, &value);

        wchar_t text[32];
        if (showDate)
            swprintf_s(text, 32, L"%02d/%02d", local.tm_mday, local.tm_mon + 1);
        else
            swprintf_s(text, 32, L"%02d:%02d", local.tm_hour, local.tm_min);
        return ref new String(text);
    }

    String^ ChatTime(int32_t unixTime)
    {
        if (unixTime <= 0)
            return L"";

        time_t value = static_cast<time_t>(unixTime);
        time_t now = time(nullptr);
        tm messageTime;
        tm currentTime;
        memset(&messageTime, 0, sizeof(messageTime));
        memset(&currentTime, 0, sizeof(currentTime));
        localtime_s(&messageTime, &value);
        localtime_s(&currentTime, &now);

        if (messageTime.tm_year == currentTime.tm_year &&
            messageTime.tm_yday == currentTime.tm_yday)
            return FormatDateTime(unixTime, false);

        time_t yesterdayValue = now - 24 * 60 * 60;
        tm yesterday;
        memset(&yesterday, 0, sizeof(yesterday));
        localtime_s(&yesterday, &yesterdayValue);
        if (messageTime.tm_year == yesterday.tm_year &&
            messageTime.tm_yday == yesterday.tm_yday)
            return L"ieri";

        return FormatDateTime(unixTime, true);
    }

    String^ MessageTime(const tg::MsgItem& message, const tg::ChatItem* chat)
    {
        String^ value = FormatDateTime(message.date, false);
        if (!message.out)
            return value;

        bool read = chat != nullptr && message.id <= chat->readOutboxMax;
        return value + (read ? L"  \u2713\u2713" : L"  \u2713");
    }

    String^ MediaDescription(const tg::MsgItem& message)
    {
        wchar_t duration[24];
        int minutes = message.duration / 60;
        int seconds = message.duration % 60;

        switch (message.kind)
        {
        case tg::MsgItem::Voice:
            swprintf_s(duration, 24, L"%d:%02d", minutes, seconds);
            return ref new String(L"\u25B6 Vocale ") + ref new String(duration);
        case tg::MsgItem::Audio:
            swprintf_s(duration, 24, L"%d:%02d", minutes, seconds);
            return ref new String(L"\u25B6 Audio ") + ref new String(duration);
        case tg::MsgItem::Video:
            swprintf_s(duration, 24, L"%d:%02d", minutes, seconds);
            return ref new String(L"\u25B6 Video ") + ref new String(duration);
        case tg::MsgItem::VideoNote:
            swprintf_s(duration, 24, L"%d:%02d", minutes, seconds);
            return ref new String(L"\u25B6 Video ") + ref new String(duration);
        case tg::MsgItem::Gif:
            return L"\u25B6 GIF";
        case tg::MsgItem::Sticker:
            return ref new String(L"Sticker ") + (message.emoji.empty() ? ref new String(L"") : ref new String(
                Telegram::Utf16(message.emoji).c_str()));
        case tg::MsgItem::Document:
        {
            std::wstring name = Telegram::Utf16(message.fileName);
            if (name.empty())
                name = L"Documento";
            wchar_t size[40];
            if (message.size > 0)
                swprintf_s(size, 40, L" (%.1f MB)", static_cast<double>(message.size) / (1024.0 * 1024.0));
            else
                size[0] = 0;
            return ref new String(L"\U0001F4C4 ") + ref new String(name.c_str()) + ref new String(size);
        }
        case tg::MsgItem::Location:
            return L"\u2302 Posizione";
        case tg::MsgItem::Contact:
            return L"Contatto";
        case tg::MsgItem::Poll:
            return L"Sondaggio";
        default:
            return L"";
        }
    }

    // Telegram's avatar palette, picked by a stable hash of the peer key
    Brush^ PaletteBrush(const std::string& key)
    {
        static const unsigned int colors[] = { 0xE17076, 0x7BC862, 0xE5CA77, 0x65AADD, 0xA695E7, 0xEE7AAE, 0x6EC9CB, 0xFAA774 };
        unsigned int h = 0;
        for (size_t i = 0; i < key.size(); ++i) h = h * 31 + static_cast<unsigned char>(key[i]);
        unsigned int c = colors[h % 8];
        return ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, (c >> 16) & 255, (c >> 8) & 255, c & 255));
    }

    String^ InitialsOf(const std::wstring& title)
    {
        std::wstring out;
        bool start = true;
        for (size_t i = 0; i < title.size() && out.size() < 2; ++i)
        {
            wchar_t c = title[i];
            if (c == L' ') { start = true; continue; }
            if (start && iswalnum(c)) out += towupper(c);   // emoji and symbols are skipped
            start = false;
        }
        return ref new String(out.empty() ? L"?" : out.c_str());
    }

    Brush^ AppBrush(const wchar_t* key)
    {
        try
        {
            Object^ value = Application::Current->Resources->Lookup(ref new String(key));
            return dynamic_cast<Brush^>(value);
        }
        catch (...)
        {
            return ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 40, 40, 40));
        }
    }
}

ChatVM::ChatVM(const tg::ChatItem& item) :
    key_(L""), title_(L""), preview_(L""), time_(L""), unreadText_(L""),
    unreadVisibility_(Windows::UI::Xaml::Visibility::Collapsed), titleOpacity_(1.0)
{
    Update(item);
}

void ChatVM::RaisePropertyChanged(String^ name)
{
    PropertyChanged(this, ref new PropertyChangedEventArgs(name));
}

String^ ChatVM::Key::get() { return key_; }
void ChatVM::Key::set(String^ value) { key_ = value; RaisePropertyChanged(L"Key"); }
String^ ChatVM::Title::get() { return title_; }
void ChatVM::Title::set(String^ value) { title_ = value; RaisePropertyChanged(L"Title"); }
String^ ChatVM::Preview::get() { return preview_; }
void ChatVM::Preview::set(String^ value) { preview_ = value; RaisePropertyChanged(L"Preview"); }
String^ ChatVM::Time::get() { return time_; }
void ChatVM::Time::set(String^ value) { time_ = value; RaisePropertyChanged(L"Time"); }
String^ ChatVM::UnreadText::get() { return unreadText_; }
void ChatVM::UnreadText::set(String^ value) { unreadText_ = value; RaisePropertyChanged(L"UnreadText"); }
Visibility ChatVM::UnreadVisibility::get() { return unreadVisibility_; }
void ChatVM::UnreadVisibility::set(Visibility value)
{
    unreadVisibility_ = value;
    RaisePropertyChanged(L"UnreadVisibility");
}
double ChatVM::TitleOpacity::get() { return titleOpacity_; }
void ChatVM::TitleOpacity::set(double value) { titleOpacity_ = value; RaisePropertyChanged(L"TitleOpacity"); }

void ChatVM::Update(const tg::ChatItem& item)
{
    Key = ref new String(Telegram::Utf16(item.key).c_str());
    Title = ref new String(Telegram::Utf16(item.title).c_str());
    Preview = ref new String(Telegram::Utf16(item.lastText).c_str());
    Time = ChatTime(item.lastDate);
    UnreadText = item.unread > 0 ? ref new String(std::to_wstring(item.unread).c_str()) : L"";
    UnreadVisibility = item.unread > 0 ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    TitleOpacity = item.muted ? 0.6 : 1.0;
    initials_ = InitialsOf(Telegram::Utf16(item.title));
    avatarBrush_ = PaletteBrush(item.key);
    badgeBrush_ = AppBrush(item.muted ? L"BadgeMuted" : L"TgBlue");
    RaisePropertyChanged(L"Initials");
    RaisePropertyChanged(L"AvatarBrush");
    RaisePropertyChanged(L"BadgeBrush");
}

MsgVM::MsgVM() :
    id_(0), sender_(L""), text_(L""), time_(L""), align_(HorizontalAlignment::Left),
    background_(nullptr), image_(nullptr), imageVisibility_(Windows::UI::Xaml::Visibility::Collapsed),
    mediaLabel_(L""), mediaVisibility_(Windows::UI::Xaml::Visibility::Collapsed), reply_(L""),
    comments_(L""), reactions_(ref new Platform::Collections::Vector<ReactionVM^>()),
    kind(tg::MsgItem::Text), duration(0), outgoing(false)
{
}

void MsgVM::RaisePropertyChanged(String^ name)
{
    PropertyChanged(this, ref new PropertyChangedEventArgs(name));
}

int MsgVM::Id::get() { return id_; }
void MsgVM::Id::set(int value) { id_ = value; RaisePropertyChanged(L"Id"); }
String^ MsgVM::Sender::get() { return sender_; }
void MsgVM::Sender::set(String^ value) { sender_ = value; RaisePropertyChanged(L"Sender"); }
String^ MsgVM::Text::get() { return text_; }
void MsgVM::Text::set(String^ value) { text_ = value; RaisePropertyChanged(L"Text"); }
String^ MsgVM::Time::get() { return time_; }
void MsgVM::Time::set(String^ value) { time_ = value; RaisePropertyChanged(L"Time"); }
HorizontalAlignment MsgVM::Align::get() { return align_; }
void MsgVM::Align::set(HorizontalAlignment value) { align_ = value; RaisePropertyChanged(L"Align"); }
Brush^ MsgVM::Background::get() { return background_; }
void MsgVM::Background::set(Brush^ value) { background_ = value; RaisePropertyChanged(L"Background"); }
ImageSource^ MsgVM::Image::get() { return image_; }
void MsgVM::Image::set(ImageSource^ value) { image_ = value; RaisePropertyChanged(L"Image"); }
Visibility MsgVM::ImageVisibility::get() { return imageVisibility_; }
void MsgVM::ImageVisibility::set(Visibility value)
{
    imageVisibility_ = value;
    RaisePropertyChanged(L"ImageVisibility");
}
String^ MsgVM::MediaLabel::get() { return mediaLabel_; }
void MsgVM::MediaLabel::set(String^ value) { mediaLabel_ = value; RaisePropertyChanged(L"MediaLabel"); }
Visibility MsgVM::MediaVisibility::get() { return mediaVisibility_; }
void MsgVM::MediaVisibility::set(Visibility value)
{
    mediaVisibility_ = value;
    RaisePropertyChanged(L"MediaVisibility");
}
String^ MsgVM::Reply::get() { return reply_; }
void MsgVM::Reply::set(String^ value) { reply_ = value; RaisePropertyChanged(L"Reply"); }

void MsgVM::Update(const tg::MsgItem& message, const tg::ChatItem* chat)
{
    Id = message.id;
    Sender = (!message.out && chat != nullptr && (chat->isGroup || chat->isChannel))
        ? ref new String(Telegram::Utf16(message.fromName).c_str()) : L"";
    Text = ref new String(Telegram::Utf16(message.text).c_str());
    Time = MessageTime(message, chat);
    Align = message.out ? HorizontalAlignment::Right : HorizontalAlignment::Left;
    Background = AppBrush(message.out ? L"BubbleOut" : L"BubbleIn");
    Image = nullptr;
    ImageVisibility = Windows::UI::Xaml::Visibility::Collapsed;
    MediaLabel = MediaDescription(message);
    MediaVisibility = MediaLabel->IsEmpty() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;

    mediaKey = message.mediaKey;
    thumbKey = message.thumbKey;
    fileName = message.fileName;
    mime = message.mime;
    emoji = message.emoji;
    kind = static_cast<int>(message.kind);
    duration = message.duration;
    outgoing = message.out;

    if (message.kind == tg::MsgItem::Sticker && !message.emoji.empty())
    {
        Text = ref new String(Telegram::Utf16(message.emoji).c_str());
        MediaLabel = L"";
        MediaVisibility = Windows::UI::Xaml::Visibility::Collapsed;
    }
    senderBrush_ = PaletteBrush(message.fromKey);
    bubbleMargin_ = message.out ? Thickness(64, 2, 10, 2) : Thickness(10, 2, 64, 2);
    if (message.kind == tg::MsgItem::Service)
    {
        Align = HorizontalAlignment::Center;
        Background = AppBrush(L"BubbleService");
        bubbleMargin_ = Thickness(40, 6, 40, 6);
    }
    RaisePropertyChanged(L"SenderBrush");
    RaisePropertyChanged(L"BubbleMargin");
    RaisePropertyChanged(L"SenderVisibility");
    RaisePropertyChanged(L"ReplyVisibility");
    RaisePropertyChanged(L"TextVisibility");
    SetReactions(message.reactions);
    std::wstring comments;
    if (message.hasComments)
        comments = message.replies == 1 ? L"1 commento" : message.replies > 1 ? std::to_wstring(message.replies) + L" commenti" : L"commenta";
    comments_ = ref new String(comments.c_str());
    RaisePropertyChanged(L"CommentsText");
    RaisePropertyChanged(L"CommentsVisibility");
}

ReactionVM::ReactionVM(int32_t msg, const tg::Reaction& r) : msgId(msg), emoji(r.emoji), mine(r.mine)
{
    std::wstring t = Telegram::Utf16(r.emoji) + L" " + std::to_wstring(r.count);
    text_ = ref new String(t.c_str());
    background_ = r.mine ? AppBrush(L"TgBlue") : ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(0x40, 0xFF, 0xFF, 0xFF));
}

void MsgVM::SetReactions(const std::vector<tg::Reaction>& reactions)
{
    reactions_->Clear();
    for (size_t i = 0; i < reactions.size(); ++i) reactions_->Append(ref new ReactionVM(id_, reactions[i]));
    RaisePropertyChanged(L"ReactionsVisibility");
}

MsgVM^ MsgVM::From(const tg::MsgItem& message, const tg::ChatItem* chat)
{
    auto result = ref new MsgVM();
    result->Update(message, chat);
    return result;
}
