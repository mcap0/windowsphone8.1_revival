using Windows.ApplicationModel.Activation;

namespace Authenticator
{
    public interface IFileOpenPickerContinuable
    {
        void ContinueFileOpenPicker(FileOpenPickerContinuationEventArgs args);
    }
}
