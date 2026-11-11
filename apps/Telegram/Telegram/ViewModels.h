#pragma once

#include "client.h"

#include <collection.h>

#include <string>

namespace Telegram
{
    [Windows::UI::Xaml::Data::Bindable]
    public ref class ChatVM sealed : Windows::UI::Xaml::Data::INotifyPropertyChanged
    {
    internal:
        ChatVM(const tg::ChatItem& item);
        void Update(const tg::ChatItem& item);

    public:

        virtual event Windows::UI::Xaml::Data::PropertyChangedEventHandler^ PropertyChanged;

        property Platform::String^ Key
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Platform::String^ Title
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Platform::String^ Preview
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Platform::String^ Time
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Platform::String^ UnreadText
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Windows::UI::Xaml::Visibility UnreadVisibility
        {
            Windows::UI::Xaml::Visibility get();
            void set(Windows::UI::Xaml::Visibility value);
        }
        property double TitleOpacity
        {
            double get();
            void set(double value);
        }
        property Platform::String^ Initials { Platform::String^ get() { return initials_; } }
        property Windows::UI::Xaml::Media::Brush^ AvatarBrush { Windows::UI::Xaml::Media::Brush^ get() { return avatarBrush_; } }
        property Windows::UI::Xaml::Media::Brush^ BadgeBrush { Windows::UI::Xaml::Media::Brush^ get() { return badgeBrush_; } }

    private:
        Platform::String^ key_;
        Platform::String^ title_;
        Platform::String^ preview_;
        Platform::String^ time_;
        Platform::String^ unreadText_;
        Windows::UI::Xaml::Visibility unreadVisibility_;
        double titleOpacity_;
        Platform::String^ initials_;
        Windows::UI::Xaml::Media::Brush^ avatarBrush_;
        Windows::UI::Xaml::Media::Brush^ badgeBrush_;

        void RaisePropertyChanged(Platform::String^ name);
    };

    // One reaction under a bubble: "👍 4", blue when it is mine. Tapping it adds or takes away mine.
    [Windows::UI::Xaml::Data::Bindable]
    public ref class ReactionVM sealed
    {
    public:
        property Platform::String^ Text { Platform::String^ get() { return text_; } }
        property Windows::UI::Xaml::Media::Brush^ Background { Windows::UI::Xaml::Media::Brush^ get() { return background_; } }
    internal:
        ReactionVM(int32_t msg, const tg::Reaction& r);
        int32_t msgId;
        std::string emoji;
        bool mine;
    private:
        Platform::String^ text_;
        Windows::UI::Xaml::Media::Brush^ background_;
    };

    [Windows::UI::Xaml::Data::Bindable]
    public ref class MsgVM sealed : Windows::UI::Xaml::Data::INotifyPropertyChanged
    {
    public:
        MsgVM();

        virtual event Windows::UI::Xaml::Data::PropertyChangedEventHandler^ PropertyChanged;

        property int Id
        {
            int get();
            void set(int value);
        }
        property Platform::String^ Sender
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Platform::String^ Text
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Platform::String^ Time
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Windows::UI::Xaml::HorizontalAlignment Align
        {
            Windows::UI::Xaml::HorizontalAlignment get();
            void set(Windows::UI::Xaml::HorizontalAlignment value);
        }
        property Windows::UI::Xaml::Media::Brush^ Background
        {
            Windows::UI::Xaml::Media::Brush^ get();
            void set(Windows::UI::Xaml::Media::Brush^ value);
        }
        property Windows::UI::Xaml::Media::ImageSource^ Image
        {
            Windows::UI::Xaml::Media::ImageSource^ get();
            void set(Windows::UI::Xaml::Media::ImageSource^ value);
        }
        property Windows::UI::Xaml::Visibility ImageVisibility
        {
            Windows::UI::Xaml::Visibility get();
            void set(Windows::UI::Xaml::Visibility value);
        }
        property Platform::String^ MediaLabel
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Windows::UI::Xaml::Visibility MediaVisibility
        {
            Windows::UI::Xaml::Visibility get();
            void set(Windows::UI::Xaml::Visibility value);
        }
        property Platform::String^ Reply
        {
            Platform::String^ get();
            void set(Platform::String^ value);
        }
        property Windows::UI::Xaml::Media::Brush^ SenderBrush { Windows::UI::Xaml::Media::Brush^ get() { return senderBrush_; } }
        property Windows::UI::Xaml::Thickness BubbleMargin { Windows::UI::Xaml::Thickness get() { return bubbleMargin_; } }
        property Windows::UI::Xaml::Visibility SenderVisibility { Windows::UI::Xaml::Visibility get() { return sender_ == nullptr || sender_->IsEmpty() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible; } }
        property Windows::UI::Xaml::Visibility ReplyVisibility { Windows::UI::Xaml::Visibility get() { return reply_ == nullptr || reply_->IsEmpty() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible; } }
        property Windows::Foundation::Collections::IVector<ReactionVM^>^ Reactions { Windows::Foundation::Collections::IVector<ReactionVM^>^ get() { return reactions_; } }
        property Windows::UI::Xaml::Visibility ReactionsVisibility { Windows::UI::Xaml::Visibility get() { return reactions_->Size == 0 ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible; } }
        property Platform::String^ CommentsText { Platform::String^ get() { return comments_; } }
        property Windows::UI::Xaml::Visibility CommentsVisibility { Windows::UI::Xaml::Visibility get() { return comments_->IsEmpty() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible; } }
        property Windows::UI::Xaml::Visibility TextVisibility { Windows::UI::Xaml::Visibility get() { return text_ == nullptr || text_->IsEmpty() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible; } }

    internal:
        void Update(const tg::MsgItem& message, const tg::ChatItem* chat);
        void SetReactions(const std::vector<tg::Reaction>& reactions);
        static MsgVM^ From(const tg::MsgItem& message, const tg::ChatItem* chat);
        std::string mediaKey;
        std::string thumbKey;
        std::string fileName;
        std::string mime;
        std::string emoji;
        int kind;
        int duration;
        bool outgoing;

    private:
        int id_;
        Platform::String^ sender_;
        Platform::String^ text_;
        Platform::String^ time_;
        Windows::UI::Xaml::HorizontalAlignment align_;
        Windows::UI::Xaml::Media::Brush^ background_;
        Windows::UI::Xaml::Media::ImageSource^ image_;
        Windows::UI::Xaml::Visibility imageVisibility_;
        Platform::String^ mediaLabel_;
        Windows::UI::Xaml::Visibility mediaVisibility_;
        Platform::String^ reply_;
        Platform::String^ comments_;
        Platform::Collections::Vector<ReactionVM^>^ reactions_;
        Windows::UI::Xaml::Media::Brush^ senderBrush_;
        Windows::UI::Xaml::Thickness bubbleMargin_;

        void RaisePropertyChanged(Platform::String^ name);
    };
}
