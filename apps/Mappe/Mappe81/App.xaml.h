// App.xaml.h: application entry point.
#pragma once

#include "App.g.h"

namespace Mappe81 {

ref class App sealed {
public:
    App();
    virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;

private:
    void OnUnhandled(Platform::Object^ sender, Windows::UI::Xaml::UnhandledExceptionEventArgs^ e);
};

}
