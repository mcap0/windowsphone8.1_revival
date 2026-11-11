// =============================================================================
// DERIVED FROM MetroSurf 2.0.1 (decompiled app):
//   ref/MetroSurf/decompiled/MetroSurfApplication.decompiled.cs
//   lines 270-441 (class Wkwp.Native), copied verbatim: P/Invoke signatures,
//   constants and the EventCallback delegate. Only the namespace changed.
// Use: personal use only, on the owner's own phone. The native engine
// (wkwp.dll / webkit.dll, LGPL WebKit) is not part of this source tree's code.
// =============================================================================
using System;
using System.Text;
using System.Runtime.InteropServices;

namespace TelegramWeb.WebKit
{
	public static class Native
	{
		[UnmanagedFunctionPointer(CallingConvention.StdCall)]
		public delegate void EventCallback(IntPtr user, IntPtr view, int ev, int value, IntPtr text);

		public const int EventLoadStarted = 1;

		public const int EventLoadCommitted = 2;

		public const int EventLoadFinished = 3;

		public const int EventLoadFailed = 4;

		public const int EventTitleChanged = 5;

		public const int EventNeedsDisplay = 6;

		public const int EventConsole = 7;

		public const int EventAlert = 8;

		public const int EventProgress = 9;

		public const int EventContentsSize = 11;

		public const int EventEditingStarted = 12;

		public const int EventEditingEnded = 13;

		public const int EventEditingChanged = 14;

		public const int EventSelectionChanged = 15;

		public const int EventPageCrashed = 16;

		public const int PhaseBegan = 1;

		public const int PhaseChanged = 2;

		public const int PhaseEnded = 3;

		public const int PhaseCancelled = 4;

		public const int TouchDown = 1;

		public const int TouchMove = 2;

		public const int TouchUp = 3;

		public const int TouchCancel = 4;

		public const int KeyboardText = 0;

		public const int KeyboardUrl = 1;

		public const int KeyboardEmail = 2;

		public const int KeyboardNumber = 3;

		public const int KeyboardPhone = 4;

		public const int KeyboardPassword = 5;

		public const int KeyboardSearch = 6;

		public const int KeyboardMultiline = 7;

		public const int KeyboardUserGesture = 256;

		public const uint ClearCookies = 1u;

		public const uint ClearLocalStorage = 2u;

		public const uint ClearIndexedDB = 4u;

		public const uint ClearCache = 8u;

		public const uint ClearCacheStorage = 16u;

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
		public static extern int wkwp_init(string resourceDirectory, string dataDirectory, EventCallback callback, IntPtr user);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_set_memory_limit(int megabytes);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_memory_warning(int critical);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_restore_session(int restore);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_clear_data(uint flags);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
		public static extern int wkwp_add_user_script(string source, int atDocumentStart, int allFrames);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_remove_user_script(int id);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern IntPtr wkwp_view_create_composited(int width, int height, float deviceScale, [MarshalAs(UnmanagedType.IInspectable)] object swapChainPanel);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_destroy(IntPtr view);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_resize(IntPtr view, int width, int height);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_suspend(IntPtr view);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_resume(IntPtr view);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern int wkwp_view_paint(IntPtr view, byte[] pixels, int width, int height, int stride);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
		public static extern void wkwp_view_load_url(IntPtr view, string url);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
		public static extern void wkwp_view_load_html(IntPtr view, string html, string baseURL);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_stop(IntPtr view);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_reload(IntPtr view);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_go_back(IntPtr view);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern int wkwp_view_can_go_back(IntPtr view);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_go_forward(IntPtr view);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
		public static extern void wkwp_view_set_user_agent(IntPtr view, string userAgent);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
		public static extern int wkwp_view_get_user_agent(IntPtr view, StringBuilder result, int capacity);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
		public static extern int wkwp_view_run_script(IntPtr view, string script, StringBuilder result, int capacity);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_scroll_gesture(IntPtr view, int x, int y, int dx, int dy, int phase);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_zoom_gesture(IntPtr view, int x, int y, float scale, int phase);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_tap(IntPtr view, int x, int y);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_double_tap(IntPtr view, int x, int y);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_touch(IntPtr view, int id, int x, int y, int phase);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
		public static extern void wkwp_view_edit_text(IntPtr view, string text, int selectionStart, int selectionEnd, int sequence);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_key(IntPtr view, int virtualKey);

		[DllImport("wkwp.dll", CallingConvention = CallingConvention.StdCall)]
		public static extern void wkwp_view_blur(IntPtr view);
	}
}
