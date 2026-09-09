// cxxsmoke.cpp - Prova de C++ no kernel (sem CRT: sem construtores
// globais, sem excecoes/RTTI/new). Chamado pelo kmain; loga via serial.
#include <stdint.h>

extern "C" void serial_puts(const char* s);

namespace mapple {

class Smoke {
public:
    void init(uint32_t magic)
    {
        magic_ = magic;
        ready_ = (magic_ == 0x2BADB002u);
    }
    bool ready() const
    {
        return ready_;
    }

private:
    uint32_t magic_ = 0;
    bool ready_ = false;
};

} // namespace mapple

static mapple::Smoke g_smoke; // POD-like: sem construtor -> seguro sem CRT

extern "C" void cxx_smoke(uint32_t magic)
{
    g_smoke.init(magic);
    serial_puts(g_smoke.ready() ? "c++: runtime ok (classes, sem CRT)\n"
                                : "c++: FAIL\n");
}
