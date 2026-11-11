using System;
using Calendario.Data;
using Calendario.Logic;
using Windows.UI.Popups;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Calendario
{
    public sealed partial class EditEventPage : Page
    {
        private EventItem eventBeingEdited;
        private bool isEditMode;

        public EditEventPage()
        {
            InitializeComponent();

            DateTime now = DateTime.Now;
            DateTime nextHour = now.Date.AddHours(Math.Floor(now.TimeOfDay.TotalHours) + 1.0);
            DatePickerControl.Date = new DateTimeOffset(nextHour.Date);
            StartTimePicker.Time = nextHour.TimeOfDay;
            EndTimePicker.Time = nextHour.AddHours(1).TimeOfDay;
            ReminderBox.SelectedIndex = 3;
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);

            if (e.Parameter is DateTime)
            {
                isEditMode = false;
                eventBeingEdited = null;
                SetDate(((DateTime)e.Parameter).Date);
                PageTitleText.Text = "Nuovo evento";
                DeleteButton.Visibility = Visibility.Collapsed;
                return;
            }

            EventItem selected = App.SelectedEvent;
            if (selected != null && selected.IsEditable)
            {
                isEditMode = true;
                eventBeingEdited = selected;
                PageTitleText.Text = "Modifica evento";
                DeleteButton.Visibility = Visibility.Visible;
                LoadEvent(selected);
            }
            else
            {
                isEditMode = false;
                eventBeingEdited = null;
                PageTitleText.Text = "Nuovo evento";
                DeleteButton.Visibility = Visibility.Collapsed;
            }
        }

        private void SetDate(DateTime date)
        {
            DatePickerControl.Date = new DateTimeOffset(date.Date);
        }

        private void LoadEvent(EventItem ev)
        {
            TitleBox.Text = ev.Title ?? "";
            LocationBox.Text = ev.Location ?? "";
            NotesBox.Text = ev.Details ?? "";
            SetDate(ev.Start.Date);
            AllDayToggle.IsOn = ev.AllDay;
            StartTimePicker.Time = ev.Start.TimeOfDay;
            EndTimePicker.Time = ev.End.TimeOfDay;
            ReminderBox.SelectedIndex = ReminderIndex(ev.Reminder);
            UpdateTimePickerState();
        }

        private static int ReminderIndex(TimeSpan? reminder)
        {
            if (!reminder.HasValue) return 0;
            if (reminder.Value == TimeSpan.Zero) return 1;
            if (reminder.Value == TimeSpan.FromMinutes(5)) return 2;
            if (reminder.Value == TimeSpan.FromMinutes(15)) return 3;
            if (reminder.Value == TimeSpan.FromMinutes(30)) return 4;
            if (reminder.Value == TimeSpan.FromHours(1)) return 5;
            if (reminder.Value == TimeSpan.FromDays(1)) return 6;
            return 3;
        }

        private static TimeSpan? ReminderValue(int index)
        {
            switch (index)
            {
                case 1: return TimeSpan.Zero;
                case 2: return TimeSpan.FromMinutes(5);
                case 3: return TimeSpan.FromMinutes(15);
                case 4: return TimeSpan.FromMinutes(30);
                case 5: return TimeSpan.FromHours(1);
                case 6: return TimeSpan.FromDays(1);
                default: return null;
            }
        }

        private void AllDayToggle_Toggled(object sender, RoutedEventArgs e)
        {
            UpdateTimePickerState();
        }

        private void UpdateTimePickerState()
        {
            bool enabled = !AllDayToggle.IsOn;
            StartTimePicker.IsEnabled = enabled;
            EndTimePicker.IsEnabled = enabled;
        }

        private async void Save_Click(object sender, RoutedEventArgs e)
        {
            string title = (TitleBox.Text ?? "").Trim();
            if (title.Length == 0)
            {
                ErrorText.Text = "Inserisci un titolo.";
                TitleBox.Focus(FocusState.Programmatic);
                return;
            }

            try
            {
                DateTime date = DatePickerControl.Date.DateTime.Date;
                bool allDay = AllDayToggle.IsOn;
                DateTime start;
                TimeSpan duration;

                if (allDay)
                {
                    start = DateTime.SpecifyKind(date, DateTimeKind.Local);
                    duration = TimeSpan.FromDays(1);
                }
                else
                {
                    start = DateTime.SpecifyKind(date.Add(StartTimePicker.Time), DateTimeKind.Local);
                    DateTime end = DateTime.SpecifyKind(date.Add(EndTimePicker.Time), DateTimeKind.Local);
                    if (end <= start) end = end.AddDays(1);
                    duration = end - start;
                }

                TimeSpan? reminder = ReminderValue(ReminderBox.SelectedIndex);
                EventItem saved = await CalendarWriter.SaveAsync(
                    eventBeingEdited,
                    title,
                    start,
                    duration,
                    allDay,
                    LocationBox.Text ?? "",
                    NotesBox.Text ?? "",
                    reminder);

                if (isEditMode)
                {
                    App.SelectedEvent = saved;
                }

                if (Frame.CanGoBack) Frame.GoBack();
                else Frame.Navigate(typeof(MainPage));
            }
            catch (Exception ex)
            {
                ErrorText.Text = "Impossibile salvare l'evento: " + ex.Message;
            }
        }

        private void Cancel_Click(object sender, RoutedEventArgs e)
        {
            if (Frame.CanGoBack) Frame.GoBack();
        }

        private async void Delete_Click(object sender, RoutedEventArgs e)
        {
            if (!isEditMode || eventBeingEdited == null) return;

            var dialog = new MessageDialog("Vuoi eliminare questo evento?", "Elimina evento");
            dialog.Commands.Add(new UICommand("Elimina"));
            dialog.Commands.Add(new UICommand("Annulla"));
            dialog.DefaultCommandIndex = 1;
            dialog.CancelCommandIndex = 1;

            IUICommand result = await dialog.ShowAsync();
            if (result == null || result.Label != "Elimina") return;

            try
            {
                await CalendarWriter.DeleteAsync(eventBeingEdited);
                App.SelectedEvent = null;
                if (Frame.CanGoBack) Frame.GoBack();
                else Frame.Navigate(typeof(MainPage));
            }
            catch (Exception ex)
            {
                ErrorText.Text = "Impossibile eliminare l'evento: " + ex.Message;
            }
        }
    }
}
