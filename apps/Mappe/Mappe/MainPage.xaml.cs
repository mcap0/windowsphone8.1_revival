using System;
using System.Collections.Generic;
using System.Text;
using Mappe.Core;
using Windows.Devices.Geolocation;
using Windows.Foundation;
using Windows.System;
using Windows.UI;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Maps;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Navigation;
using Windows.UI.Xaml.Shapes;
using Windows.Storage;

namespace Mappe
{
    public sealed partial class MainPage : Page
    {
        private const double ItalyLat = 41.9;
        private const double ItalyLon = 12.5;
        private const double ItalyZoom = 6;
        private const string KeyLat = "map.lat";
        private const string KeyLon = "map.lon";
        private const string KeyZoom = "map.zoom";
        private const string AppLinkScheme = "metrosurf";

        private TileService _tiles;
        private readonly SearchDebouncer _debounce = new SearchDebouncer(400, 3);
        private readonly DispatcherTimer _timer = new DispatcherTimer();
        private bool _initialized;
        private int _tick;
        private int _searchSeq;
        private int _reverseSeq;
        private Geolocator _geo;
        private Geoposition _lastPos;
        private UIElement _posDot;
        private UIElement _searchPin;
        private UIElement _pressPin;
        private double? _pressLat;
        private double? _pressLon;
        private string _savedKey = string.Empty;

        // Diagnostics
        private string _loadingStatus = "-";
        private string _geoStatus = "-";
        private string _geoError = "-";
        private string _searchInfo = "-";
        private string _reverseInfo = "-";

        public MainPage()
        {
            this.InitializeComponent();
            try
            {
                _tiles = new TileService(ReadTileMode());
            }
            catch (Exception)
            {
                _tiles = new TileService(TileService.ModeCustom);
            }
        }

        // Shows the last unhandled exception (saved by App) at the top of the page.
        public void ShowCrash()
        {
            var values = ApplicationData.Current.LocalSettings.Values;
            if (!values.ContainsKey(App.CrashKey)) return;
            SetSearchText("Errore: " + values[App.CrashKey]);
        }

        private static string ReadTileMode()
        {
            var values = ApplicationData.Current.LocalSettings.Values;
            if (values.ContainsKey("tiles.mode") && values["tiles.mode"] is string)
                return (string)values["tiles.mode"];
            return TileService.ModeHttp;
        }

        private void ToggleTiles_Click(object sender, RoutedEventArgs e)
        {
            string next = _tiles.Mode == TileService.ModeHttp ? TileService.ModeCustom : TileService.ModeHttp;
            ApplicationData.Current.LocalSettings.Values["tiles.mode"] = next;
            MapView.TileSources.Clear();
            _tiles = new TileService(next);
            InitTiles();
            SetSearchText("Tile in modalita' " + next + ". Se la mappa non si vede, prova l'altra modalita'.");
            RefreshDiag();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            if (_initialized) return;
            _initialized = true;

            ShowCrash();
            try { RestoreView(); } catch (Exception ex) { SetSearchText("Vista non ripristinata: " + ex.Message); }
            InitTiles();
            StartTimer();
            InitLocation();
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            SaveView(true);
            base.OnNavigatedFrom(e);
        }

        // ---- Map and tiles ---------------------------------------------------------

        private void InitTiles()
        {
            try
            {
                var source = new MapTileSource(_tiles.DataSource);
                source.IsTransparencyEnabled = false;
                source.Layer = MapTileLayer.BackgroundReplacement;
                source.ZIndex = 1;
                MapView.TileSources.Add(source);
                MapView.Style = MapStyle.None;
            }
            catch (Exception ex)
            {
                SetGeoText("Mappa non inizializzata: " + TileService.Describe(ex));
            }
        }

        private void RestoreView()
        {
            double lat = ItalyLat, lon = ItalyLon, zoom = ItalyZoom;
            var values = ApplicationData.Current.LocalSettings.Values;
            if (values.ContainsKey(KeyLat) && values.ContainsKey(KeyLon) && values.ContainsKey(KeyZoom) &&
                values[KeyLat] is double && values[KeyLon] is double && values[KeyZoom] is double)
            {
                lat = (double)values[KeyLat];
                lon = (double)values[KeyLon];
                zoom = (double)values[KeyZoom];
            }
            MapView.Center = new Geopoint(new BasicGeoposition { Latitude = lat, Longitude = lon });
            MapView.ZoomLevel = zoom;
        }

        private void SaveView(bool force)
        {
            try
            {
                Geopoint center = MapView.Center;
                if (center == null) return;
                double lat = center.Position.Latitude;
                double lon = center.Position.Longitude;
                double zoom = MapView.ZoomLevel;
                string key = GeoUrls.Num(lat) + "|" + GeoUrls.Num(lon) + "|" + GeoUrls.Num(zoom);
                if (!force && key == _savedKey) return;
                var values = ApplicationData.Current.LocalSettings.Values;
                values[KeyLat] = lat;
                values[KeyLon] = lon;
                values[KeyZoom] = zoom;
                _savedKey = key;
            }
            catch (Exception)
            {
                // Remembering the view is a convenience; never let it break the map.
            }
        }

        // ---- Timer: debounce, status polling, diagnostics refresh, view saving -----

        private void StartTimer()
        {
            _timer.Interval = TimeSpan.FromMilliseconds(200);
            _timer.Tick += Timer_Tick;
            _timer.Start();
        }

        private void Timer_Tick(object sender, object e)
        {
            _tick++;
            try
            {
                string query;
                if (_debounce.TryFire(NowMs(), out query)) RunSearch(query);

                _loadingStatus = MapView.LoadingStatus.ToString();
                if (_tick % 15 == 0) SaveView(false);
                if (DiagPanel.Visibility == Visibility.Visible && _tick % 5 == 0) RefreshDiag();
            }
            catch (Exception ex)
            {
                _loadingStatus = "errore timer: " + TileService.Describe(ex);
            }
        }

        private static long NowMs()
        {
            return DateTime.UtcNow.Ticks / TimeSpan.TicksPerMillisecond;
        }

        // ---- Location ----------------------------------------------------------------

        private void InitLocation()
        {
            try
            {
                // WP8.1 has no RequestAccessAsync: the 'location' capability in the manifest is enough.
                _geo = new Geolocator
                {
                    DesiredAccuracy = PositionAccuracy.High,
                    ReportInterval = 2000,
                    MovementThreshold = 5
                };
                _geo.PositionChanged += Geo_PositionChanged;
                _geo.StatusChanged += Geo_StatusChanged;
                ShowGeoStatus(_geo.LocationStatus);
            }
            catch (Exception ex)
            {
                _geoError = TileService.Describe(ex);
                SetGeoText("Posizione non disponibile: " + ex.Message);
            }
        }

        private void Geo_PositionChanged(Geolocator sender, PositionChangedEventArgs args)
        {
            Geoposition pos = args.Position;
            var ignored = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => ShowPosition(pos));
        }

        private void Geo_StatusChanged(Geolocator sender, StatusChangedEventArgs args)
        {
            PositionStatus status = args.Status;
            var ignored = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => ShowGeoStatus(status));
        }

        private void ShowPosition(Geoposition pos)
        {
            _lastPos = pos;
            _geoStatus = "Ready";
            var point = new Geopoint(new BasicGeoposition
            {
                Latitude = pos.Coordinate.Point.Position.Latitude,
                Longitude = pos.Coordinate.Point.Position.Longitude
            });
            if (_posDot == null)
            {
                var dot = new Ellipse
                {
                    Width = 18,
                    Height = 18,
                    Fill = new SolidColorBrush(Colors.DodgerBlue),
                    Stroke = new SolidColorBrush(Colors.White),
                    StrokeThickness = 3
                };
                _posDot = dot;
                MapView.Children.Add(dot);
                MapControl.SetNormalizedAnchorPoint(dot, new Point(0.5, 0.5));
            }
            MapControl.SetLocation(_posDot, point);
            SetGeoText(null);
        }

        private void ShowGeoStatus(PositionStatus status)
        {
            _geoStatus = status.ToString();
            switch (status)
            {
                case PositionStatus.Disabled:
                    SetGeoText("Attiva la posizione nelle Impostazioni");
                    break;
                case PositionStatus.NotAvailable:
                    SetGeoText("GPS non disponibile su questo dispositivo");
                    break;
                case PositionStatus.NoData:
                case PositionStatus.Initializing:
                case PositionStatus.NotInitialized:
                    SetGeoText("Cerco la posizione...");
                    break;
                default:
                    SetGeoText(null);
                    break;
            }
        }

        private void Locate_Click(object sender, RoutedEventArgs e)
        {
            if (_lastPos == null)
            {
                SetGeoText(_geo == null
                    ? "Posizione non disponibile. Attiva la posizione nelle Impostazioni."
                    : "Posizione non ancora disponibile, riprova tra poco.");
                return;
            }
            MapView.Center = new Geopoint(new BasicGeoposition
            {
                Latitude = _lastPos.Coordinate.Point.Position.Latitude,
                Longitude = _lastPos.Coordinate.Point.Position.Longitude
            });
            MapView.ZoomLevel = 16;
        }

        private void SetGeoText(string text)
        {
            GeoText.Text = text ?? string.Empty;
            GeoText.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
        }

        // ---- Search ----------------------------------------------------------------

        private void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            _debounce.Touch(SearchBox.Text, NowMs());
        }

        private void SearchBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key != VirtualKey.Enter) return;
            e.Handled = true;
            _debounce.Cancel();
            _debounce.ResetLast();
            RunSearch(SearchBox.Text.Trim());
            MapView.Focus(FocusState.Programmatic);
        }

        private void SearchFocus_Click(object sender, RoutedEventArgs e)
        {
            SearchBox.Focus(FocusState.Programmatic);
        }

        private async void RunSearch(string query)
        {
            int seq = ++_searchSeq;
            if (string.IsNullOrWhiteSpace(query))
            {
                ResultsList.ItemsSource = null;
                ResultsList.Visibility = Visibility.Collapsed;
                SetSearchText(null);
                return;
            }

            SetSearchText("Cerco \"" + query + "\"...");
            try
            {
                double? biasLat = null;
                double? biasLon = null;
                if (_lastPos != null)
                {
                    biasLat = _lastPos.Coordinate.Point.Position.Latitude;
                    biasLon = _lastPos.Coordinate.Point.Position.Longitude;
                }
                string json = await Net.GetStringAsync(GeoUrls.Search(query, 8, "it", biasLat, biasLon));
                if (seq != _searchSeq) return;

                List<PlaceResult> results = PhotonParser.Parse(json);
                _searchInfo = "ok, " + results.Count + " risultati";
                ResultsList.ItemsSource = results;
                ResultsList.Visibility = results.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
                SetSearchText(results.Count > 0 ? results.Count + " risultati" : "Nessun risultato");
            }
            catch (Exception ex)
            {
                if (seq != _searchSeq) return;
                _searchInfo = "errore: " + TileService.Describe(ex);
                ResultsList.Visibility = Visibility.Collapsed;
                SetSearchText("Ricerca non riuscita: " + ex.Message);
            }
        }

        private void ResultsList_ItemClick(object sender, ItemClickEventArgs e)
        {
            var place = e.ClickedItem as PlaceResult;
            if (place == null) return;

            var point = new Geopoint(new BasicGeoposition { Latitude = place.Lat, Longitude = place.Lon });
            MapView.Center = point;
            MapView.ZoomLevel = 17;
            ShowSearchPin(place, point);
            ResultsList.Visibility = Visibility.Collapsed;
            SetSearchText(place.Title);
        }

        private void ShowSearchPin(PlaceResult place, Geopoint point)
        {
            if (_searchPin != null) MapView.Children.Remove(_searchPin);

            var stack = new StackPanel();
            stack.Children.Add(new Border
            {
                Background = new SolidColorBrush(new Color { A = 0xE0, R = 0, G = 0, B = 0 }),
                Padding = new Thickness(6, 2, 6, 2),
                Child = new TextBlock
                {
                    Text = place.Title,
                    Foreground = new SolidColorBrush(Colors.White),
                    FontSize = 14,
                    MaxWidth = 240,
                    TextWrapping = TextWrapping.Wrap
                }
            });
            stack.Children.Add(new Ellipse
            {
                Width = 14,
                Height = 14,
                Fill = new SolidColorBrush(Colors.Red),
                Stroke = new SolidColorBrush(Colors.White),
                StrokeThickness = 2,
                HorizontalAlignment = HorizontalAlignment.Center
            });
            MapControl.SetNormalizedAnchorPoint(stack, new Point(0.5, 1.0));
            MapControl.SetLocation(stack, point);
            MapView.Children.Add(stack);
            _searchPin = stack;
        }

        private void SetSearchText(string text)
        {
            StatusText.Text = text ?? string.Empty;
            StatusText.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
        }

        // ---- Long press: reverse geocoding ----------------------------------------------

        private void MapView_MapHolding(MapControl sender, MapInputEventArgs args)
        {
            Geopoint point = args.Location;
            if (point == null) return;
            double lat = point.Position.Latitude;
            double lon = point.Position.Longitude;
            _pressLat = lat;
            _pressLon = lon;

            if (_pressPin != null) MapView.Children.Remove(_pressPin);
            var dot = new Ellipse
            {
                Width = 12,
                Height = 12,
                Fill = new SolidColorBrush(Colors.Orange),
                Stroke = new SolidColorBrush(Colors.White),
                StrokeThickness = 2
            };
            MapControl.SetNormalizedAnchorPoint(dot, new Point(0.5, 0.5));
            MapControl.SetLocation(dot, point);
            MapView.Children.Add(dot);
            _pressPin = dot;

            CoordBox.Text = GeoUrls.Num(lat) + ", " + GeoUrls.Num(lon);
            AddressText.Text = "Cerco l'indirizzo...";
            PressPanel.Visibility = Visibility.Visible;
            ReverseGeocodeAsync(lat, lon);
        }

        private async void ReverseGeocodeAsync(double lat, double lon)
        {
            int seq = ++_reverseSeq;
            try
            {
                string json = await Net.GetStringAsync(GeoUrls.Reverse(lat, lon, "it"));
                if (seq != _reverseSeq) return;
                List<PlaceResult> results = PhotonParser.Parse(json);
                if (results.Count == 0)
                {
                    _reverseInfo = "nessun risultato";
                    AddressText.Text = "Nessun indirizzo trovato";
                    return;
                }
                _reverseInfo = "ok";
                AddressText.Text = results[0].Title + "\n" + results[0].Subtitle;
            }
            catch (Exception ex)
            {
                if (seq != _reverseSeq) return;
                _reverseInfo = "errore: " + TileService.Describe(ex);
                AddressText.Text = "Indirizzo non disponibile: " + ex.Message;
            }
        }

        private void CopyCoords_Click(object sender, RoutedEventArgs e)
        {
            CoordBox.Focus(FocusState.Programmatic);
            CoordBox.SelectAll();
        }

        private async void OpenBrowser_Click(object sender, RoutedEventArgs e)
        {
            if (_pressLat == null || _pressLon == null) return;
            double lat = _pressLat.Value;
            double lon = _pressLon.Value;
            try
            {
                bool ok = await Launcher.LaunchUriAsync(new Uri(GeoUrls.OsmAppLink(lat, lon, 18, AppLinkScheme)));
                if (!ok)
                    await Launcher.LaunchUriAsync(new Uri(GeoUrls.OsmAppLink(lat, lon, 18, "https")));
            }
            catch (Exception ex)
            {
                AddressText.Text = "Impossibile aprire il browser: " + ex.Message;
            }
        }

        private void ClosePress_Click(object sender, RoutedEventArgs e)
        {
            PressPanel.Visibility = Visibility.Collapsed;
            if (_pressPin != null) MapView.Children.Remove(_pressPin);
            _pressPin = null;
        }

        // ---- Diagnostics -----------------------------------------------------------

        private void Diag_Click(object sender, RoutedEventArgs e)
        {
            DiagPanel.Visibility = Visibility.Visible;
            RefreshDiag();
        }

        private void Diag_Refresh_Click(object sender, RoutedEventArgs e)
        {
            RefreshDiag();
        }

        private void Diag_Close_Click(object sender, RoutedEventArgs e)
        {
            DiagPanel.Visibility = Visibility.Collapsed;
        }

        private void RefreshDiag()
        {
            var sb = new StringBuilder();
            sb.AppendLine("Mappe 0.1.0.0");
            sb.AppendLine("Tile modalita': " + _tiles.Mode + " -> " + _tiles.PathName);
            sb.AppendLine("Tile stats: " + _tiles.Stats());
            sb.AppendLine("Ultimo errore tile: " + _tiles.LastErrorTile + " -> " + _tiles.LastError);
            sb.AppendLine("MapControl.LoadingStatus: " + _loadingStatus);
            sb.AppendLine("Cache: " + (_tiles.Mode == TileService.ModeCustom ? "memoria LRU 200 + disco LocalFolder/tiles" : "cache della piattaforma"));
            sb.AppendLine("MapControl.Style: " + MapView.Style + ", TileSources: " + MapView.TileSources.Count);
            if (MapView.Center != null)
                sb.AppendLine("Centro: " + GeoUrls.Num(MapView.Center.Position.Latitude) + ", " +
                              GeoUrls.Num(MapView.Center.Position.Longitude) + ", zoom " + GeoUrls.Num(MapView.ZoomLevel));
            sb.AppendLine("GPS stato: " + _geoStatus + ", ultimo errore: " + _geoError);
            if (_lastPos != null)
                sb.AppendLine("GPS: " + GeoUrls.Num(_lastPos.Coordinate.Point.Position.Latitude) + ", " +
                              GeoUrls.Num(_lastPos.Coordinate.Point.Position.Longitude) + ", precisione " +
                              GeoUrls.Num(_lastPos.Coordinate.Accuracy) + " m");
            sb.AppendLine("Ricerca: " + _searchInfo);
            sb.AppendLine("Reverse: " + _reverseInfo);
            sb.AppendLine("Ora: " + DateTime.Now.ToString("HH:mm:ss"));
            DiagText.Text = sb.ToString();
        }
    }
}
