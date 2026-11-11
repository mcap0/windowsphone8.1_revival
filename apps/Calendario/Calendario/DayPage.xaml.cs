using System;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Calendario.Data;
using Calendario.Logic;
using Calendario.Models;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Calendario
{
    public sealed partial class DayPage : Page
    {
        private readonly ObservableCollection<AgendaRow> rows = new ObservableCollection<AgendaRow>();
        private DateTime selectedDate;
        private int loadVersion;

        public DayPage()
        {
            InitializeComponent();
            EventList.ItemsSource = rows;
            selectedDate = DateTime.Today;
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            if (e.Parameter is DateTime)
            {
                selectedDate = ((DateTime)e.Parameter).Date;
            }
            LoadDayAsync();
        }

        private void Page_Loaded(object sender, RoutedEventArgs e)
        {
            UpdateHeader();
        }

        private void UpdateHeader()
        {
            DayTitleText.Text = FullDate(selectedDate);
        }

        private static string FullDate(DateTime date)
        {
            return ItalianDates.WeekdayLong(date.DayOfWeek) + " " + date.Day + " "
                + ItalianDates.MonthLong(date.Month) + " " + date.Year;
        }

        private async Task LoadDayAsync()
        {
            int version = ++loadVersion;
            DateTime day = selectedDate.Date;
            UpdateHeader();
            StatusText.Text = "Caricamento...";
            rows.Clear();

            try
            {
                DateTime nextDay = day.AddDays(1);
                var events = await CalendarReader.LoadAsync(day, nextDay);
                if (version != loadVersion) return;

                var groups = AgendaBuilder.Group(events, day, day);
                if (groups.Count == 0)
                {
                    StatusText.Text = "Nessun evento";
                    return;
                }

                foreach (AgendaRow row in AgendaRow.Build(groups, DateTime.Today, false))
                {
                    rows.Add(row);
                }
                StatusText.Text = rows.Count == 1 ? "1 evento" : rows.Count + " eventi";
            }
            catch (Exception ex)
            {
                if (version == loadVersion) StatusText.Text = "Errore: " + ex.Message;
            }
        }

        private void PreviousDay_Click(object sender, RoutedEventArgs e)
        {
            selectedDate = selectedDate.AddDays(-1);
            LoadDayAsync();
        }

        private void NextDay_Click(object sender, RoutedEventArgs e)
        {
            selectedDate = selectedDate.AddDays(1);
            LoadDayAsync();
        }

        private void Add_Click(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(EditEventPage), selectedDate.Date);
        }

        private void EventList_ItemClick(object sender, ItemClickEventArgs e)
        {
            var row = e.ClickedItem as AgendaRow;
            if (row == null || row.Event == null) return;
            App.SelectedEvent = row.Event;
            Frame.Navigate(typeof(DetailPage));
        }
    }
}
