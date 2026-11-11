#pragma once

// Selection of several songs in a list (brani, playlist, album, artist): the page's command bar is swapped for one with
// tutti / aggiungi a playlist / elimina, plus sposta / rimuovi inside a playlist. Back leaves the selection.

#include <functional>
#include <ppltasks.h>
#include "Services.h"

namespace Musica
{
    // Back key: when set and returning true, App::OnBackPressed does not navigate back.
    inline std::function<bool()>& BackHook()
    {
        static std::function<bool()> hook;
        return hook;
    }

    inline Platform::String^ SongCount(unsigned int n)
    {
        return n == 1 ? ref new Platform::String(L"1 brano") : n.ToString() + " brani";
    }

    // Asks, then deletes the files; done() runs after the deletion (also on errors).
    inline void ConfirmDeleteSongs(Platform::String^ what,
                                   Windows::Foundation::Collections::IVector<Song^>^ songs,
                                   std::function<void()> done)
    {
        using namespace Windows::UI::Popups;
        using namespace concurrency;
        auto paths = ref new Platform::Collections::Vector<Platform::String^>();
        for (unsigned int i = 0; i < songs->Size; ++i)
            paths->Append(songs->GetAt(i)->Path);
        auto dialog = ref new MessageDialog("Eliminare " + what + " dal telefono?");
        dialog->Commands->Append(ref new UICommand("elimina"));
        dialog->Commands->Append(ref new UICommand("annulla"));
        dialog->DefaultCommandIndex = 1;
        dialog->CancelCommandIndex = 1;
        create_task(dialog->ShowAsync()).then([paths, done](IUICommand^ command)
        {
            if (command == nullptr || command->Label != "elimina")
                return;
            create_task(MusicService::DeleteSongsAsync(paths)).then([done](task<unsigned int> result)
            {
                Platform::String^ problem = nullptr;
                try
                {
                    unsigned int failed = result.get();
                    if (failed > 0)
                        problem = "Impossibile eliminare " + SongCount(failed) + ".";
                }
                catch (Platform::Exception^)
                {
                    problem = "Errore durante l'eliminazione dei brani.";
                }
                if (done)
                    done();
                if (problem != nullptr)
                    create_task((ref new MessageDialog(problem))->ShowAsync());
            }, task_continuation_context::use_current());
        }, task_continuation_context::use_current());
    }

    // Playlist chooser (existing ones except `exclude`, or a new name); done(name) on confirmation.
    inline void PickPlaylist(Platform::String^ title, Platform::String^ action, Platform::String^ exclude,
                             std::function<void(Platform::String^)> done)
    {
        using namespace Windows::UI::Xaml;
        using namespace Windows::UI::Xaml::Controls;
        using namespace concurrency;
        auto dialog = ref new ContentDialog();
        dialog->Title = title;
        dialog->PrimaryButtonText = action;
        dialog->SecondaryButtonText = "annulla";

        auto names = ref new Platform::Collections::Vector<Platform::String^>();
        for (unsigned int i = 0; i < MusicService::GetPlaylists()->Size; ++i)
        {
            Platform::String^ name = MusicService::GetPlaylists()->GetAt(i)->Name;
            if (exclude == nullptr || name != exclude)
                names->Append(name);
        }
        auto panel = ref new StackPanel();
        auto list = ref new ListView();
        list->ItemsSource = names;
        list->SelectionMode = ListViewSelectionMode::Single;
        list->MaxHeight = 260;
        list->ItemTemplate = safe_cast<DataTemplate^>(Markup::XamlReader::Load(
            "<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\">"
            "<TextBlock Text=\"{Binding}\" FontSize=\"22\" Margin=\"8,6\"/></DataTemplate>"));
        auto nameBox = ref new TextBox();
        nameBox->PlaceholderText = "oppure nuova playlist";
        panel->Children->Append(list);
        panel->Children->Append(nameBox);
        dialog->Content = panel;
        create_task(dialog->ShowAsync()).then([list, nameBox, done](ContentDialogResult result)
        {
            if (result != ContentDialogResult::Primary)
                return;
            Platform::String^ name = nameBox->Text;
            if (name != nullptr && name->Length() > 0)
            {
                bool exists = false;
                for (unsigned int i = 0; i < MusicService::GetPlaylists()->Size; ++i)
                    if (MusicService::GetPlaylists()->GetAt(i)->Name == name)
                        exists = true;
                if (!exists)
                    MusicService::CreatePlaylist(name);
                done(name);
                return;
            }
            Platform::String^ chosen = dynamic_cast<Platform::String^>(list->SelectedItem);
            if (chosen != nullptr)
                done(chosen);
        }, task_continuation_context::use_current());
    }

    ref class SongSelection sealed
    {
    internal:
        // playlist: non-null inside a playlist page (adds sposta / rimuovi); changed: after songs left the list
        SongSelection(Windows::UI::Xaml::Controls::Page^ page, Windows::UI::Xaml::Controls::ListView^ list,
                      PlaylistInfo^ playlist, std::function<void()> changed)
            : _page(page), _list(list), _playlist(playlist), _changed(changed), _saved(nullptr), _bar(nullptr),
              _active(false), _clickWas(true),
              _modeWas(Windows::UI::Xaml::Controls::ListViewSelectionMode::None)
        {
            using namespace Windows::UI::Xaml::Controls;
            Platform::WeakReference weak(this);
            _list->SelectionChanged += ref new SelectionChangedEventHandler(
                [weak](Platform::Object^, SelectionChangedEventArgs^)
                {
                    SongSelection^ self = weak.Resolve<SongSelection>();
                    if (self != nullptr)
                        self->Update();
                });
        }

        bool Active() { return _active; }

        void Enter(Song^ first)
        {
            using namespace Windows::UI::Xaml::Controls;
            if (!_active)
            {
                _active = true;
                _clickWas = _list->IsItemClickEnabled;
                _modeWas = _list->SelectionMode;
                _list->IsItemClickEnabled = false;   // a tap ticks the song instead of playing it
                _list->SelectionMode = ListViewSelectionMode::Multiple;
                _saved = _page->BottomAppBar;
                _page->BottomAppBar = Bar();
                Platform::WeakReference weak(this);
                BackHook() = [weak]()
                {
                    SongSelection^ self = weak.Resolve<SongSelection>();
                    if (self == nullptr || !self->Active())
                        return false;
                    self->Exit();
                    return true;
                };
            }
            if (first != nullptr)
                _list->SelectedItems->Append(first);
            Update();
        }

        void Exit()
        {
            using namespace Windows::UI::Xaml::Controls;
            if (!_active)
                return;
            _active = false;
            _list->SelectedItems->Clear();
            _list->SelectionMode = _modeWas;
            _list->IsItemClickEnabled = _clickWas;
            _page->BottomAppBar = _saved;
            _saved = nullptr;
            BackHook() = nullptr;
        }

    private:
        Windows::UI::Xaml::Controls::Page^ _page;
        Windows::UI::Xaml::Controls::ListView^ _list;
        PlaylistInfo^ _playlist;
        std::function<void()> _changed;
        Windows::UI::Xaml::Controls::AppBar^ _saved;
        Windows::UI::Xaml::Controls::CommandBar^ _bar;
        Windows::UI::Xaml::Controls::AppBarButton^ _all;
        Windows::UI::Xaml::Controls::AppBarButton^ _add;
        Windows::UI::Xaml::Controls::AppBarButton^ _delete;
        Windows::UI::Xaml::Controls::AppBarButton^ _move;
        Windows::UI::Xaml::Controls::AppBarButton^ _remove;
        bool _active;
        bool _clickWas;
        Windows::UI::Xaml::Controls::ListViewSelectionMode _modeWas;

        Windows::Foundation::Collections::IVector<Song^>^ Selected()
        {
            auto songs = ref new Platform::Collections::Vector<Song^>();
            for (unsigned int i = 0; i < _list->SelectedItems->Size; ++i)
            {
                Song^ song = dynamic_cast<Song^>(_list->SelectedItems->GetAt(i));
                if (song != nullptr)
                    songs->Append(song);
            }
            return songs;
        }

        unsigned int Total()
        {
            auto items = dynamic_cast<Windows::Foundation::Collections::IVector<Song^>^>(_list->ItemsSource);
            return items == nullptr ? 0 : items->Size;
        }

        void Update()
        {
            if (!_active || _bar == nullptr)
                return;
            unsigned int n = _list->SelectedItems->Size;
            bool any = n > 0;
            _add->IsEnabled = any;
            _delete->IsEnabled = any;
            if (_move != nullptr) _move->IsEnabled = any;
            if (_remove != nullptr) _remove->IsEnabled = any;
            bool all = n > 0 && n >= Total();
            _all->Label = all ? "nessuno" : "tutti";
            _delete->Label = any ? "elimina " + n.ToString() : ref new Platform::String(L"elimina");
        }

        Windows::UI::Xaml::Controls::AppBarButton^ Button(Platform::String^ label,
                                                           Windows::UI::Xaml::Controls::Symbol icon,
                                                           std::function<void(SongSelection^)> action)
        {
            using namespace Windows::UI::Xaml;
            using namespace Windows::UI::Xaml::Controls;
            auto b = ref new AppBarButton();
            b->Label = label;
            b->Icon = ref new SymbolIcon(icon);
            Platform::WeakReference weak(this);
            b->Click += ref new RoutedEventHandler([weak, action](Platform::Object^, RoutedEventArgs^)
            {
                SongSelection^ self = weak.Resolve<SongSelection>();
                if (self != nullptr && self->Active())
                    action(self);
            });
            return b;
        }

        Windows::UI::Xaml::Controls::CommandBar^ Bar()
        {
            using namespace Windows::UI::Xaml::Controls;
            if (_bar != nullptr)
                return _bar;
            _bar = ref new CommandBar();
            _bar->IsSticky = true;
            _all = Button("tutti", Symbol::SelectAll, [](SongSelection^ s) { s->ToggleAll(); });
            _add = Button("a playlist", Symbol::Add, [](SongSelection^ s) { s->AddToPlaylist(); });
            _delete = Button("elimina", Symbol::Delete, [](SongSelection^ s) { s->Delete(); });
            _move = nullptr;
            _remove = nullptr;
            if (_playlist != nullptr)
            {
                _move = Button("sposta", Symbol::Forward, [](SongSelection^ s) { s->Move(); });
                _remove = Button("rimuovi", Symbol::Remove, [](SongSelection^ s) { s->RemoveFromPlaylist(); });
                _bar->PrimaryCommands->Append(_move);
                _bar->PrimaryCommands->Append(_remove);
                _bar->PrimaryCommands->Append(_add);
                _bar->PrimaryCommands->Append(_all);
                // the fifth button does not fit the phone's bar: delete from the phone goes to the "..." menu
                _bar->SecondaryCommands->Append(_delete);
            }
            else
            {
                _bar->PrimaryCommands->Append(_add);
                _bar->PrimaryCommands->Append(_delete);
                _bar->PrimaryCommands->Append(_all);
            }
            return _bar;
        }

        void ToggleAll()
        {
            if (_list->SelectedItems->Size > 0 && _list->SelectedItems->Size >= Total())
                _list->SelectedItems->Clear();
            else
                _list->SelectAll();
            Update();
        }

        void AddToPlaylist()
        {
            auto songs = Selected();
            if (songs->Size == 0)
                return;
            Platform::WeakReference weak(this);
            PickPlaylist("Aggiungi " + SongCount(songs->Size), "aggiungi",
                         _playlist == nullptr ? nullptr : _playlist->Name, [weak, songs](Platform::String^ name)
            {
                MusicService::AddSongsToPlaylist(name, songs);
                SongSelection^ self = weak.Resolve<SongSelection>();
                if (self != nullptr)
                    self->Exit();
            });
        }

        void Move()
        {
            auto songs = Selected();
            if (songs->Size == 0 || _playlist == nullptr)
                return;
            Platform::WeakReference weak(this);
            PlaylistInfo^ from = _playlist;
            PickPlaylist("Sposta " + SongCount(songs->Size), "sposta", from->Name,
                         [weak, songs, from](Platform::String^ name)
            {
                MusicService::AddSongsToPlaylist(name, songs);
                SongSelection^ self = weak.Resolve<SongSelection>();
                if (self != nullptr)
                    self->Exit();
                for (unsigned int i = 0; i < songs->Size; ++i)
                    MusicService::RemoveFromPlaylist(from, songs->GetAt(i));
                if (self != nullptr && self->_changed)
                    self->_changed();
            });
        }

        void RemoveFromPlaylist()
        {
            auto songs = Selected();
            if (songs->Size == 0 || _playlist == nullptr)
                return;
            PlaylistInfo^ from = _playlist;
            Exit();
            for (unsigned int i = 0; i < songs->Size; ++i)
                MusicService::RemoveFromPlaylist(from, songs->GetAt(i));
            if (_changed)
                _changed();
        }

        void Delete()
        {
            auto songs = Selected();
            if (songs->Size == 0)
                return;
            Platform::WeakReference weak(this);
            ConfirmDeleteSongs(SongCount(songs->Size), songs, [weak]()
            {
                SongSelection^ self = weak.Resolve<SongSelection>();
                if (self == nullptr)
                    return;
                self->Exit();
                if (self->_changed)
                    self->_changed();
            });
        }
    };
}
