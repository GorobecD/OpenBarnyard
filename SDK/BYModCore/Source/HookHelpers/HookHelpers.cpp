#include "pch.h"
#include "HookHelpers.h"

#include <detours.h>

bool UnprotectMemory(void* memory, std::size_t size, unsigned long& oldProtect)
{
    return (VirtualProtect(memory, size, PAGE_EXECUTE_READWRITE, &oldProtect) != 0);
}

bool WriteJump(void* jumpAddress, void* output)
{
#if defined(_X86_)
    // Unprotect memory.
    if (!UnprotectMemory(output, 5))
    {
        return false;
    }
    
    // Write x86 jmp instruction.
    auto ouputBytes = static_cast<std::uint8_t*>(output);
    *ouputBytes = 0xE9;
    *reinterpret_cast<std::uint32_t*>(ouputBytes + 1) = (
        reinterpret_cast<std::uintptr_t>(jumpAddress) -
        (reinterpret_cast<std::uintptr_t>(output) + 5));

    return true;
#else
// TODO: Support other architectures.
#error "Unsupported architecture"
#endif
}

void RawLog( const char* a_szFormat, ... );

void InstallHook(void*& origFunc, void* hookFunc)
{
    void* pTarget = origFunc;
    LONG  r1      = DetourTransactionBegin();
    LONG  r2      = DetourUpdateThread(GetCurrentThread());
    LONG  r3      = DetourAttach(&origFunc, hookFunc);
    LONG  r4      = DetourTransactionCommit();
    if ( r1 || r2 || r3 || r4 )
        RawLog( "InstallHook %p FAILED: begin %ld update %ld attach %ld commit %ld", pTarget, r1, r2, r3, r4 );
    else
        RawLog( "InstallHook %p ok (first byte now %02x)", pTarget, *(unsigned char*)pTarget );
}

void* InstallVTableHook(void** vtable, std::size_t virtualFuncIndex, void* hookFunc)
{
    // Get original function pointer.
    void* origFunc = vtable[virtualFuncIndex];

    // Set new function pointer and return original function pointer.
    vtable[virtualFuncIndex] = hookFunc;
    return origFunc;
}

