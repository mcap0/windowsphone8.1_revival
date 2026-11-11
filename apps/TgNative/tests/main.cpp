int runTlTests();
int runCryptoTests();
int runMtprotoAuthTests();
int runMtprotoDhTests();
int runMtprotoMsgTests();
int runMtprotoRpcTests();
int runTlObjTests();
int runVoiceTests();
int runClientTests();

int main()
{
    const int failures =
        runTlTests() +
        runCryptoTests() +
        runMtprotoAuthTests() +
        runMtprotoDhTests() +
        runMtprotoMsgTests() +
        runMtprotoRpcTests() +
        runTlObjTests() +
        runVoiceTests() +
        runClientTests();

    return failures == 0 ? 0 : 1;
}
