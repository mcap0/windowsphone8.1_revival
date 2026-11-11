#include <iostream>

int RunSsdpTests();
int RunDlnaSoapTests();
int RunSrtTests();
int RunTsTests();
int RunProxyHttpTests();
int RunControllerTests();
int RunQueueTests();

int main()
{
    int failures = 0;
    failures += RunSsdpTests();
    failures += RunDlnaSoapTests();
    failures += RunSrtTests();
    failures += RunTsTests();
    failures += RunProxyHttpTests();
    failures += RunControllerTests();
    failures += RunQueueTests();
    std::cout << (failures ? "FAIL " : "PASS ") << failures << " test(s)\n";
    return failures ? 1 : 0;
}
