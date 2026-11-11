using System;
using Calendario.Data;
using Calendario.Logic;
using Windows.UI.Popups;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Calendario
{
    public sealed partial class DetailPage : Page
    {
        public DetailPage()
        {
            InitializeComponent();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            if (App.SelectedEvent == null)
            {
                if (Frame.CanGoBack) Frame.GoBack();
                else Frame.Navigate(typeof(MainPage));
                return;
            }
            RenderEvent();
        }

        private void Page_Loaded(object sender, RoutedEventArgs e)
        {
            RenderEvent();
        }

        private void RenderEvent()
        {
            EventItem ev = App.SelectedEvent;
            if (ev == null)
            {
                TitleText.Text = "Evento non disponibile";
                return;
            }

            TitleText.Text = ev.Title;
            WhenText.Text = ev.SpansDays
                ? "Dal " + FullDate(ev.FirstDay) + " al " + FullDate(ev.LastDay)
                : FullDate(ev.FirstDay);
            TimeText.Text = ev.AllDay ? "Tutto il giorno" : TimeFormat.Range(ev);
            LocationText.Text = string.IsNullOrWhiteSpace(ev.Location) ? "-" : ev.Location;
            CalendarText.Text = string.IsNullOrWhiteSpace(ev.CalendarName) ? "-" : ev.CalendarName;
            ReminderText.Text = TimeFormat.ReminderText(ev.Reminder);
            DetailsBox.Text = ev.Details ?? "";

            bool editable = ev.IsEditable;
            EditButton.Visibility = editable ? Visibility.Visible : Visibility.Collapsed;
            DeleteButton.Visibility = editable ? Visibility.Visible : Visibility.Collapsed;
            ReadOnlyNoteText.Text = editable ? "" : "Evento di Google: si modifica su Google";
            ReadOnlyNoteText.Visibility = editable ? Visibility.Collapsed : Visibility.Visible;
        }

        private static string FullDate(DateTime d)
        {
            return ItalianDates.WeekdayLong(d.DayOfWeek) + " " + d.Day + " "
                + ItalianDates.MonthLong(d.Month) + " " + d.Year;
        }

        private void Edit_Click(object sender, RoutedEventArgs e)
        {
            EventItem ev = App.SelectedEvent;
            if (ev == null || !ev.IsEditable) return;
            Frame.Navigate(typeof(EditEventPage));
        }

        private async void Delete_Click(object sender, RoutedEventArgs e)
        {
            EventItem ev = App.SelectedEvent;
            if (ev == null || !ev.IsEditable) return;

            var dialog = new MessageDialog("Vuoi eliminare questo evento?", "Elimina evento");
            dialog.Commands.Add(new UICommand("Elimina"));
            dialog.Commands.Add(new UICommand("Annulla"));
            dialog.DefaultCommandIndex = 1;
            dialog.CancelCommandIndex = 1;

            IUICommand result = await dialog.ShowAsync();
            if (result == null || result.Label != "Elimina") return;

            try
            {
                if (!await CalendarWriter.DeleteAsync(ev)) return;
                App.SelectedEvent = null;
                if (Frame.CanGoBack) Frame.GoBack();
                else Frame.Navigate(typeof(MainPage));
            }
            catch (Exception ex)
            {
                await new MessageDialog("Impossibile eliminare l'evento: " + ex.Message, "Errore").ShowAsync();
            }
        }
    }
}
