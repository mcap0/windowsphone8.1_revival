#ifndef PERLECAST_TEST_H
#define PERLECAST_TEST_H

#include <iostream>
#include <stdexcept>
#include <string>

inline int& TestFailures() { static int count = 0; return count; }
inline void Check(bool condition, const std::string& what)
{
    if (condition) std::cout << "  ok   " << what << "\n";
    else { ++TestFailures(); std::cout << "  FAIL " << what << "\n"; }
}
template<class A, class B> inline void Equal(const A& expected, const B& actual, const std::string& what)
{
    Check(expected == actual, what);
}

#endif
