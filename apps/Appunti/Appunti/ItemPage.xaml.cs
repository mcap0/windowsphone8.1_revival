using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Appunti
{
    public sealed partial class ItemPage : Page
    {
        // Parameter is the text of an existing item, or null for "Nuovo testo".
        private string _original;
        private bool _isNew;

        public ItemPage()
        {
            this.InitializeComponent();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            _original = e.Parameter as string;
            _isNew = _original == null;
            Editor.Text = _original ?? "";
            base.OnNavigatedTo(e);
        }

        // Focus and select everything so the keyboard opens with the Copy icon in its suggestion bar.
        private async void Page_Loaded(object sender, RoutedEventArgs e)
        {
            // Wait for the page transition to finish, otherwise focus is lost and the keyboard stays closed.
            await System.Threading.Tasks.Task.Delay(300);
            Editor.Focus(FocusState.Programmatic);
            Editor.SelectAll();
        }

        protected override async void OnNavigatedFrom(NavigationEventArgs e)
        {
            base.OnNavigatedFrom(e);
            string text = Editor.Text;
            if (!string.IsNullOrWhiteSpace(text) && (_isNew || text != _original))
            {
                await ClipStore.AddAsync(text);
            }
        }
    }
}
