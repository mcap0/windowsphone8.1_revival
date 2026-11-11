using System;
using System.Linq;
using Claude.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Claude
{
    public sealed partial class SettingsPage : Page
    {
        public SettingsPage()
        {
            InitializeComponent();
            ModelBox.ItemsSource = ClaudeModel.All;
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            KeyBox.Password = Settings.ApiKey ?? "";
            ModelBox.SelectedItem = ClaudeModel.All.FirstOrDefault(m => m.Id == Settings.Model) ?? ClaudeModel.All[0];
            SearchSwitch.IsOn = Settings.WebSearch;
            SystemBox.Text = Settings.SystemPrompt;
        }

        void OnSave(object sender, RoutedEventArgs e)
        {
            Settings.ApiKey = KeyBox.Password;
            if (ModelBox.SelectedItem is ClaudeModel model) Settings.Model = model.Id;
            Settings.WebSearch = SearchSwitch.IsOn;
            Settings.SystemPrompt = SystemBox.Text;
            if (Frame.CanGoBack) Frame.GoBack();
        }

        void OnDefaultSystem(object sender, RoutedEventArgs e) => SystemBox.Text = Settings.DefaultSystem;
    }
}
