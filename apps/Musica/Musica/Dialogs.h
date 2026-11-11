#pragma once

#include <functional>
#include <ppltasks.h>

namespace Musica
{
    // Name prompt for rename/new: calls done(name) only on "salva" with a non-empty name.
    inline void AskName(Platform::String^ title, Platform::String^ current,
                        std::function<void(Platform::String^)> done)
    {
        using namespace Windows::UI::Xaml::Controls;
        auto dialog = ref new ContentDialog();
        dialog->Title = title;
        dialog->PrimaryButtonText = "salva";
        dialog->SecondaryButtonText = "annulla";
        auto box = ref new TextBox();
        box->Text = current == nullptr ? "" : current;
        box->SelectAll();
        dialog->Content = box;
        concurrency::create_task(dialog->ShowAsync()).then([box, done](ContentDialogResult result)
        {
            if (result == ContentDialogResult::Primary && box->Text != nullptr && box->Text->Length() > 0)
                done(box->Text);
        }, concurrency::task_continuation_context::use_current());
    }
}
