#pragma once

#include "MainPage.g.h"
#include "lumiaweb.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Browser
{
    // One LW_EVENT_FIELD_FOCUS/UPDATE, copied on the engine thread (the strings are only valid inside the callback).
    struct FieldState
    {
        int kind = LW_FIELD_NONE;
        int selectionStart = 0;
        int selectionEnd = 0;
        int x = 0, y = 0, width = 0, height = 0;
        int enterAction = 0;
        int maxLength = -1;
        unsigned int sequence = 0;
        std::wstring text;
    };

    // One tab: its engine view and the panel the view draws into, or neither when the tab is discarded (memory) and
    // only its address and title remain, to load again when it is shown.
    struct Tab
    {
        int id = 0;
        lw_view* view = nullptr;
        Windows::UI::Xaml::Controls::SwapChainPanel^ panel;
        // The panel's independent input source (its thread runs until the tab is discarded).
        Windows::UI::Core::CoreIndependentInputSource^ input;
        std::wstring url;
        std::wstring title;
        bool canGoBack = false;
        bool canGoForward = false;
        bool loading = false;
        bool failed = false;
        int progress = 0;
        unsigned long long lastUsed = 0;
        // The tab a link opened this one from: Back on its first page goes back there, as on Android.
        int openerId = 0;
    };

    // A favorite or a history entry.
    struct PageEntry
    {
        std::wstring title;
        std::wstring url;
        long long time = 0; // history: FILETIME of the visit
    };

    // A file the engine saves (LW_EVENT_DOWNLOAD), or one an earlier run left in Downloads\Internet.
    struct DownloadEntry
    {
        int id = 0;
        std::wstring url;
        std::wstring path;
        std::wstring name;
        long long total = -1;
        long long received = 0;
        int state = 0; // 0 in corso, 1 completato, 2 non riuscito, 3 annullato
        std::wstring error;
        bool toPictures = false; // "salva immagine": moved into the phone's pictures when it arrives
    };

    public ref class MainPage sealed
    {
    public:
        MainPage();

    internal:
        // Engine events that change the UI, forwarded from the engine's threads with the view they belong to.
        void OnEngineEventOnUIThread(lw_view* view, int type, long long number, int id, const std::wstring& text,
                                     const std::wstring& text2);
        // A tile on Start or another app opened an address: a tab for it.
        void OpenFromLaunch(const std::wstring& url);
        void OnSiteIconLinks(const std::wstring& links);
        // The photo picker came back (App::OnActivated, PickFileContinuation): the page's <input type=file> gets them.
        void OnFilesPicked(Windows::Foundation::Collections::IVectorView<Windows::Storage::StorageFile^>^ files);
        // A field took/lost the focus or changed: the compose bar follows it. UI thread.
        void OnFieldEventOnUIThread(const FieldState& state, bool update);
        // Long press: the menu for a link or image, or straight the text panel for plain text. UI thread.
        void OnContextMenuOnUIThread(const std::wstring& link, const std::wstring& image, const std::wstring& text);
        // The Back list for the next start, saved at every suspension. UI thread.
        void SaveSession();
        void OnSiteResumed(unsigned long long awayMs);
        // The address box's Enter, also for text sent by the PC (tools/phonecmd).
        void RunAddressText(std::wstring text, bool fromPC);

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        void Address_KeyUp(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void HideKeyboard();
        void Address_GotFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Address_LostFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ClearAddress_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void SetLoadLine(bool visible, bool failed);
        void More_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void Favorites_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void Reload_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void TabsButton_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void Address_TextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void HardwareBack(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);

        // Tabs (see the tabs section of MainPage.xaml.cpp).
        std::shared_ptr<Tab> ActiveTab();
        std::shared_ptr<Tab> TabForView(lw_view* view);
        std::shared_ptr<Tab> NewTab(const std::wstring& url, int openerId, bool activate);
        void ActivateTab(const std::shared_ptr<Tab>& tab);
        void CloseTab(std::shared_ptr<Tab> tab);
        void EnsureLive(const std::shared_ptr<Tab>& tab);
        void OpenWindow(lw_view* opener, int id, const std::wstring& url);
        void Discard(const std::shared_ptr<Tab>& tab, const wchar_t* reason);
        void DiscardBeyond(size_t live);
        void UpdateBar();
        std::vector<std::shared_ptr<Tab>> m_tabs;
        std::shared_ptr<Tab> m_activeTab;
        int m_nextTabId;
        int m_viewWidth;
        int m_viewHeight;
        float m_viewScale;

        // Favorites and history (files in LocalFolder), the list page and the suggestions under the address.
        enum class ListKind { None, Favorites, History, Tabs, Downloads, Files, Picker };
        void LoadEntries();
        void SaveFavorites();
        void SaveHistory();
        void AddHistory(const std::wstring& url, const std::wstring& title);
        bool IsFavorite(const std::wstring& url);
        void ToggleFavorite(const std::wstring& url, const std::wstring& title);
        void ShowList(ListKind kind);
        void HideList();
        void AddListRow(const std::wstring& title, const std::wstring& detail, std::function<void()> open,
                        std::function<void()> hold, std::function<void()> close);
        void ListTitle1_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void ListTitle2_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void ListAction_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void ListClose_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void UpdateSuggestions();
        std::vector<PageEntry> m_favorites;
        std::vector<PageEntry> m_historyEntries;
        bool m_historyDirty;
        ListKind m_listKind;

        // Dialogs: the page's (LW_EVENT_DIALOG) and the app's own questions, on one panel.
        void ShowPageDialog(lw_view* view, int id, int kind, const std::wstring& message, const std::wstring& value);
        void ShowAppDialog(const std::wstring& title, const std::wstring& message, const std::wstring& value,
                           const wchar_t* ok, std::function<void(bool, const std::wstring&)> done);
        void CloseDialog(bool accepted);
        void DialogOk_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void DialogCancel_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void DialogInput_KeyUp(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void DialogPassword_KeyUp(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void ShowPasswordQuestion(lw_view* view, int id, int refused, const std::wstring& server, const std::wstring& realm);
        lw_view* m_dialogView;
        int m_dialogId;
        bool m_dialogHasInput;
        std::function<void(bool, const std::wstring&)> m_dialogDone;
        void ReplaceDialog();
        bool m_dialogReplaced;
        // What pages ask of the phone: permission questions, the position, notifications, fullscreen.
        void ShowPermissionQuestion(lw_view* view, int id, int kind, const std::wstring& site);
        void WatchPosition(int accuracy);
        void ShowPageNotification(lw_view* view, int id, bool show, const std::wstring& title, const std::wstring& body);
        void ShowFullscreen(lw_view* view);
        lw_view* m_fullscreenView;
        Windows::Devices::Geolocation::Geolocator^ m_geolocator;
        Windows::Foundation::EventRegistrationToken m_positionToken;
        Windows::Foundation::EventRegistrationToken m_positionStatusToken;
        std::map<int, Windows::UI::Notifications::ToastNotification^> m_toasts;

        // Find in page.
        void ShowFind();
        void HideFind();
        void FindText_TextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void FindText_KeyUp(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void FindPrevious_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void FindNext_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void FindClose_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void Find(int direction);
        int m_findId;
        int m_findMatches;
        int m_findIndex;
        int m_findDirection;

        // Mobile or desktop version per site (lw_set_site_user_agent).
        bool IsDesktopSite(const std::wstring& url);
        void ToggleDesktopSite(const std::wstring& url);
        std::vector<std::wstring> m_desktopSites;
        // "ua <name>": site -> identity name (MainPage.xaml.cpp, identities).
        void SetSiteIdentity(const std::wstring& url, const std::wstring& name);
        std::vector<std::pair<std::wstring, std::wstring>> m_siteIdentities;
        void PinToStart(const std::wstring& url, const std::wstring& title);
        void AddSiteIconChoice(const std::wstring& file, unsigned int background);
        void LoadSiteIcons(std::shared_ptr<std::vector<std::wstring>> links, size_t next, int generation);
        void ApplyTile(const std::wstring& logo, unsigned int background); // background: 0xAARRGGBB
        void CloseIconPicker();
        void IconPickerClose_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        std::wstring m_pinURL;
        std::wstring m_pinName;
        std::wstring m_pinId;
        int m_iconGeneration;
        int m_siteIcons;

        // Downloads (see the downloads section of MainPage.xaml.cpp).
        void OnDownloadEvent(int type, long long number, int id, const std::wstring& text, const std::wstring& text2);
        void LoadDownloads();
        void RefreshDownloads();
        void ShowDownloadMenu(const DownloadEntry& entry);
        void OpenFile(const std::wstring& path);
        void ShareFile(const std::wstring& path);
        void CopyToLibrary(const std::wstring& path, const wchar_t* folder, const wchar_t* label);
        void RemoveDownload(const DownloadEntry& entry, bool deleteFile);
        std::vector<DownloadEntry> m_downloads;
        bool m_downloadsLoaded;
        unsigned long long m_downloadsRefreshed;
        std::wstring m_shareFile;
        // "Salva immagine": addresses asked for, whose download goes into the phone's pictures when it arrives.
        std::vector<std::wstring> m_imageSaves;
        // The page's <input type=file>: photos through the system picker, any other file from our own list of the
        // phone's folders (the WP8.1 pickers only show photos).
        void PickFiles(lw_view* view, int id, bool multiple, const std::wstring& accept);
        void PickPhotos();
        void ShowFolder(const std::wstring& folder);
        void AnswerPick(const std::vector<std::wstring>& paths);
        lw_view* m_pickView;
        int m_pickId;
        bool m_pickMultiple;
        std::vector<std::wstring> m_pickExtensions;
        std::wstring m_pickFolder;
        std::vector<std::wstring> m_pickSelected;
        // The page's form pickers (LW_EVENT_PICKER): <select> and colours on the list page, dates and times with the
        // phone's own pickers. One at a time, for the tab on screen.
        void ShowPicker(lw_view* view, int id, int kind, const std::wstring& text, const std::wstring& text2);
        void AnswerPicker(const wchar_t* value);
        void ShowDatePicker(const std::wstring& type, const std::wstring& value);
        Windows::UI::Xaml::Controls::Grid^ AddPickerRow(const std::wstring& text, const std::wstring& detail, int swatch,
                                                        bool enabled, bool selected, std::function<void()> chosen);
        lw_view* m_pickerView;
        int m_pickerId;
        // A notice over the bottom of the page; a tap on it runs its action.
        void ShowNotice(const std::wstring& text, std::function<void()> action);
        void Notice_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        std::function<void()> m_noticeAction;
        Windows::UI::Xaml::DispatcherTimer^ m_noticeTimer;
        // Long-press menu and text panel.
        void AddMenuItem(const wchar_t* label, std::function<void()> action);
        void ShowSheet(const std::wstring& text, bool link);
        void Share(const std::wstring& text, bool link);
        void Menu_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void SheetShare_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void SheetClose_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void ShareRequested(Windows::ApplicationModel::DataTransfer::DataTransferManager^ sender,
                            Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^ e);
        std::wstring m_sheetText;
        bool m_sheetIsLink;
        std::wstring m_shareText;
        bool m_shareIsLink;
        void Panel_SizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);
        void FlushLog(Platform::Object^ sender, Platform::Object^ e);

        // Compose bar (phase 3.d): the native text box above the keyboard. Every keystroke is rendered here by XAML,
        // on the UI thread; the engine only receives the resulting edit, posted on the Web thread's input lane.
        void Field_TextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void Field_KeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void Field_LostFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Secret_PasswordChanged(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Secret_KeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void Send_Click(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        // What Enter does in the bar, shared by the keyboard's key and the Invio border: newline in a multiline field,
        // a real Enter keydown (implicit submit, page handlers) anywhere else.
        void BarEnter();
        void InputPane_Showing(Windows::UI::ViewManagement::InputPane^ sender,
                               Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e);
        void InputPane_Hiding(Windows::UI::ViewManagement::InputPane^ sender,
                              Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e);
        void ShowCompose(const FieldState& state);
        void HideCompose(bool notifyEngine, const wchar_t* reason);
        // Sends the difference between the bar's text and the text the engine last reported, as one lw_text_edit.
        void SendFieldEdit();
        void SendFieldEditFor(const std::wstring& now, bool secret);

        // Touch (see the touch section of MainPage.xaml.cpp): the panel's own input thread, XAML events as fallback.
        void StartInput(Windows::UI::Xaml::Controls::SwapChainPanel^ panel);
        void StartXamlInput(Windows::UI::Xaml::Controls::SwapChainPanel^ panel);
        void Input_PointerPressed(Platform::Object^ sender, Windows::UI::Core::PointerEventArgs^ e);
        void Input_PointerMoved(Platform::Object^ sender, Windows::UI::Core::PointerEventArgs^ e);
        void Input_PointerReleased(Platform::Object^ sender, Windows::UI::Core::PointerEventArgs^ e);
        void Input_PointerCaptureLost(Platform::Object^ sender, Windows::UI::Core::PointerEventArgs^ e);
        void Panel_PointerPressed(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void Panel_PointerMoved(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void Panel_PointerReleased(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void Panel_PointerCanceled(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);

        // ---- site apps (Site.h): bar without address, PIN vault (Vault.h), login filled ----
        void ApplySiteMode();
        void ShowLock(const wchar_t* message);
        void HideLock();
        void LockOther_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void LockNoPin_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void LockNext_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void BuildKeypad();
        void LockKey(wchar_t key);
        void ShowLockStep(int step, const wchar_t* message);
        void UpdatePinDots();
        void SetupVault(const std::wstring& pin);
        void UnlockVault();
        void ForgetCredentials();
        bool IsSiteURL(const std::wstring& url);
        void FillLogin(lw_view* view, const std::wstring& url);
        std::wstring m_siteUser;      // in memory only while unlocked
        std::wstring m_sitePassword;
        bool m_unlocked;
        bool m_lockBusy;
        bool m_lockSetup;
        bool m_changingPin;           // PIN added or changed from the menu: credentials already open
        int m_lockStep;               // 0 credentials, 1 choose PIN, 2 repeat PIN, 3 unlock
        std::wstring m_pinEntry;      // digits typed on the keypad
        std::wstring m_pinFirst;      // the PIN of step 1, to compare in step 2
        int m_lockFailures;
        unsigned long long m_lockWaitUntil;

        bool StartEngine();
        void CreateView(int width, int height);
        void Navigate(const std::wstring& text);
        void BenchStart();
        void BenchTick(Platform::Object^ sender, Platform::Object^ args);
        void StayAwakeForPC();
        void StayAwakeTick(Platform::Object^ sender, Platform::Object^ args);
        void RestoreSession();

        // The active tab's view (null while it is being created): the compose bar, the menu and the commands use it.
        lw_view* m_view;
        std::wstring m_currentURL;
        bool m_tracing;
        Windows::UI::Xaml::DispatcherTimer^ m_benchTimer;
        Windows::System::Display::DisplayRequest^ m_benchDisplay;
        int m_benchStep;
        int m_benchLoads;
        ULONGLONG m_benchAt;
        bool m_benchResting;
        Windows::UI::Xaml::DispatcherTimer^ m_awakeTimer;
        Windows::System::Display::DisplayRequest^ m_awakeDisplay;
        bool m_awake;
        std::wstring m_pendingLaunchURL;
        bool m_selectAddressOnKeyboard;
        Windows::UI::Xaml::DispatcherTimer^ m_logTimer;


        // Compose bar state. m_field is the text and selection the engine last reported (the base of the next diff);
        // m_syncField is set while the bar is updated from the engine, so TextChanged does not echo the same text back.
        FieldState m_field;
        unsigned int m_textSequence;
        bool m_syncField;
        bool m_composeVisible;
        int m_barKind;
        Windows::UI::ViewManagement::InputPane^ m_inputPane;
        Windows::Foundation::EventRegistrationToken m_inputShowingToken;
        Windows::Foundation::EventRegistrationToken m_inputHidingToken;
    };
}
