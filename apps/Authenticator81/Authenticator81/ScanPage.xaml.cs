using System.Linq;
using System;
using System.Threading.Tasks;
using Authenticator.Core;
using Windows.ApplicationModel.Activation;
using Windows.Devices.Enumeration;
using Windows.Media.Capture;
using Windows.Media.Devices;
using Windows.Media.MediaProperties;
using Windows.Storage.Pickers;
using Windows.Storage.Streams;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Authenticator
{
    public sealed partial class ScanPage : Page, IFileOpenPickerContinuable
    {
        MediaCapture capture;
        bool pageActive;
        bool suspended;
        bool starting;
        bool busy;
        bool pickerPending;
        bool continuousFocus;
        uint photoWidth = 1024;
        uint photoHeight = 768;

        public ScanPage()
        {
            InitializeComponent();
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            pageActive = true;
            suspended = false;
            await StartCameraAsync();
        }

        protected override async void OnNavigatedFrom(NavigationEventArgs e)
        {
            pageActive = false;
            await StopCameraAsync();
            base.OnNavigatedFrom(e);
        }

        async void OnScanClick(object sender, RoutedEventArgs e)
        {
            await ScanOnceAsync();
        }

        async void OnPreviewTapped(object sender, TappedRoutedEventArgs e)
        {
            await ScanOnceAsync();
        }

        async Task ScanOnceAsync()
        {
            if (busy)
                return;

            if (!pageActive || suspended || capture == null)
            {
                StatusText.Text = "Fotocamera non disponibile.";
                return;
            }

            busy = true;
            ScanButton.IsEnabled = false;

            try
            {
                StatusText.Text = "Messa a fuoco…";
                // the Lumia driver can turn the flash back on: switch it off before every shot
                DisableFlashAndTorch(capture);
                FocusControl focus = capture.VideoDeviceController.FocusControl;
                if (focus != null && focus.Supported)
                {
                    // single autofocus at close range (a QR is held near the lens), then the shot
                    try
                    {
                        var fs = new FocusSettings { Mode = FocusMode.Auto, DisableDriverFallback = false };
                        if (focus.SupportedFocusRanges.Contains(AutoFocusRange.Macro)) fs.AutoFocusRange = AutoFocusRange.Macro;
                        else if (focus.SupportedFocusRanges.Contains(AutoFocusRange.FullRange)) fs.AutoFocusRange = AutoFocusRange.FullRange;
                        focus.Configure(fs);
                    }
                    catch (Exception) { }
                    try { await focus.FocusAsync(); }
                    catch (Exception) { }
                }

                if (!pageActive || suspended || capture == null)
                    return;

                StatusText.Text = "Acquisizione immagine…";
                using (var stream = new InMemoryRandomAccessStream())
                {
                    var encoding = ImageEncodingProperties.CreateJpeg();
                    encoding.Width = photoWidth;
                    encoding.Height = photoHeight;

                    await capture.CapturePhotoToStreamAsync(encoding, stream);

                    stream.Seek(0);
                    StatusText.Text = "Lettura del codice…";
                    string text = await QrCodeDecoder.DecodeStreamAsync(stream);

                    if (string.IsNullOrEmpty(text))
                    {
                        StatusText.Text = "nessun QR trovato, avvicinati e riprova";
                        return;
                    }

                    await HandleDecodedTextAsync(text);
                }
            }
            catch (Exception ex)
            {
                if (pageActive && !suspended)
                    StatusText.Text = "Scansione non riuscita: " + ex.Message;
            }
            finally
            {
                busy = false;
                ScanButton.IsEnabled = true;
            }
        }

        async Task StartCameraAsync()
        {
            if (!pageActive || suspended || pickerPending || capture != null || starting)
                return;

            starting = true;
            MediaCapture newCapture = null;
            try
            {
                StatusText.Text = "Ricerca della fotocamera posteriore…";
                var devices = await DeviceInformation.FindAllAsync(DeviceClass.VideoCapture);
                DeviceInformation backCamera = null;

                foreach (DeviceInformation device in devices)
                {
                    if (device.EnclosureLocation != null
                        && device.EnclosureLocation.Panel == Windows.Devices.Enumeration.Panel.Back)
                    {
                        backCamera = device;
                        break;
                    }
                }

                if (backCamera == null)
                {
                    StatusText.Text = "Fotocamera posteriore non disponibile.";
                    return;
                }

                newCapture = new MediaCapture();
                var settings = new MediaCaptureInitializationSettings
                {
                    VideoDeviceId = backCamera.Id,
                    StreamingCaptureMode = StreamingCaptureMode.Video,
                    PhotoCaptureSource = PhotoCaptureSource.Photo
                };

                await newCapture.InitializeAsync(settings);
                if (!pageActive || suspended || pickerPending)
                    return;

                DisableFlashAndTorch(newCapture);
                await ConfigurePhotoResolutionAsync(newCapture);

                if (!pageActive || suspended || pickerPending)
                    return;

                newCapture.SetPreviewRotation(VideoRotation.Clockwise90Degrees);

                continuousFocus = false;
                FocusControl focus = newCapture.VideoDeviceController.FocusControl;
                if (focus != null && focus.Supported)
                {
                    foreach (FocusMode mode in focus.SupportedFocusModes)
                    {
                        if (mode == FocusMode.Continuous)
                        {
                            try
                            {
                                focus.Configure(new FocusSettings { Mode = FocusMode.Continuous });
                                continuousFocus = true;
                            }
                            catch (Exception) { }
                            break;
                        }
                    }
                }

                capture = newCapture;
                newCapture = null;
                CameraPreview.Source = capture;
                await capture.StartPreviewAsync();
                DisableFlashAndTorch(capture);
                // continuous focus only runs after a first focus request
                if (continuousFocus)
                {
                    try { await capture.VideoDeviceController.FocusControl.FocusAsync(); } catch (Exception) { }
                }

                if (pageActive && !suspended && !pickerPending)
                    StatusText.Text = "Inquadra il QR e premi Scansiona, oppure tocca l’anteprima.";
            }
            catch (Exception ex)
            {
                if (pageActive && !suspended)
                    StatusText.Text = "Impossibile avviare la fotocamera: " + ex.Message;
            }
            finally
            {
                if (newCapture != null)
                    newCapture.Dispose();
                starting = false;
            }
        }

        static void DisableFlashAndTorch(MediaCapture mediaCapture)
        {
            try
            {
                FlashControl flash = mediaCapture.VideoDeviceController.FlashControl;
                if (flash != null && flash.Supported)
                {
                    flash.Auto = false;
                    flash.RedEyeReduction = false;
                    flash.Enabled = false;
                }
            }
            catch (Exception) { }

            try
            {
                TorchControl torch = mediaCapture.VideoDeviceController.TorchControl;
                if (torch != null && torch.Supported)
                    torch.Enabled = false;
            }
            catch (Exception) { }
        }

        async Task ConfigurePhotoResolutionAsync(MediaCapture mediaCapture)
        {
            try
            {
                var properties = mediaCapture.VideoDeviceController.GetAvailableMediaStreamProperties(MediaStreamType.Photo);
                VideoEncodingProperties selected = null;
                ulong smallestArea = ulong.MaxValue;

                foreach (var property in properties)
                {
                    VideoEncodingProperties video = property as VideoEncodingProperties;
                    if (video == null || video.Width < 1024 || video.Height == 0)
                        continue;

                    ulong area = (ulong)video.Width * (ulong)video.Height;
                    if (area < smallestArea)
                    {
                        selected = video;
                        smallestArea = area;
                    }
                }

                if (selected != null)
                {
                    await mediaCapture.VideoDeviceController.SetMediaStreamPropertiesAsync(MediaStreamType.Photo, selected);
                    photoWidth = selected.Width;
                    photoHeight = selected.Height;
                }
            }
            catch (Exception)
            {
                photoWidth = 1024;
                photoHeight = 768;
            }
        }

        async Task StopCameraAsync()
        {
            while (starting || busy)
                await Task.Delay(50);

            MediaCapture oldCapture = capture;
            capture = null;
            CameraPreview.Source = null;

            if (oldCapture != null)
            {
                try { await oldCapture.StopPreviewAsync(); }
                catch (Exception) { }
                oldCapture.Dispose();
            }
        }

        public async Task SuspendCameraAsync()
        {
            suspended = true;
            await StopCameraAsync();
        }

        public async void ResumeCameraAsync()
        {
            if (pickerPending)
                return;

            suspended = false;
            await StartCameraAsync();
        }

        async void OnPickPhoto(object sender, RoutedEventArgs e)
        {
            try
            {
                pickerPending = true;
                suspended = true;
                App.SetPickerTarget("scan");

                await StopCameraAsync();

                var picker = new FileOpenPicker();
                picker.SuggestedStartLocation = PickerLocationId.PicturesLibrary;
                picker.ViewMode = PickerViewMode.Thumbnail;
                picker.FileTypeFilter.Add(".jpg");
                picker.FileTypeFilter.Add(".jpeg");
                picker.FileTypeFilter.Add(".png");
                picker.FileTypeFilter.Add(".bmp");
                picker.PickSingleFileAndContinue();
            }
            catch (Exception ex)
            {
                App.ClearPickerTarget();
                pickerPending = false;
                suspended = false;
                StatusText.Text = "Impossibile aprire le foto: " + ex.Message;
                if (pageActive)
                    await StartCameraAsync();
            }
        }

        public async void ContinueFileOpenPicker(FileOpenPickerContinuationEventArgs args)
        {
            App.ClearPickerTarget();
            pickerPending = false;
            suspended = false;

            try
            {
                if (args.Files == null || args.Files.Count == 0)
                {
                    StatusText.Text = "Nessuna foto selezionata.";
                }
                else
                {
                    StatusText.Text = "Lettura del QR nella foto…";
                    string text = await QrCodeDecoder.DecodeFileAsync(args.Files[0]);
                    if (string.IsNullOrEmpty(text))
                    {
                        StatusText.Text = "nessun QR trovato, avvicinati e riprova";
                    }
                    else
                    {
                        await HandleDecodedTextAsync(text);
                    }
                }
            }
            catch (Exception ex)
            {
                StatusText.Text = "Lettura della foto non riuscita: " + ex.Message;
            }
            finally
            {
                if (pageActive && !suspended)
                    await StartCameraAsync();
            }
        }

        async Task HandleDecodedTextAsync(string text)
        {
            OtpImportResult result;
            try
            {
                result = OtpUri.ParseImport(text);
            }
            catch (Exception ex)
            {
                StatusText.Text = "QR non valido: " + ex.Message;
                return;
            }

            StatusText.Text = "Account trovati. Conferma l’importazione.";

            int imported;
            try
            {
                imported = await AccountImport.ImportAsync(result);
            }
            catch (Exception ex)
            {
                StatusText.Text = "Importazione non riuscita: " + ex.Message;
                return;
            }

            if (imported < 0)
            {
                StatusText.Text = "Importazione annullata.";
                return;
            }

            if (result.IsMigration && result.BatchSize > 1)
            {
                StatusText.Text = "QR " + (result.BatchIndex + 1) + " di " + result.BatchSize
                    + " importato, scansiona il prossimo.";
                return;
            }

            if (imported == 0)
                StatusText.Text = "Account già presenti.";

            if (Frame != null && Frame.CanGoBack)
                Frame.GoBack();
        }
    }
}
