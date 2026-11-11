using System;
using System.Collections.Generic;
using System.ComponentModel;
using Posta.Services;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Posta.Pages
{
    /// <summary>Browses C:\Data\Users\Public; tap = select/unselect (also across folders), "Allega" hands the
    /// chosen files back to ComposePage through ComposeRequest.</summary>
    public sealed partial class FilePage : Page
    {
        private string _dir = PublicFiles.Root;
        private readonly List<string> _selected = new List<string>();

        public FilePage()
        {
            InitializeComponent();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            _selected.Clear();
            Show(PublicFiles.Root);
        }

        /// <summary>Back key: goes up one folder; false at the root (the page closes).</summary>
        public bool HandleBack()
        {
            if (string.Equals(_dir, PublicFiles.Root, StringComparison.OrdinalIgnoreCase))
                return false;
            Show(_dir.Substring(0, _dir.LastIndexOf('\\')));
            return true;
        }

        private void Show(string dir)
        {
            _dir = dir;
            PathText.Text = dir;
            var items = new List<FileItem>();
            foreach (var entry in PublicFiles.List(dir))
                items.Add(new FileItem(entry, _selected.Contains(dir + @"\" + entry.Name)));
            Entries.ItemsSource = items;
            UpdateStatus();
        }

        private void Entries_ItemClick(object sender, ItemClickEventArgs e)
        {
            var item = e.ClickedItem as FileItem;
            if (item == null)
                return;
            var path = _dir + @"\" + item.Name;
            if (item.IsDir)
            {
                Show(path);
                return;
            }
            var on = !_selected.Remove(path);
            if (on)
                _selected.Add(path);
            item.Selected = on;
            UpdateStatus();
        }

        private void UpdateStatus()
        {
            SelectionStatus.Text = _selected.Count == 0 ? "tocca i file da allegare" : _selected.Count + " file selezionati";
        }

        private void Attach_Click(object sender, Windows.UI.Xaml.RoutedEventArgs e)
        {
            if (_selected.Count == 0)
                return;
            ComposeRequest.PendingAttachments = new List<string>(_selected);
            _selected.Clear();
            if (Frame.CanGoBack)
                Frame.GoBack();
        }

        public sealed class FileItem : INotifyPropertyChanged
        {
            private bool _selected;

            public FileItem(PublicFiles.Entry entry, bool selected)
            {
                Name = entry.Name;
                IsDir = entry.IsDir;
                _selected = selected;
                Detail = entry.IsDir ? "cartella" : entry.Size >= 1024 * 1024
                    ? (entry.Size / (1024.0 * 1024.0)).ToString("0.0") + " MB"
                    : ((entry.Size + 1023) / 1024) + " KB";
            }

            public string Name { get; private set; }
            public bool IsDir { get; private set; }
            public string Glyph { get { return IsDir ? "\U0001F4C1" : _selected ? "\u2611" : "\u2610"; } }

            public Windows.UI.Xaml.Media.Brush Highlight
            {
                get
                {
                    return new Windows.UI.Xaml.Media.SolidColorBrush(_selected
                        ? Windows.UI.Color.FromArgb(0x55, 0x00, 0x67, 0xC0) : Windows.UI.Colors.Transparent);
                }
            }

            public bool Selected
            {
                get { return _selected; }
                set
                {
                    _selected = value;
                    var changed = PropertyChanged;
                    if (changed == null)
                        return;
                    changed(this, new PropertyChangedEventArgs("Glyph"));
                    changed(this, new PropertyChangedEventArgs("Highlight"));
                }
            }

            public event PropertyChangedEventHandler PropertyChanged;
            public string Detail { get; private set; }
        }
    }
}
