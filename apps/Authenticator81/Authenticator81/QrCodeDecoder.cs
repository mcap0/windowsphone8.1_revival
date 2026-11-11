using System;
using System.Threading.Tasks;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Windows.Storage.Streams;

namespace Authenticator
{
    public static class QrCodeDecoder
    {
        public static async Task<string> DecodeFileAsync(StorageFile file)
        {
            if (file == null)
                throw new ArgumentNullException("file");

            using (IRandomAccessStream stream = await file.OpenAsync(FileAccessMode.Read))
                return await DecodeStreamAsync(stream);
        }

        public static async Task<string> DecodeStreamAsync(IRandomAccessStream stream)
        {
            if (stream == null)
                throw new ArgumentNullException("stream");

            stream.Seek(0);
            BitmapDecoder decoder = await BitmapDecoder.CreateAsync(stream);

            uint width = decoder.PixelWidth;
            uint height = decoder.PixelHeight;
            if (width == 0 || height == 0)
                return null;

            double scale = Math.Min(1.0, 1024.0 / Math.Max(width, height));
            var transform = new BitmapTransform
            {
                ScaledWidth = Math.Max(1u, (uint)(width * scale)),
                ScaledHeight = Math.Max(1u, (uint)(height * scale))
            };

            PixelDataProvider pixels = await decoder.GetPixelDataAsync(
                BitmapPixelFormat.Bgra8,
                BitmapAlphaMode.Ignore,
                transform,
                ExifOrientationMode.RespectExifOrientation,
                ColorManagementMode.DoNotColorManage);

            byte[] pixelBytes = pixels.DetachPixelData();
            var reader = new ZXing.BarcodeReader
            {
                AutoRotate = true,
                Options = new ZXing.Common.DecodingOptions
                {
                    TryHarder = true,
                    PossibleFormats = new[] { ZXing.BarcodeFormat.QR_CODE }
                }
            };

            ZXing.Result result = reader.Decode(
                pixelBytes,
                (int)transform.ScaledWidth,
                (int)transform.ScaledHeight,
                ZXing.BitmapFormat.BGRA32);

            return result == null ? null : result.Text;
        }
    }
}
