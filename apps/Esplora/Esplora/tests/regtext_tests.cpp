#include "../Core/RegText.h"
#include <cstdio>
#include <cstdlib>
using namespace RegText;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main()
{
    KeyPath p;
    CHECK(ParseKeyPath(L"HKLM\\SOFTWARE\\Microsoft\\", p) && p.hive == 2 && p.sub == L"SOFTWARE\\Microsoft");
    CHECK(ParseKeyPath(L"Computer\\HKEY_CURRENT_USER\\Software", p) && p.hive == 1 && p.sub == L"Software");
    CHECK(ParseKeyPath(L"hkcr", p) && p.hive == 0 && p.sub.empty());
    CHECK(ParseKeyPath(L"", p) && p.hive == -1);
    CHECK(!ParseKeyPath(L"FOO\\bar", p));
    ParseKeyPath(L"HKLM\\A\\B", p);
    CHECK(Format(p) == L"HKLM\\A\\B");
    CHECK(Format(Parent(p)) == L"HKLM\\A");
    CHECK(Format(Parent(Parent(p))) == L"HKLM");
    CHECK(Parent(Parent(Parent(p))).hive == -1);
    CHECK(Format(Child(Parent(Parent(p)), L"X")) == L"HKLM\\X");
    std::vector<unsigned char> d; std::wstring e;
    CHECK(TextToValue(Dword, L"0x1234", d, e) && d.size() == 4 && d[0] == 0x34 && d[1] == 0x12);
    CHECK(ValueToText(Dword, d) == L"0x1234 (4660)");
    CHECK(TextToValue(Dword, L"0x1234 (4660)", d, e) && d[0] == 0x34);
    CHECK(TextToValue(Dword, L"4660", d, e) && d[1] == 0x12);
    CHECK(!TextToValue(Dword, L"4294967296", d, e));
    CHECK(!TextToValue(Dword, L"zz", d, e));
    CHECK(TextToValue(DwordBE, L"1", d, e) && d[3] == 1 && d[0] == 0);
    CHECK(TextToValue(Qword, L"0x100000000", d, e) && d.size() == 8 && d[4] == 1);
    CHECK(ValueToText(Qword, d) == L"0x100000000 (4294967296)");
    CHECK(TextToValue(Sz, L"ciao", d, e) && d.size() == 10 && ValueToText(Sz, d) == L"ciao");
    CHECK(TextToValue(MultiSz, L"a\nbc", d, e) && ValueToText(MultiSz, d) == L"a\nbc" && d.size() == 12);
    CHECK(TextToValue(MultiSz, L"", d, e) && d.size() == 4);
    CHECK(TextToValue(Binary, L"01 0a FF", d, e) && d.size() == 3 && d[2] == 255 && Hex(d, 10) == L"01 0A FF");
    CHECK(!TextToValue(Binary, L"0", d, e));
    CHECK(!TextToValue(Binary, L"0g", d, e));
    CHECK(FormatSize(1536) == L"1.5 KB");
    CHECK(FormatSize(5) == L"5 B");
    printf(fails ? "FALLITI %d\n" : "tutti ok\n", fails);
    return fails ? 1 : 0;
}
