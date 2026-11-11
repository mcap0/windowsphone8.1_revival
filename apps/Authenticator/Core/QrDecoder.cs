using System;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading.Tasks;
using Windows.Graphics.Imaging;
using Windows.Storage;
using ZXing;
using ZXing.Common;

namespace Authenticator.Core
{
    /// <summary>Reads an otpauth:// QR code from a photo taken with the camera.</summary>
    public static class QrDecoder
    {
        public static async Task<string> DecodeAsync(StorageFile photo)
        {
            using (var stream = await photo.OpenReadAsync())
            {
                var decoder = await BitmapDecoder.CreateAsync(stream);
                // a 41 MP photo is far too big: scale it so that the long side is at most 1600 px
                double scale = Math.Min(1.0, 1600.0 / Math.Max(decoder.PixelWidth, decoder.PixelHeight));
                var transform = new BitmapTransform
                {
                    ScaledWidth = (uint)(decoder.PixelWidth * scale),
                    ScaledHeight = (uint)(decoder.PixelHeight * scale),
                    InterpolationMode = BitmapInterpolationMode.Fant
                };
                var pixels = await decoder.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore,
                    transform, ExifOrientationMode.RespectExifOrientation, ColorManagementMode.DoNotColorManage);
                byte[] bgra = pixels.DetachPixelData();
                int w = (int)transform.ScaledWidth, h = (int)transform.ScaledHeight;

                var source = new RGBLuminanceSource(bgra, w, h, RGBLuminanceSource.BitmapFormat.BGRA32);
                var reader = new BarcodeReaderGeneric
                {
                    AutoRotate = true,
                    Options = new DecodingOptions
                    {
                        TryHarder = true,
                        PossibleFormats = new[] { BarcodeFormat.QR_CODE }
                    }
                };
                var result = reader.Decode(source);
                return result?.Text;
            }
        }
    }
}
