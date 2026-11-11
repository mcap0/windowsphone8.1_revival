// TileTask: every 30 minutes (TimeTrigger, with internet) downloads the home place's forecast and updates the
// Start tile.
#pragma once

namespace MeteoTask {

public ref class TileTask sealed : public Windows::ApplicationModel::Background::IBackgroundTask {
public:
    virtual void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ taskInstance);
};

}  // namespace MeteoTask
