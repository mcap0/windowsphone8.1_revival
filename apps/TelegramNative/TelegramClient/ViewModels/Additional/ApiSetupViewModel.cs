// 
// This is the source code of Telegram for Windows Phone v. 3.x.x.
// It is licensed under GNU GPL v. 2 or later.
// You should have received a copy of the license in this archive (see LICENSE).
// 
// Copyright Evgeny Nadymov, 2013-present.
// 
using System.Globalization;
using Caliburn.Micro;
using Telegram.Api;
using Telegram.Api.Aggregator;
using Telegram.Api.Services;
using Telegram.Api.Services.Cache;
using TelegramClient.Helpers;
using TelegramClient.Services;
using TelegramClient.ViewModels.Auth;

namespace TelegramClient.ViewModels.Additional
{
    public class ApiSetupViewModel : ViewModelBase
    {
        private string _apiIdText;

        private string _apiHash;

        private string _errorMessage;

        public ApiSetupViewModel(ICacheService cacheService, ICommonErrorHandler errorHandler, IStateService stateService, INavigationService navigationService, IMTProtoService mtProtoService, ITelegramEventAggregator eventAggregator)
            : base(cacheService, errorHandler, stateService, navigationService, mtProtoService, eventAggregator)
        {
            // First run (nothing configured yet): Save continues to the sign-in screen.
            IsFirstRun = !ApiCredentials.IsConfigured;

            _apiIdText = ApiConfig.ApiId > 0 ? ApiConfig.ApiId.ToString(CultureInfo.InvariantCulture) : string.Empty;
            _apiHash = ApiConfig.ApiHash ?? string.Empty;
        }

        public bool IsFirstRun { get; private set; }

        public string ApiIdText
        {
            get { return _apiIdText; }
            set
            {
                if (SetField(ref _apiIdText, value, () => ApiIdText))
                {
                    NotifyOfPropertyChange(() => IsValid);
                }
            }
        }

        public string ApiHash
        {
            get { return _apiHash; }
            set
            {
                if (SetField(ref _apiHash, value, () => ApiHash))
                {
                    NotifyOfPropertyChange(() => IsValid);
                }
            }
        }

        public string ErrorMessage
        {
            get { return _errorMessage; }
            set { SetField(ref _errorMessage, value, () => ErrorMessage); }
        }

        public bool IsValid
        {
            get { return ParseApiId() > 0 && !string.IsNullOrWhiteSpace(ApiHash); }
        }

        private int ParseApiId()
        {
            int apiId;
            if (string.IsNullOrWhiteSpace(ApiIdText)) return 0;
            return int.TryParse(ApiIdText.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out apiId) ? apiId : 0;
        }

        public void Save()
        {
            var apiId = ParseApiId();
            if (apiId <= 0)
            {
                ErrorMessage = "ID API non valido: inserisci il numero di my.telegram.org.";
                return;
            }
            if (string.IsNullOrWhiteSpace(ApiHash))
            {
                ErrorMessage = "Hash API mancante.";
                return;
            }

            ApiCredentials.Save(apiId, ApiHash.Trim());
            ErrorMessage = null;

            if (IsFirstRun)
            {
                NavigationService.UriFor<SignInViewModel>().Navigate();
            }
            else
            {
                NavigationService.GoBack();
            }
        }
    }
}
