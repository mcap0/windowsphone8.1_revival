#define NOMINMAX
#include "Watch.h"
#include "Shared.h"
#include "MailPoll.h"

#include <windows.h>
#include <collection.h>
#include <ppltasks.h>
#include <string>

using namespace PostaTask;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;

// Every 15 minutes (posta-timer) and when the internet comes back (posta-rete), also with the phone locked: one mail
// check, with toasts for new unread mail. Without a network the check waits for posta-rete.
void Watch::Run(IBackgroundTaskInstance^ taskInstance)
{
    BackgroundTaskDeferral^ deferral = taskInstance->GetDeferral();
    SetToastContext(L"watch");
    std::wstring name = taskInstance->Task != nullptr ? taskInstance->Task->Name->Data() : L"?";
    std::wstring network = NetworkText();
    std::wstring line = L"watch (" + name + L"): batteria " + std::to_wstring(BatteryPercent()) + L"%, rete " + network;
    if (network == L"nessuna")
    {
        line += L"; posta: niente rete, controllo al ritorno della rete";
    }
    else
    {
        std::wstring mail = PollMail();
        SetCheck(mail);
        line += L"; " + mail;
    }
    Log(line);
    WaitInbox(4000);
    deferral->Complete();
}

// After an app update WP8.1 needs RemoveAccess + RequestAccess, otherwise the old registrations never fire again.
void Checks::Register()
{
    IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
    PackageVersion v = Package::Current->Id->Version;
    String^ version = v.Major.ToString() + "." + v.Minor.ToString() + "." + v.Build.ToString() + "." + v.Revision.ToString();
    String^ registered = values->HasKey("bgVersion") ? dynamic_cast<String^>(values->Lookup("bgVersion")) : nullptr;
    if (registered != version)
    {
        BackgroundExecutionManager::RemoveAccess();
        for (auto pair : BackgroundTaskRegistration::AllTasks) pair->Value->Unregister(true);
    }
    concurrency::create_task(BackgroundExecutionManager::RequestAccessAsync()).then([values, version](BackgroundAccessStatus status)
    {
        if (status == BackgroundAccessStatus::Denied || status == BackgroundAccessStatus::Unspecified)
        {
            Log(L"app: accesso in background negato (" + std::to_wstring(static_cast<int>(status)) + L")");
            return;
        }
        bool timer = false, network = false;
        for (auto pair : BackgroundTaskRegistration::AllTasks)
        {
            if (pair->Value->Name == "posta-timer") timer = true;
            if (pair->Value->Name == "posta-rete") network = true;
        }
        if (!timer)
        {
            BackgroundTaskBuilder^ builder = ref new BackgroundTaskBuilder();
            builder->Name = "posta-timer";
            builder->TaskEntryPoint = "PostaTask.Watch";
            builder->SetTrigger(ref new TimeTrigger(15, false));
            builder->Register();
        }
        if (!network)
        {
            BackgroundTaskBuilder^ builder = ref new BackgroundTaskBuilder();
            builder->Name = "posta-rete";
            builder->TaskEntryPoint = "PostaTask.Watch";
            builder->SetTrigger(ref new SystemTrigger(SystemTriggerType::InternetAvailable, false));
            builder->Register();
        }
        values->Insert("bgVersion", version);
        if (!timer || !network) Log(L"app: controlli in background registrati (" + std::to_wstring(static_cast<int>(status)) + L")");
    }).then([](concurrency::task<void> t)
    {
        try { t.get(); }
        catch (Exception^ ex) { Log(L"app: registrazione fallita " + Hex(ex->HResult)); }
    });

    // First launch with this feature: inbox baseline now (no toasts), off the UI thread.
    if (!values->HasKey("mailNext"))
    {
        concurrency::create_task([]
        {
            Log(L"app: " + PollMail());
        });
    }
}
