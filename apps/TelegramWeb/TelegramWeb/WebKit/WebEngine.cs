// =============================================================================
// DERIVED FROM MetroSurf 2.0.1 (decompiled app), personal use only.
// Source: ref/MetroSurf/decompiled/MetroSurfApplication.decompiled.cs
//   CustomUserAgent            (MainPage, ~line 1613): iPhone UA string, copied
//                              WITHOUT MetroSurf's " Metrosurf/2.0.1" suffix.
//   ViewportScript             (MainPage, ~line 1677): copied verbatim below.
//   StartEngineAsync / CreateViewAsync / WebHost_SizeChanged / SizeGpuPanel:
//                              engine init, composited SwapChainPanel view,
//                              resize, panel sizing (same calls and arguments).
//   OnEngineEvent              event switch; codes 5 (title) and 7 (console) are
//                              ignored by MetroSurf and are used here.
//   SchedulePaint / PaintAsync software paint fallback into an Image.
//   Pointer / manipulation / tap forwarding, SendTouch.
//   OnEditingEvent / SyncProxy / KeyboardProxy handlers (hidden TextBox mirror).
//   Suspend, Resume, memory warning, back navigation.
// Adapted: no XAML FindName lookups, no address bar, no polyfill (MetroSurf's
// polyfill is a YouTube-only fix), no download manager.
// =============================================================================
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using Windows.Devices.Input;
using Windows.Foundation;
using Windows.Graphics.Display;
using Windows.System;
using Windows.UI.Core;
using Windows.UI.Input;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;

namespace TelegramWeb.WebKit
{
    public sealed class WebEngine
    {
        public const string HomeUrl = "https://web.telegram.org/k/";

        // MetroSurf CustomUserAgent (iPhone Safari string) without the " Metrosurf/2.0.1" suffix.
        private const string MobileUserAgent = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1";

        private const int PortraitViewportWidth = 450;
        private const int LandscapeViewportWidth = 800;

        // Copied verbatim from MetroSurf (MainPage.ViewportScript).
		private const string ViewportScript = "\n(function () {\n    var width = WIDTH;\n    var ours = null;\n\n    window.__metrosurfSetViewportWidth = function (newWidth) {\n        width = newWidth;\n        apply();\n    };\n\n    function apply() {\n        var value = 'width=' + width;\n        var metas = document.querySelectorAll('meta[name=viewport]');\n        if (!metas.length) {\n            var root = document.head || document.documentElement;\n            if (!root) return;\n            ours = document.createElement('meta');\n            ours.setAttribute('name', 'viewport');\n            root.appendChild(ours);\n            metas = [ours];\n        }\n        for (var i = 0; i < metas.length; i++) {\n            if (metas[i].getAttribute('content') !== value)\n                metas[i].setAttribute('content', value);\n        }\n    }\n\n    new MutationObserver(function (records) {\n        for (var i = 0; i < records.length; i++) {\n            var record = records[i];\n            if (record.type === 'attributes') {\n                if (record.target.nodeName === 'META') { apply(); return; }\n                continue;\n            }\n            var added = record.addedNodes;\n            for (var j = 0; j < added.length; j++) {\n                if (added[j].nodeName === 'META' || added[j].nodeName === 'HEAD') { apply(); return; }\n            }\n        }\n    }).observe(document, { childList: true, subtree: true, attributes: true, attributeFilter: ['content', 'name'] });\n\n    apply();\n})();";

        private readonly CoreDispatcher dispatcher;
        private readonly Control page;
        private readonly Grid webHost;
        private readonly SwapChainPanel gpuPanel;
        private readonly Image webImage;
        private readonly TextBox keyboardProxy;
        private readonly DispatcherTimer proxyPoll;

        private Native.EventCallback callback;
        private IntPtr view = IntPtr.Zero;
        private bool engineStarted;
        private bool engineReady;
        private bool creatingView;
        private bool gpu = true;
        private bool pinching;
        private bool editing;
        private bool editingMultiline;
        private bool movingFocusAway;
        private float deviceScale = 1f;
        private int viewWidth;
        private int viewHeight;
        private int viewportWidth = PortraitViewportWidth;
        private int paintPending;
        private int editSequence;
        private int mirroredStart;
        private int mirroredEnd;
        private string mirroredText = "";
        private WriteableBitmap bitmap;
        private byte[] pixels;
        private readonly HashSet<uint> pageTouches = new HashSet<uint>();
        private readonly Dictionary<uint, Point> lastTouchPositions = new Dictionary<uint, Point>();

        public event Action<string> TitleChanged;
        public event Action<string> ConsoleMessage;
        public event Action LoadFinished;
        public event Action<string> AlertMessage;

        public WebEngine(CoreDispatcher dispatcher, Control page, Grid webHost, SwapChainPanel gpuPanel, Image webImage, TextBox keyboardProxy)
        {
            this.dispatcher = dispatcher;
            this.page = page;
            this.webHost = webHost;
            this.gpuPanel = gpuPanel;
            this.webImage = webImage;
            this.keyboardProxy = keyboardProxy;

            proxyPoll = new DispatcherTimer();
            proxyPoll.Interval = TimeSpan.FromMilliseconds(60);
            proxyPoll.Tick += (s, e) =>
            {
                if (!editing || FocusManager.GetFocusedElement() != (object)keyboardProxy)
                {
                    proxyPoll.Stop();
                }
                else
                {
                    SyncProxy();
                }
            };
        }

        // ---- input wiring (MainPage.AttachTouchInput / KeyboardProxy handlers) ----

        public void AttachInput()
        {
            webHost.ManipulationMode = ManipulationModes.TranslateX | ManipulationModes.TranslateY | ManipulationModes.Scale;
            webHost.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler(OnPointerPressed), true);
            webHost.AddHandler(UIElement.PointerMovedEvent, new PointerEventHandler(OnPointerMoved), true);
            webHost.AddHandler(UIElement.PointerReleasedEvent, new PointerEventHandler(OnPointerReleased), true);
            webHost.AddHandler(UIElement.PointerCanceledEvent, new PointerEventHandler(OnPointerCanceled), true);
            webHost.AddHandler(UIElement.PointerCaptureLostEvent, new PointerEventHandler(OnPointerCanceled), true);
            webHost.ManipulationStarted += OnManipulationStarted;
            webHost.ManipulationDelta += OnManipulationDelta;
            webHost.ManipulationCompleted += OnManipulationCompleted;
            webHost.Tapped += OnTapped;
            webHost.DoubleTapped += OnDoubleTapped;
            webHost.SizeChanged += (s, e) => HostSizeChanged();
            keyboardProxy.TextChanged += (s, e) => SyncProxy();
            keyboardProxy.KeyDown += OnKeyDown;
            keyboardProxy.LostFocus += OnKeyboardLostFocus;
        }

        private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            PointerPoint p = e.GetCurrentPoint(webHost);
            if (e.Pointer.PointerDeviceType == PointerDeviceType.Touch)
            {
                pageTouches.Add(p.PointerId);
                SendTouch(p.PointerId, p.Position, 1);
            }
        }

        private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
        {
            PointerPoint p = e.GetCurrentPoint(webHost);
            if (pageTouches.Contains(p.PointerId) && p.IsInContact)
            {
                SendTouch(p.PointerId, p.Position, 2);
            }
        }

        private void OnPointerReleased(object sender, PointerRoutedEventArgs e)
        {
            ForwardLift(e, 3);
        }

        private void OnPointerCanceled(object sender, PointerRoutedEventArgs e)
        {
            ForwardLift(e, 4);
        }

        private void ForwardLift(PointerRoutedEventArgs e, int phase)
        {
            PointerPoint p = e.GetCurrentPoint(webHost);
            if (pageTouches.Remove(p.PointerId))
            {
                SendTouch(p.PointerId, p.Position, phase);
            }
        }

        private void SendTouch(uint id, Point position, int phase)
        {
            Point last;
            if (phase == 2 && lastTouchPositions.TryGetValue(id, out last)
                && Math.Abs(last.X - position.X) < 0.5 && Math.Abs(last.Y - position.Y) < 0.5)
            {
                return;
            }
            if (phase == 3 || phase == 4)
            {
                lastTouchPositions.Remove(id);
            }
            else
            {
                lastTouchPositions[id] = position;
            }
            if (view != IntPtr.Zero)
            {
                Native.wkwp_view_touch(view, (int)id, (int)(position.X * deviceScale), (int)(position.Y * deviceScale), phase);
            }
        }

        private int Px(double logical)
        {
            return (int)(logical * deviceScale);
        }

        private void OnManipulationStarted(object sender, ManipulationStartedRoutedEventArgs e)
        {
            if (view == IntPtr.Zero) return;
            Point pos = e.Position;
            Native.wkwp_view_scroll_gesture(view, Px(pos.X), Px(pos.Y), 0, 0, 1);
        }

        private void OnManipulationDelta(object sender, ManipulationDeltaRoutedEventArgs e)
        {
            if (view == IntPtr.Zero) return;
            Point pos = e.Position;
            int x = Px(pos.X);
            int y = Px(pos.Y);
            if (!pinching && Math.Abs(e.Cumulative.Scale - 1f) > 0.02f)
            {
                pinching = true;
                Native.wkwp_view_scroll_gesture(view, x, y, 0, 0, 4);
                Native.wkwp_view_zoom_gesture(view, x, y, 1f, 1);
            }
            if (pinching)
            {
                Native.wkwp_view_zoom_gesture(view, x, y, e.Cumulative.Scale, 2);
                return;
            }
            int dx = (int)Math.Round(e.Delta.Translation.X * deviceScale);
            int dy = (int)Math.Round(e.Delta.Translation.Y * deviceScale);
            Native.wkwp_view_scroll_gesture(view, x, y, dx, dy, 2);
        }

        private void OnManipulationCompleted(object sender, ManipulationCompletedRoutedEventArgs e)
        {
            if (view == IntPtr.Zero) return;
            Point pos = e.Position;
            int x = Px(pos.X);
            int y = Px(pos.Y);
            if (pinching)
            {
                pinching = false;
                Native.wkwp_view_zoom_gesture(view, x, y, e.Cumulative.Scale, 3);
            }
            else
            {
                Native.wkwp_view_scroll_gesture(view, x, y, 0, 0, 3);
            }
        }

        private void OnTapped(object sender, TappedRoutedEventArgs e)
        {
            if (view == IntPtr.Zero) return;
            Point p = e.GetPosition(webHost);
            Native.wkwp_view_tap(view, Px(p.X), Px(p.Y));
        }

        private void OnDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            if (view == IntPtr.Zero) return;
            Point p = e.GetPosition(webHost);
            Native.wkwp_view_double_tap(view, Px(p.X), Px(p.Y));
        }

        private void OnKeyDown(object sender, KeyRoutedEventArgs e)
        {
            Run(SyncProxy, CoreDispatcherPriority.Low);
            if (editing && view != IntPtr.Zero && e.Key == VirtualKey.Enter && !editingMultiline)
            {
                e.Handled = true;
                Native.wkwp_view_key(view, 13);
            }
        }

        private void OnKeyboardLostFocus(object sender, RoutedEventArgs e)
        {
            if (movingFocusAway)
            {
                movingFocusAway = false;
                return;
            }
            Run(() =>
            {
                if (editing && view != IntPtr.Zero && FocusManager.GetFocusedElement() != (object)keyboardProxy)
                {
                    Native.wkwp_view_blur(view);
                }
            }, CoreDispatcherPriority.Low);
        }

        // ---- engine lifecycle (StartEngineAsync / CreateViewAsync / WebHost_SizeChanged) ----

        public async Task<bool> StartAsync(string resourceDirectory, string dataDirectory, bool restoreSession)
        {
            if (engineStarted) return engineReady;
            engineStarted = true;

            DisplayInformation info = DisplayInformation.GetForCurrentView();
            deviceScale = (float)info.RawPixelsPerViewPixel;
            viewportWidth = ViewportWidthFor(info.CurrentOrientation);
            info.OrientationChanged += (s, e) => OnOrientationChanged(s.CurrentOrientation);

            callback = OnEngineEvent;
            int result = await Task.Run(() => Native.wkwp_init(resourceDirectory, dataDirectory, callback, IntPtr.Zero));
            if (result != 0)
            {
                return false;
            }
            Native.wkwp_set_memory_limit((int)(MemoryManager.AppMemoryUsageLimit >> 20));
            Native.wkwp_restore_session(restoreSession ? 1 : 0);
            ApplyUserScripts();
            engineReady = true;
            return true;
        }

        public void HostSizeChanged()
        {
            if (view == IntPtr.Zero)
            {
                TryCreateView();
                return;
            }
            if (webHost.ActualWidth <= 0) return;
            viewWidth = Math.Max(1, (int)(webHost.ActualWidth * deviceScale));
            viewHeight = Math.Max(1, (int)(webHost.ActualHeight * deviceScale));
            if (gpu)
            {
                SizeGpuPanel(viewWidth, viewHeight);
            }
            Native.wkwp_view_resize(view, viewWidth, viewHeight);
        }

        private async void TryCreateView()
        {
            if (!engineReady || creatingView || webHost.ActualWidth <= 0) return;
            creatingView = true;
            viewWidth = Math.Max(1, (int)(webHost.ActualWidth * deviceScale));
            viewHeight = Math.Max(1, (int)(webHost.ActualHeight * deviceScale));
            int width = viewWidth;
            int height = viewHeight;
            float scale = deviceScale;
            SizeGpuPanel(width, height);
            await Task.Delay(50);
            object panel = gpuPanel;
            IntPtr created = await Task.Run(() => Native.wkwp_view_create_composited(width, height, scale, panel));
            if (created == IntPtr.Zero)
            {
                creatingView = false;
                return;
            }
            view = created;
            Native.wkwp_view_set_user_agent(view, MobileUserAgent);
            Native.wkwp_view_load_url(view, HomeUrl);
        }

        private void SizeGpuPanel(int physicalWidth, int physicalHeight)
        {
            gpuPanel.Visibility = Visibility.Visible;
            webImage.Visibility = Visibility.Collapsed;
            gpuPanel.Width = physicalWidth;
            gpuPanel.Height = physicalHeight;
            gpuPanel.RenderTransform = new ScaleTransform { ScaleX = 1f / deviceScale, ScaleY = 1f / deviceScale };
            webHost.Clip = new RectangleGeometry { Rect = new Rect(0, 0, physicalWidth / deviceScale, physicalHeight / deviceScale) };
        }

        private void ApplyUserScripts()
        {
            // Order: notification bridge first (so it wraps Notification before the page runs), then viewport.
            Native.wkwp_add_user_script(UserScripts.NotificationShim, 1, 0);
            Native.wkwp_add_user_script(ViewportScript.Replace("WIDTH", viewportWidth.ToString()), 1, 0);
        }

        private void OnOrientationChanged(DisplayOrientations orientation)
        {
            int width = ViewportWidthFor(orientation);
            if (width == viewportWidth) return;
            viewportWidth = width;
            if (view != IntPtr.Zero)
            {
                string script = "window.__metrosurfSetViewportWidth && window.__metrosurfSetViewportWidth(" + width + ")";
                IntPtr target = view;
                Task.Run(() => Native.wkwp_view_run_script(target, script, null, 0));
            }
        }

        private static int ViewportWidthFor(DisplayOrientations orientation)
        {
            return (orientation == DisplayOrientations.Landscape || orientation == DisplayOrientations.LandscapeFlipped)
                ? LandscapeViewportWidth
                : PortraitViewportWidth;
        }

        // ---- navigation and power state ----

        public bool TryGoBack()
        {
            if (view == IntPtr.Zero || Native.wkwp_view_can_go_back(view) == 0)
            {
                return false;
            }
            Native.wkwp_view_go_back(view);
            return true;
        }

        public void Suspend()
        {
            if (view != IntPtr.Zero) Native.wkwp_view_suspend(view);
        }

        public void Resume()
        {
            if (view != IntPtr.Zero) Native.wkwp_view_resume(view);
        }

        public void MemoryUsageIncreased(bool critical)
        {
            if (view != IntPtr.Zero) Native.wkwp_memory_warning(critical ? 1 : 0);
        }

        // ---- engine events (native thread) ----

        private void OnEngineEvent(IntPtr user, IntPtr eventView, int ev, int value, IntPtr text)
        {
            string message = text != IntPtr.Zero ? Marshal.PtrToStringUni(text) : null;
            switch (ev)
            {
                case Native.EventNeedsDisplay:
                    SchedulePaint();
                    break;
                case Native.EventEditingStarted:
                case Native.EventEditingEnded:
                case Native.EventEditingChanged:
                    Run(() => OnEditingEvent(ev, value, message));
                    break;
                case Native.EventLoadFinished:
                    Run(() => { if (LoadFinished != null) LoadFinished(); });
                    break;
                case Native.EventTitleChanged:
                    Run(() => { if (TitleChanged != null) TitleChanged(message ?? ""); });
                    break;
                case Native.EventConsole:
                    Run(() => { if (ConsoleMessage != null) ConsoleMessage(message ?? ""); });
                    break;
                case Native.EventAlert:
                    Run(() => { if (AlertMessage != null) AlertMessage(message ?? ""); });
                    break;
                case Native.EventPageCrashed:
                    Run(() => { if (view != IntPtr.Zero) Native.wkwp_view_reload(view); });
                    break;
            }
        }

        private void Run(Action action, CoreDispatcherPriority priority = CoreDispatcherPriority.Normal)
        {
            // Fire-and-forget by design (as MetroSurf does); the operation is kept only to show intent.
            IAsyncAction operation = dispatcher.RunAsync(priority, () => action());
        }

        // ---- software paint fallback (SchedulePaint / PaintAsync) ----

        private void SchedulePaint()
        {
            if (Interlocked.Exchange(ref paintPending, 1) == 1) return;
            Run(async () =>
            {
                if (gpu)
                {
                    gpu = false;
                    gpuPanel.Visibility = Visibility.Collapsed;
                    webImage.Visibility = Visibility.Visible;
                }
                await PaintAsync();
            });
        }

        private async Task PaintAsync()
        {
            Interlocked.Exchange(ref paintPending, 0);
            if (view == IntPtr.Zero) return;
            int width = viewWidth;
            int height = viewHeight;
            if (bitmap == null || bitmap.PixelWidth != width || bitmap.PixelHeight != height)
            {
                bitmap = new WriteableBitmap(width, height);
                pixels = new byte[width * height * 4];
                webImage.Source = bitmap;
                webImage.Width = width / deviceScale;
                webImage.Height = height / deviceScale;
            }
            IntPtr painted = view;
            byte[] buffer = pixels;
            WriteableBitmap target = bitmap;
            int result = await Task.Run(() => Native.wkwp_view_paint(painted, buffer, width, height, width * 4));
            if (result == 0)
            {
                using (Stream stream = WindowsRuntimeBufferExtensions.AsStream(target.PixelBuffer))
                {
                    stream.Write(buffer, 0, buffer.Length);
                }
                target.Invalidate();
            }
        }

        // ---- text input mirror (OnEditingEvent / SyncProxy / InputScopeFor) ----

        private void OnEditingEvent(int ev, int value, string message)
        {
            if (view == IntPtr.Zero) return;

            if (ev == Native.EventEditingEnded)
            {
                editing = false;
                Task.Delay(150).ContinueWith(_ => Run(() =>
                {
                    if (!editing && FocusManager.GetFocusedElement() == (object)keyboardProxy)
                    {
                        movingFocusAway = true;
                        page.Focus(FocusState.Programmatic);
                    }
                }));
                return;
            }
            if (ev == Native.EventEditingStarted)
            {
                int kind = value & 0xFF;
                editing = true;
                editingMultiline = kind == Native.KeyboardMultiline;
                keyboardProxy.AcceptsReturn = editingMultiline;
                keyboardProxy.IsTextPredictionEnabled = kind != Native.KeyboardPassword && kind != Native.KeyboardUrl && kind != Native.KeyboardEmail;
                keyboardProxy.IsSpellCheckEnabled = keyboardProxy.IsTextPredictionEnabled;
                InputScope scope = new InputScope();
                scope.Names.Add(new InputScopeName(InputScopeFor(kind)));
                keyboardProxy.InputScope = scope;
                if ((value & Native.KeyboardUserGesture) != 0)
                {
                    keyboardProxy.Focus(FocusState.Programmatic);
                }
                return;
            }

            if (value < editSequence || message == null) return;
            int newline = message.IndexOf('\n');
            if (newline < 0) return;
            string[] parts = message.Substring(0, newline).Split(' ');
            string text = message.Substring(newline + 1).Replace('\n', '\r');
            int start, end;
            if (parts.Length < 2 || !int.TryParse(parts[0], out start) || !int.TryParse(parts[1], out end)) return;
            if (text == mirroredText && start == mirroredStart && end == mirroredEnd) return;

            mirroredText = text;
            mirroredStart = start;
            mirroredEnd = end;
            if (keyboardProxy.Text != text)
            {
                keyboardProxy.Text = text;
            }
            start = Math.Min(start, text.Length);
            keyboardProxy.Select(start, Math.Max(0, Math.Min(end, text.Length) - start));
        }

        private void SyncProxy()
        {
            if (editing && !proxyPoll.IsEnabled)
            {
                proxyPoll.Start();
            }
            if (!editing || view == IntPtr.Zero) return;

            string text = keyboardProxy.Text;
            int start = keyboardProxy.SelectionStart;
            int end = start + keyboardProxy.SelectionLength;
            if (text == mirroredText && start == mirroredStart && end == mirroredEnd) return;

            mirroredText = text;
            mirroredStart = start;
            mirroredEnd = end;
            Native.wkwp_view_edit_text(view, text.Replace('\r', '\n'), start, end, ++editSequence);
        }

        private static InputScopeNameValue InputScopeFor(int kind)
        {
            switch (kind)
            {
                case Native.KeyboardUrl: return (InputScopeNameValue)1;
                case Native.KeyboardEmail: return (InputScopeNameValue)5;
                case Native.KeyboardNumber: return (InputScopeNameValue)29;
                case Native.KeyboardPhone: return (InputScopeNameValue)32;
                case Native.KeyboardSearch: return (InputScopeNameValue)50;
                default: return (InputScopeNameValue)0;
            }
        }
    }
}
