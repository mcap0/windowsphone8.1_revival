#pragma once

#include "App.g.h"

namespace Risultati
{
    ref class App sealed
    {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
        virtual void OnActivated(Windows::ApplicationModel::Activation::IActivatedEventArgs^ e) override;

    private:
        void Show(Platform::String^ args);
        void OnUnhandled(Platform::Object^ sender, Windows::UI::Xaml::UnhandledExceptionEventArgs^ e);
    };
}
