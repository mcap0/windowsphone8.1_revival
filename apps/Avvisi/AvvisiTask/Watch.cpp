#define NOMINMAX
#include "Watch.h"
#include "Shared.h"
#include "MailPoll.h"
#include "TgPoll.h"

#include <windows.h>
#include <string>

using namespace AvvisiTask;
using namespace Platform;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;

namespace
{
    const long long NagEvery = 2 * 3600 * 10000000LL;   // "Avvisi fermo" at most every 2 hours
}

// Every 30 min and when the network comes back. WP8.1 does not let a background task restart the audio task, so
// when KeepAlive is not running the checks are done here (every 30 min instead of 2) and a toast asks to reopen Avvisi.
void Watch::Run(IBackgroundTaskInstance^ taskInstance)
{
    BackgroundTaskDeferral^ deferral = taskInstance->GetDeferral();
    SetToastContext(L"watch");
    std::wstring name = taskInstance->Task != nullptr ? taskInstance->Task->Name->Data() : L"?";
    long long age = -1, nagAt = 0;
    std::wstring state;
    try
    {
        IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
        if (values->HasKey("last")) age = (NowUtc() - safe_cast<long long>(values->Lookup("last"))) / 600000000LL;
        if (values->HasKey("nagAt")) nagAt = safe_cast<long long>(values->Lookup("nagAt"));
        String^ s = values->HasKey("state") ? dynamic_cast<String^>(values->Lookup("state")) : nullptr;
        if (s != nullptr) state = s->Data();
    }
    catch (...) { }
    std::wstring line = L"watch (" + name + L"): ultimo tick " + (age < 0 ? std::wstring(L"mai") : std::to_wstring(age) + L" min fa") +
        L", batteria " + std::to_wstring(BatteryPercent()) + L"%, rete " + NetworkText();

    bool stoppedByUser = state.find(L"fermato dall'app") != std::wstring::npos;
    if ((age < 0 || age > 5) && !stoppedByUser && NetworkText() != L"nessuna")
    {
        std::wstring telegram = PollTelegram();
        std::wstring mail = PollMail();
        SetChecks(telegram, mail);
        line += L"; " + telegram + L"; " + mail;
        if (NowUtc() - nagAt > NagEvery)
        {
            Toast(L"Avvisi fermo", L"tocca per riattivare i controlli ogni 2 minuti", true, L"start");
            try { ApplicationData::Current->LocalSettings->Values->Insert("nagAt", NowUtc()); } catch (...) { }
        }
    }
    Log(line);
    deferral->Complete();
}
