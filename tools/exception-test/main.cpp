// Throws and catches C++ exceptions with the CRT linked statically, the way OpenTTD's script
// engine does. Exits with 42 when every case was caught where it should be.
#include <cstdio>
#include <stdexcept>
#include <string>

struct Guard
{
    int& count;
    ~Guard() { ++count; } // runs only if unwinding works
};

static int Deep(int n)
{
    if (n == 0) throw std::runtime_error("deep");
    return Deep(n - 1) + 1;
}

int main()
{
    int passed = 0, unwound = 0;
    try { throw 7; } catch (int v) { passed += v == 7; }
    try { Guard g{ unwound }; throw std::string("text"); } catch (std::string const& s) { passed += s == "text"; }
    try { Deep(50); } catch (std::exception const& e) { passed += std::string(e.what()) == "deep"; }
    try { try { throw 1.5; } catch (double) { throw; } } catch (double d) { passed += d == 1.5; }
    std::printf("exception-test: %d of 4 caught, %d destructor(s) ran during unwinding\n", passed, unwound);
    return passed == 4 && unwound == 1 ? 42 : 1;
}
