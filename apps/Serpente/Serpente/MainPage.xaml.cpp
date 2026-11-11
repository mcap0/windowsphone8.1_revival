#include "pch.h"
#include "MainPage.xaml.h"

using namespace Serpente;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Shapes;

namespace
{
    const int Cols = 16;           // the board is 16 cells wide; the rows fill the screen
    const double SwipeMin = 24.0;  // pixels before a drag counts as a turn
}

MainPage::MainPage() : state_(Ready), cell_(0), best_(0), swipeTaken_(false)
{
    InitializeComponent();
    Windows::UI::ViewManagement::StatusBar::GetForCurrentView()->HideAsync();
    parts_ = ref new Platform::Collections::Vector<Rectangle^>();
    bodyBrush_ = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x43, 0xA0, 0x47));
    headBrush_ = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x8B, 0xC3, 0x4A));
    auto values = ApplicationData::Current->LocalSettings->Values;
    if (values->HasKey(L"best")) best_ = safe_cast<int>(values->Lookup(L"best"));
    timer_ = ref new DispatcherTimer();
    timer_->Tick += ref new EventHandler<Object^>(this, &MainPage::OnTick);
    Area->SizeChanged += ref new SizeChangedEventHandler(this, &MainPage::OnSizeChanged);
    Root->Tapped += ref new TappedEventHandler(this, &MainPage::OnTapped);
    Root->ManipulationDelta += ref new ManipulationDeltaEventHandler(this, &MainPage::OnManipulationDelta);
    Root->ManipulationCompleted += ref new ManipulationCompletedEventHandler(this, &MainPage::OnManipulationCompleted);
    ShowScore();
}

void MainPage::OnSizeChanged(Object^, SizeChangedEventArgs^ e)
{
    if (game_ != nullptr || e->NewSize.Width <= 0) return;  // built once: the size does not change (portrait only)
    Build(e->NewSize.Width, e->NewSize.Height);
}

void MainPage::Build(double width, double height)
{
    cell_ = std::floor((width - 2) / Cols);
    int rows = static_cast<int>((height - 2) / cell_);
    BoardFrame->Width = Cols * cell_ + 2;
    BoardFrame->Height = rows * cell_ + 2;
    Board->Width = Cols * cell_;
    Board->Height = rows * cell_;
    game_.reset(new Game(Cols, rows, static_cast<uint32_t>(GetTickCount64())));
    food_ = Square(ref new SolidColorBrush(ColorHelper::FromArgb(255, 0xE5, 0x39, 0x35)));
    Board->Children->Append(food_);
    Draw();
    SetState(Ready);
}

Rectangle^ MainPage::Square(Brush^ brush)
{
    auto r = ref new Rectangle();
    r->Width = cell_ - 2;   // a dark line between the squares
    r->Height = cell_ - 2;
    r->Fill = brush;
    return r;
}

void MainPage::Start()
{
    game_->Reset();
    ShowScore();
    Draw();
    Resume();
}

void MainPage::Resume()
{
    TimeSpan t;
    t.Duration = game_->Interval() * 10000LL;
    timer_->Interval = t;
    timer_->Start();
    if (awake_ == nullptr)
    {
        awake_ = ref new Windows::System::Display::DisplayRequest();
        awake_->RequestActive();
    }
    SetState(Playing);
}

void MainPage::Pause()
{
    if (state_ != Playing) return;
    timer_->Stop();
    SetState(Paused);
}

bool MainPage::HandleBack()
{
    if (state_ != Playing) return false;  // paused or over: back leaves the game
    Pause();
    return true;
}

void MainPage::SetState(State s)
{
    state_ = s;
    if (s != Playing && awake_ != nullptr)
    {
        awake_->RequestRelease();
        awake_ = nullptr;
    }
    switch (s)
    {
    case Ready: Message->Text = L"Scorri per girare\ntocca per iniziare"; break;
    case Playing: Message->Text = L""; break;
    case Paused: Message->Text = L"In pausa\ntocca per continuare"; break;
    case Over:
    {
        std::wstring m = game_->Full() ? L"Hai riempito tutto!" : L"Fine";
        m += L"\n" + std::to_wstring(game_->Score()) + L" punti\ntocca per ricominciare";
        Message->Text = ref new String(m.c_str());
    }
        break;
    }
}

void MainPage::OnTick(Object^, Object^)
{
    if (state_ != Playing) return;
    bool ate = game_->Step();
    if (game_->Over())
    {
        timer_->Stop();
        if (game_->Score() > best_)
        {
            best_ = game_->Score();
            ApplicationData::Current->LocalSettings->Values->Insert(L"best", best_);
        }
        ShowScore();
        Draw();
        SetState(Over);
        return;
    }
    if (ate)
    {
        ShowScore();
        TimeSpan t;
        t.Duration = game_->Interval() * 10000LL;
        timer_->Interval = t;
    }
    Draw();
}

// one square per cell of the snake, reused from step to step
void MainPage::Draw()
{
    const auto& body = game_->Body();
    while (parts_->Size < body.size())
    {
        auto r = Square(bodyBrush_);
        parts_->Append(r);
        Board->Children->Append(r);
    }
    for (unsigned i = 0; i < parts_->Size; i++)
    {
        Rectangle^ r = parts_->GetAt(i);
        if (i >= body.size()) { r->Visibility = Windows::UI::Xaml::Visibility::Collapsed; continue; }
        r->Visibility = Windows::UI::Xaml::Visibility::Visible;
        r->Fill = i == 0 ? headBrush_ : bodyBrush_;
        Canvas::SetLeft(r, body[i].x * cell_ + 1);
        Canvas::SetTop(r, body[i].y * cell_ + 1);
    }
    Cell f = game_->Food();
    food_->Visibility = f.x < 0 ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
    Canvas::SetLeft(food_, f.x * cell_ + 1);
    Canvas::SetTop(food_, f.y * cell_ + 1);
}

void MainPage::ShowScore()
{
    ScoreText->Text = game_ == nullptr ? L"0" : game_->Score().ToString();
    BestText->Text = L"record " + best_.ToString();
}

void MainPage::OnTapped(Object^, TappedRoutedEventArgs^)
{
    if (game_ == nullptr) return;
    switch (state_)
    {
    case Ready: Resume(); break;
    case Over: Start(); break;
    case Paused: Resume(); break;
    case Playing: Pause(); break;
    }
}

// a drag turns as soon as it is long enough (not at the lift: quicker), one turn per finger
void MainPage::OnManipulationDelta(Object^, ManipulationDeltaRoutedEventArgs^ e)
{
    if (swipeTaken_ || game_ == nullptr) return;
    double dx = e->Cumulative.Translation.X, dy = e->Cumulative.Translation.Y;
    if (std::fabs(dx) < SwipeMin && std::fabs(dy) < SwipeMin) return;
    swipeTaken_ = true;
    Dir d = std::fabs(dx) > std::fabs(dy) ? (dx > 0 ? Right : Left) : (dy > 0 ? Down : Up);
    if (state_ == Ready || state_ == Paused) { game_->Turn(d); Resume(); return; }
    if (state_ == Over) return;
    game_->Turn(d);
}

void MainPage::OnManipulationCompleted(Object^, ManipulationCompletedRoutedEventArgs^)
{
    swipeTaken_ = false;
}
