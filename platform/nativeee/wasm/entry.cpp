#include "common.h"
#include "platform/system.h"
#include "retail/libc.h"
#include "ps2e_seam.h"
#include <kernel.h>

extern "C" unsigned char __heap_base[];

extern "C"
{
    int Main(u32 argc, char** argv);

    __attribute__((export_name("entry"))) void entry(void)
    {

        u32 now = reinterpret_cast<u32>(RetailLibc::Sbrk(0));
        u32 base = reinterpret_cast<u32>(__heap_base);
        if (now < base)
        {
            RetailLibc::Sbrk(static_cast<s32>((base - now + 15u) & ~15u));
        }

        static char* argv[] = {const_cast<char*>("reNsanity"), nullptr};
        Platform::System::Exit(Main(1, argv));
    }

    [[noreturn]] void ProgramExit(int status)
    {
        Platform::System::Exit(status);
    }

    int kputs(char* text)
    {
        ps2e_system_kputs(reinterpret_cast<ps2e_u32>(text));
        return 0;
    }
}
