#pragma once

#include "App.g.h"

namespace Musica
{
    ref class App sealed
    {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
        virtual void OnActivated(Windows::ApplicationModel::Activation::IActivatedEventArgs^ e) override;
        virtual void OnFileActivated(Windows::ApplicationModel::Activation::FileActivatedEventArgs^ e) override;

    private:
        Windows::UI::Xaml::Controls::Frame^ EnsureFrame(Platform::String^ arguments);
        void RunWhenLibraryReady(Windows::Foundation::Uri^ uri, int triesLeft);
        void ReopenWhenLibraryReady(Platform::String^ page, Platform::String^ arg, int triesLeft);
        void OnFrameNavigated(Platform::Object^ sender, Windows::UI::Xaml::Navigation::NavigationEventArgs^ e);
        void PlayFileWhenLibraryReady(Platform::String^ name, int triesLeft, Platform::String^ path = nullptr);
        void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ e);
        void OnResuming(Platform::Object^ sender, Platform::Object^ e);
        void OnBackPressed(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);
        void OnUnhandled(Platform::Object^ sender, Windows::UI::Xaml::UnhandledExceptionEventArgs^ e);
    };
}
