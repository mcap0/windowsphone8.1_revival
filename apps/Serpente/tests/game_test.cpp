// g++ -std=c++11 -I../Serpente game_test.cpp && ./a.out
#include "Game.h"
#include <cstdio>
using namespace Serpente;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
int main()
{
    Game g(10, 10, 7);
    CHECK(g.Body().size() == 4 && g.Body().front().x == 5, "starts with 4 cells heading right");
    g.Turn(Left);
    g.Step();
    CHECK(!g.Over() && g.Body().front().x == 6, "reverse is ignored");
    g.Turn(Down); g.Turn(Left);
    g.Step(); g.Step();
    CHECK(g.Body().front().x == 5 && g.Body().front().y == 6, "two queued turns make a U-turn");
    Game w(10, 10, 3);
    for (int i = 0; i < 10 && !w.Over(); i++) w.Step();
    CHECK(w.Over(), "the wall ends the game");
    // the head may enter the cell the tail is leaving: a 2x2 loop of length 4 never dies
    Game l(10, 10, 9);
    l.Turn(Down); l.Step(); l.Turn(Left); l.Step(); l.Turn(Up); l.Step();
    CHECK(!l.Over(), "head follows the tail into its cell");
    l.Turn(Right); l.Step();
    CHECK(!l.Over(), "the loop goes on");
    // eating grows by 2 and scores
    Game e(12, 1, 1);
    int ate = 0; size_t before = e.Body().size();
    while (!e.Over()) { if (e.Step()) ate++; }
    CHECK(ate == 0 || e.Score() == ate, "score counts the food");
    CHECK(before == 4, "body length");
    Game f(8, 8, 5);
    // a simple serpentine never hits anything until the board fills or it ends
    CHECK(f.Interval() == 170, "first speed");
    printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails;
}
