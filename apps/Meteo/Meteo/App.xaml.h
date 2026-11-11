#pragma once

#include "App.g.h"

namespace Meteo {

ref class App sealed {
public:
    App();
    virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;

protected:
    virtual void OnActivated(Windows::ApplicationModel::Activation::IActivatedEventArgs^ e) override;

private:
    void Show(Platform::String^ uri);
    void OnBackPressed(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);
    void RegisterTileTask();
};

}  // namespace Meteo
