using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using Calendario.Data;
using Calendario.Logic;
using Calendario.Models;
using Windows.Storage;
using Windows.UI;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Navigation;
using Windows.UI.Xaml.Shapes;

namespace Calendario
{
    public sealed partial class MainPage : Page
    {
        private const int PastDaysAtStart = 7;
        private const int ForwardStepDays = 60;
        private const int PastStepDays = 7;
        private const int SwipeThreshold = 100;

        private static readonly SolidColorBrush TodayBrush = new SolidColorBrush(Color.FromArgb(0xFF, 0x00, 0x63, 0xB1));
        private static readonly SolidColorBrush SelectedBrush = new SolidColorBrush(Color.FromArgb(0x66, 0x00, 0x63, 0xB1));
        private static readonly SolidColorBrush EmptyBrush = new SolidColorBrush(Colors.Transparent);
        private static readonly SolidColorBrush DotBrush = new SolidColorBrush(Color.FromArgb(0xFF, 0x4D, 0xA6, 0xFF));

        private readonly ObservableCollection<AgendaRow> agendaRows = new ObservableCollection<AgendaRow>();
        private readonly ObservableCollection<AgendaRow> dayRows = new ObservableCollection<AgendaRow>();
        private readonly Border[] cells = new Border[MonthGrid.Cells];
        private readonly TextBlock[] cellNumbers = new TextBlock[MonthGrid.Cells];
        private readonly Ellipse[] cellDots = new Ellipse[MonthGrid.Cells];

        private List<EventItem> agendaEvents = new List<EventItem>();
        private DateTime agendaFrom;
        private DateTime agendaTo;
        private bool agendaLoaded;

        private DateTime monthStart;
        private DateTime selectedDay;
        private List<EventItem> monthEvents = new List<EventItem>();
        private HashSet<DateTime> monthDots = new HashSet<DateTime>();
        private bool monthLoaded;

        private bool busy;

        public MainPage()
        {
            this.InitializeComponent();
            AgendaList.ItemsSource = agendaRows;
            DayList.ItemsSource = dayRows;
            BuildMonthGrid();

            DateTime today = DateTime.Today;
            agendaFrom = today.AddDays(-PastDaysAtStart);
            agendaTo = today.AddDays(ForwardStepDays);
            monthStart = MonthGrid.MonthStart(today);
            selectedDay = today;
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            ShowLastCrash();

            if (e.NavigationMode == NavigationMode.Back)
            {
                agendaLoaded = false;
                monthLoaded = false;
                await ReloadAgendaAsync(null);
                fourLoaded = false;
                if (MainPivot.SelectedIndex == 1)
                {
                    await LoadMonthAsync();
                }
                else if (MainPivot.SelectedIndex == 2)
                {
                    await LoadFourAsync(false);
                }
            }
        }

        private async void Page_Loaded(object sender, RoutedEventArgs e)
        {
            if (!agendaLoaded) await ReloadAgendaAsync(null);
        }

        private void ShowLastCrash()
        {
            try
            {
                object text;
                if (ApplicationData.Current.LocalSettings.Values.TryGetValue(App.LastCrashKey, out text) && text != null)
                {
                    string s = text.ToString();
                    if (s.Length > 800) s = s.Substring(0, 800);
                    ErrorText.Text = "Errore precedente (tocca per nascondere):\n" + s;
                }
            }
            catch (Exception)
            {
            }
        }

        private void ErrorText_Tapped(object sender, TappedRoutedEventArgs e)
        {
            try { ApplicationData.Current.LocalSettings.Values.Remove(App.LastCrashKey); } catch (Exception) { }
            ErrorText.Text = "";
        }

        private void ShowError(Exception ex)
        {
            ErrorText.Text = "Errore: " + ex.Message;
        }

        private async Task ReloadAgendaAsync(string anchorKey)
        {
            if (busy) return;
            busy = true;
            try
            {
                StatusText.Text = "Caricamento...";
                List<EventItem> loaded = await CalendarReader.LoadAsync(agendaFrom, agendaTo);
                agendaEvents = loaded;
                List<AgendaRow> rows = AgendaRow.Build(AgendaBuilder.Group(agendaEvents, agendaFrom, agendaTo.AddDays(-1)), DateTime.Today, true);
                agendaRows.Clear();
                foreach (AgendaRow r in rows) agendaRows.Add(r);

                LiveTile.Update(agendaEvents, DateTime.Now);
                StatusText.Text = agendaEvents.Count + " eventi, aggiornato alle " + DateTime.Now.ToString("HH:mm");
                ErrorText.Text = "";
                agendaLoaded = true;

                ScrollAgenda(anchorKey ?? TodayAnchorKey(rows));
            }
            catch (Exception ex)
            {
                ShowError(ex);
            }
            finally
            {
                busy = false;
            }
        }

        private static string TodayAnchorKey(IList<AgendaRow> rows)
        {
            DateTime today = DateTime.Today;
            foreach (AgendaRow r in rows)
            {
                if (r.IsHeader && r.Date >= today) return r.Key;
            }
            return rows.Count > 0 ? rows[rows.Count - 1].Key : null;
        }

        private void ScrollAgenda(string key)
        {
            if (key == null) return;
            AgendaRow row = agendaRows.FirstOrDefault(r => r.Key == key);
            if (row == null) return;
            try { AgendaList.ScrollIntoView(row, ScrollIntoViewAlignment.Leading); } catch (Exception) { }
        }

        private async void Past_Click(object sender, RoutedEventArgs e)
        {
            if (busy) return;
            string anchor = agendaRows.Count > 0 ? agendaRows[0].Key : null;
            agendaFrom = agendaFrom.AddDays(-PastStepDays);
            await ReloadAgendaAsync(anchor);
        }

        private async void LoadMore_Click(object sender, RoutedEventArgs e)
        {
            if (busy) return;
            busy = true;
            try
            {
                DateTime oldTo = agendaTo;
                DateTime newTo = oldTo.AddDays(ForwardStepDays);
                List<EventItem> more = await CalendarReader.LoadAsync(oldTo, newTo);
                agendaEvents.AddRange(more);
                agendaTo = newTo;
                foreach (AgendaRow r in AgendaRow.Build(AgendaBuilder.Group(more, oldTo, newTo.AddDays(-1)), DateTime.Today, true))
                {
                    agendaRows.Add(r);
                }
                StatusText.Text = agendaEvents.Count + " eventi, fino al " + newTo.AddDays(-1).ToString("dd/MM/yyyy");
            }
            catch (Exception ex)
            {
                ShowError(ex);
            }
            finally
            {
                busy = false;
            }
        }

        private void AgendaList_ItemClick(object sender, ItemClickEventArgs e)
        {
            OpenDetail(e.ClickedItem as AgendaRow);
        }

        private void DayList_ItemClick(object sender, ItemClickEventArgs e)
        {
            OpenDetail(e.ClickedItem as AgendaRow);
        }

        private void OpenDetail(AgendaRow row)
        {
            if (row == null || row.Event == null) return;
            App.SelectedEvent = row.Event;
            Frame.Navigate(typeof(DetailPage));
        }

        private void BuildMonthGrid()
        {
            for (int r = 0; r < 6; r++)
            {
                MonthHost.RowDefinitions.Add(new RowDefinition { Height = new GridLength(56) });
            }
            for (int c = 0; c < 7; c++)
            {
                MonthHost.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            }
            for (int i = 0; i < MonthGrid.Cells; i++)
            {
                var number = new TextBlock { HorizontalAlignment = HorizontalAlignment.Center, FontSize = 20 };
                var dot = new Ellipse
                {
                    Width = 6,
                    Height = 6,
                    Margin = new Thickness(0, 2, 0, 0),
                    Fill = DotBrush,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    Visibility = Visibility.Collapsed
                };
                var stack = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
                stack.Children.Add(number);
                stack.Children.Add(dot);

                var border = new Border { Child = stack, Background = EmptyBrush, Margin = new Thickness(2) };
                border.Tapped += Cell_Tapped;
                Grid.SetRow(border, i / 7);
                Grid.SetColumn(border, i % 7);
                MonthHost.Children.Add(border);

                cells[i] = border;
                cellNumbers[i] = number;
                cellDots[i] = dot;
            }
        }

        private async Task LoadMonthAsync()
        {
            if (busy) return;
            busy = true;
            try
            {
                DateTime gridStart = MonthGrid.GridStart(monthStart.Year, monthStart.Month);
                DateTime gridEnd = gridStart.AddDays(MonthGrid.Cells);
                monthEvents = await CalendarReader.LoadAsync(gridStart, gridEnd);
                monthDots = AgendaBuilder.DaysWithEvents(monthEvents, gridStart, gridEnd.AddDays(-1));
                monthLoaded = true;
                UpdateMonthUi();
            }
            catch (Exception ex)
            {
                ShowError(ex);
            }
            finally
            {
                busy = false;
            }
        }

        private void UpdateMonthUi()
        {
            DateTime today = DateTime.Today;
            MonthTitleText.Text = ItalianDates.MonthTitle(monthStart.Year, monthStart.Month);

            DateTime[] days = MonthGrid.Build(monthStart.Year, monthStart.Month);
            for (int i = 0; i < MonthGrid.Cells; i++)
            {
                DateTime d = days[i];
                cells[i].Tag = d;
                cellNumbers[i].Text = d.Day.ToString();
                cellNumbers[i].Opacity = MonthGrid.InMonth(d, monthStart.Month) ? 1.0 : 0.35;
                cellNumbers[i].FontWeight = d == today ? Windows.UI.Text.FontWeights.Bold : Windows.UI.Text.FontWeights.Normal;
                cellDots[i].Visibility = monthDots.Contains(d) ? Visibility.Visible : Visibility.Collapsed;
                cells[i].Background = d == selectedDay ? SelectedBrush : (d == today ? TodayBrush : EmptyBrush);
            }

            dayRows.Clear();
            List<DayGroup> groups = AgendaBuilder.Group(monthEvents, selectedDay, selectedDay);
            string label = ItalianDates.DayLabel(selectedDay, today);
            if (groups.Count == 0)
            {
                dayRows.Add(AgendaRow.Header(selectedDay, label + ": nessun evento"));
            }
            else
            {
                foreach (AgendaRow r in AgendaRow.Build(groups, today, true)) dayRows.Add(r);
            }
        }

        private void Cell_Tapped(object sender, TappedRoutedEventArgs e)
        {
            var border = sender as Border;
            if (border == null || border.Tag == null) return;
            DateTime date = ((DateTime)border.Tag).Date;
            selectedDay = date;
            Frame.Navigate(typeof(DayPage), date);
        }

        private void ChangeMonth(int delta)
        {
            DateTime target = MonthGrid.AddMonths(monthStart, delta);
            DateTime today = DateTime.Today;
            monthStart = target;
            selectedDay = (today.Year == target.Year && today.Month == target.Month) ? today : target;
            monthLoaded = false;
            LoadMonthAsync();
        }

        private void PrevMonth_Click(object sender, RoutedEventArgs e) { ChangeMonth(-1); }

        private void NextMonth_Click(object sender, RoutedEventArgs e) { ChangeMonth(1); }

        private void MonthHost_ManipulationCompleted(object sender, ManipulationCompletedRoutedEventArgs e)
        {
            double dx = e.Cumulative.Translation.X;
            if (dx < -SwipeThreshold) ChangeMonth(1);
            else if (dx > SwipeThreshold) ChangeMonth(-1);
        }

        private void MainPivot_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (MainPivot.SelectedIndex == 1 && !monthLoaded) LoadMonthAsync();
            if (MainPivot.SelectedIndex == 2 && !fourLoaded) LoadFourAsync(true);
        }

        private void Add_Click(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(EditEventPage), DateTime.Today);
        }

        private async void Today_Click(object sender, RoutedEventArgs e)
        {
            if (MainPivot.SelectedIndex == 0)
            {
                ScrollAgenda(TodayAnchorKey(agendaRows));
                return;
            }
            if (MainPivot.SelectedIndex == 2)
            {
                fourStart = DateTime.Today;
                await LoadFourAsync(true);
                return;
            }
            DateTime today = DateTime.Today;
            monthStart = MonthGrid.MonthStart(today);
            selectedDay = today;
            monthLoaded = false;
            await LoadMonthAsync();
        }

        private async void Refresh_Click(object sender, RoutedEventArgs e)
        {
            if (MainPivot.SelectedIndex == 0)
            {
                await ReloadAgendaAsync(null);
            }
            else if (MainPivot.SelectedIndex == 2)
            {
                await LoadFourAsync(false);
            }
            else
            {
                monthLoaded = false;
                await LoadMonthAsync();
            }
        }
    }
}
