// App.xaml.h: started from the tile, from "assistente:" (long press of Search) or again while open.
#pragma once

#include "App.g.h"

namespace Assistente {

ref class App sealed {
public:
    App();
    virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
    virtual void OnActivated(Windows::ApplicationModel::Activation::IActivatedEventArgs^ e) override;

private:
    void Show(Platform::String^ reason);
    void OnUnhandled(Platform::Object^ sender, Windows::UI::Xaml::UnhandledExceptionEventArgs^ e);
    void OnResuming(Platform::Object^ sender, Platform::Object^ e);
};

}
