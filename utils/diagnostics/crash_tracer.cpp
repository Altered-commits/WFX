// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "crash_tracer.hpp"

#if defined(WFX_PLATFORM_POSIX)
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>

#if defined(WFX_PLATFORM_LINUX)
#include <ucontext.h>
#elif defined(WFX_PLATFORM_MACOS)
#include <sys/ucontext.h>
#endif

#elif defined(WFX_PLATFORM_WINDOWS)
#include <windows.h>
#endif

namespace WFX::Utils {

// vvv Safe strlen vvv
static std::size_t SafeStrLen(const char* s) noexcept
{
    if(!s)
        return 0;

    const char* p = s;
    while(*p)
        ++p;

    return static_cast<std::size_t>(p - s);
}

void CrashTracer::SafeWrite(int fd, const char* s) noexcept
{
#if defined(WFX_PLATFORM_WINDOWS)
    DWORD w;
    WriteFile(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd)), s, (DWORD)SafeStrLen(s), &w, nullptr);
#else
    ::write(fd, s, SafeStrLen(s));
#endif
}

void CrashTracer::SafeWriteInt(int fd, long long v) noexcept
{
    char buf[32];
    int i = 31;
    buf[i--] = '\0';

    bool neg = v < 0;
    unsigned long long x = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;

    if(x == 0)
        buf[i--] = '0';

    while(x && i >= 0) {
        buf[i--] = '0' + (x % 10);
        x /= 10;
    }

    if(neg)
        buf[i--] = '-';

    SafeWrite(fd, buf + i + 1);
}

void CrashTracer::SafeWriteHex(int fd, unsigned long long v) noexcept
{
    static constexpr char kHexChars[] = "0123456789abcdef";

    char buf[32];
    int i = 31;
    buf[i--] = '\0';

    if(v == 0)
        buf[i--] = '0';

    while(v && i >= 0) {
        buf[i--] = kHexChars[v & 0xF];
        v >>= 4;
    }

    buf[i--] = 'x';
    buf[i--] = '0';

    SafeWrite(fd, buf + i + 1);
}

void CrashTracer::SafeWriteFrame(int fd, const Frame& f, int i) noexcept
{
    // [N] func_name
    //     file.cpp:123
    SafeWrite(fd, "  [");
    SafeWriteInt(fd, i);
    SafeWrite(fd, "] ");
    SafeWrite(fd, f.func ? f.func : "??");
    SafeWrite(fd, "\n      ");
    SafeWrite(fd, f.file ? f.file : "??");
    SafeWrite(fd, ":");
    SafeWriteInt(fd, f.line);
    SafeWrite(fd, "\n");
}

// vvv Common time getter vvv
long long CrashTracer::GetEpochNow() noexcept
{
#if defined(WFX_PLATFORM_WINDOWS)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    return (long long)(t / 10000000ULL - 11644473600ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
#endif
}

// vvv UTC conversion vvv
CrashTracer::UTCTime CrashTracer::EpochToUTC(long long sec) noexcept
{
    long long days = sec / 86400;
    long long rem = sec % 86400;

    if(rem < 0) {
        rem += 86400;
        --days;
    }

    int h = (int)(rem / 3600);
    rem %= 3600;
    int m = (int)(rem / 60);
    int s = (int)(rem % 60);

    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);

    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;

    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned mth = mp + (mp < 10 ? 3 : -9);

    y += (mth <= 2);

    return {(int)y, (int)mth, (int)d, h, m, s};
}

// vvv UTC timestamp vvv
void CrashTracer::SafeWriteTimestamp(int fd, long long epoch) noexcept
{
    auto t = EpochToUTC(epoch);

    SafeWrite(fd, "  Time     : ");

    SafeWriteInt(fd, t.year);
    SafeWrite(fd, "-");
    if(t.mon < 10)
        SafeWrite(fd, "0");

    SafeWriteInt(fd, t.mon);
    SafeWrite(fd, "-");
    if(t.day < 10)
        SafeWrite(fd, "0");

    SafeWriteInt(fd, t.day);
    SafeWrite(fd, " ");
    if(t.hour < 10)
        SafeWrite(fd, "0");

    SafeWriteInt(fd, t.hour);
    SafeWrite(fd, ":");
    if(t.min < 10)
        SafeWrite(fd, "0");

    SafeWriteInt(fd, t.min);
    SafeWrite(fd, ":");
    if(t.sec < 10)
        SafeWrite(fd, "0");

    SafeWriteInt(fd, t.sec);

    SafeWrite(fd, " UTC\n");
}

// vvv Path vvv
void CrashTracer::BuildLogPath(char* out, std::size_t max, long long epoch) noexcept
{
    std::size_t pos = 0;

    auto append = [&](const char* s) {
        for(std::size_t i = 0; s[i] && pos + 1 < max; i++)
            out[pos++] = s[i];
    };

    // Inline epoch -> decimal into path buffer, no 'SafeWriteInt' (writes to fd not buf)
    auto appendInt = [&](long long v) {
        char buf[24];
        int i = 23;
        buf[i--] = '\0';

        unsigned long long x = (unsigned long long)v;
        if(x == 0) {
            buf[i--] = '0';
        }
        else
            while(x && i >= 0) {
                buf[i--] = '0' + (x % 10);
                x /= 10;
            }

        append(buf + i + 1);
    };

    append(logDir_);
    append("/");
    appendInt(epoch);
    append("_wfx_");
    append(workerName_);
    append(".log");

    out[pos] = '\0';
}

// vvv Registers vvv
#if defined(WFX_PLATFORM_POSIX)
void CrashTracer::WriteRegisters(int fd, void* uctxPtr) noexcept
{
    if(!uctxPtr)
        return;

#if defined(WFX_PLATFORM_LINUX) && defined(WFX_ARCH_X64)
    auto* uc = (ucontext_t*)uctxPtr;
    auto& r = uc->uc_mcontext.gregs;

    SafeWrite(fd, "  RAX    : ");
    SafeWriteHex(fd, r[REG_RAX]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RBX    : ");
    SafeWriteHex(fd, r[REG_RBX]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RCX    : ");
    SafeWriteHex(fd, r[REG_RCX]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RDX    : ");
    SafeWriteHex(fd, r[REG_RDX]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RSI    : ");
    SafeWriteHex(fd, r[REG_RSI]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RDI    : ");
    SafeWriteHex(fd, r[REG_RDI]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RBP    : ");
    SafeWriteHex(fd, r[REG_RBP]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RSP    : ");
    SafeWriteHex(fd, r[REG_RSP]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R8     : ");
    SafeWriteHex(fd, r[REG_R8]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R9     : ");
    SafeWriteHex(fd, r[REG_R9]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R10    : ");
    SafeWriteHex(fd, r[REG_R10]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R11    : ");
    SafeWriteHex(fd, r[REG_R11]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R12    : ");
    SafeWriteHex(fd, r[REG_R12]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R13    : ");
    SafeWriteHex(fd, r[REG_R13]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R14    : ");
    SafeWriteHex(fd, r[REG_R14]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R15    : ");
    SafeWriteHex(fd, r[REG_R15]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RIP    : ");
    SafeWriteHex(fd, r[REG_RIP]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  EFL    : ");
    SafeWriteHex(fd, r[REG_EFL]);
    SafeWrite(fd, "\n");

#elif defined(WFX_PLATFORM_LINUX) && defined(WFX_ARCH_ARM64)
    auto* uc = (ucontext_t*)uctxPtr;
    auto& r = uc->uc_mcontext;

    SafeWrite(fd, "  X0     : ");
    SafeWriteHex(fd, r.regs[0]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X1     : ");
    SafeWriteHex(fd, r.regs[1]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X2     : ");
    SafeWriteHex(fd, r.regs[2]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X3     : ");
    SafeWriteHex(fd, r.regs[3]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X4     : ");
    SafeWriteHex(fd, r.regs[4]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X5     : ");
    SafeWriteHex(fd, r.regs[5]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X6     : ");
    SafeWriteHex(fd, r.regs[6]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X7     : ");
    SafeWriteHex(fd, r.regs[7]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X8     : ");
    SafeWriteHex(fd, r.regs[8]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X9     : ");
    SafeWriteHex(fd, r.regs[9]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X10    : ");
    SafeWriteHex(fd, r.regs[10]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X11    : ");
    SafeWriteHex(fd, r.regs[11]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X12    : ");
    SafeWriteHex(fd, r.regs[12]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X13    : ");
    SafeWriteHex(fd, r.regs[13]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X14    : ");
    SafeWriteHex(fd, r.regs[14]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X15    : ");
    SafeWriteHex(fd, r.regs[15]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X16    : ");
    SafeWriteHex(fd, r.regs[16]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X17    : ");
    SafeWriteHex(fd, r.regs[17]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X18    : ");
    SafeWriteHex(fd, r.regs[18]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X19    : ");
    SafeWriteHex(fd, r.regs[19]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X20    : ");
    SafeWriteHex(fd, r.regs[20]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X21    : ");
    SafeWriteHex(fd, r.regs[21]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X22    : ");
    SafeWriteHex(fd, r.regs[22]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X23    : ");
    SafeWriteHex(fd, r.regs[23]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X24    : ");
    SafeWriteHex(fd, r.regs[24]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X25    : ");
    SafeWriteHex(fd, r.regs[25]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X26    : ");
    SafeWriteHex(fd, r.regs[26]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X27    : ");
    SafeWriteHex(fd, r.regs[27]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X28    : ");
    SafeWriteHex(fd, r.regs[28]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  FP     : ");
    SafeWriteHex(fd, r.regs[29]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  LR     : ");
    SafeWriteHex(fd, r.regs[30]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  SP     : ");
    SafeWriteHex(fd, r.sp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  PC     : ");
    SafeWriteHex(fd, r.pc);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  PSTATE : ");
    SafeWriteHex(fd, r.pstate);
    SafeWrite(fd, "\n");

#elif defined(WFX_PLATFORM_MACOS) && defined(WFX_ARCH_X64)
    auto* uc = (ucontext_t*)uctxPtr;
    auto& ss = uc->uc_mcontext->__ss;

    SafeWrite(fd, "  RAX    : ");
    SafeWriteHex(fd, ss.__rax);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RBX    : ");
    SafeWriteHex(fd, ss.__rbx);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RCX    : ");
    SafeWriteHex(fd, ss.__rcx);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RDX    : ");
    SafeWriteHex(fd, ss.__rdx);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RSI    : ");
    SafeWriteHex(fd, ss.__rsi);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RDI    : ");
    SafeWriteHex(fd, ss.__rdi);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RBP    : ");
    SafeWriteHex(fd, ss.__rbp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RSP    : ");
    SafeWriteHex(fd, ss.__rsp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R8     : ");
    SafeWriteHex(fd, ss.__r8);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R9     : ");
    SafeWriteHex(fd, ss.__r9);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R10    : ");
    SafeWriteHex(fd, ss.__r10);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R11    : ");
    SafeWriteHex(fd, ss.__r11);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R12    : ");
    SafeWriteHex(fd, ss.__r12);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R13    : ");
    SafeWriteHex(fd, ss.__r13);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R14    : ");
    SafeWriteHex(fd, ss.__r14);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R15    : ");
    SafeWriteHex(fd, ss.__r15);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RIP    : ");
    SafeWriteHex(fd, ss.__rip);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RFLAGS : ");
    SafeWriteHex(fd, ss.__rflags);
    SafeWrite(fd, "\n");

#elif defined(WFX_PLATFORM_MACOS) && defined(WFX_ARCH_ARM64)
    auto* uc = (ucontext_t*)uctxPtr;
    auto& ss = uc->uc_mcontext->__ss;

    SafeWrite(fd, "  X0     : ");
    SafeWriteHex(fd, ss.__x[0]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X1     : ");
    SafeWriteHex(fd, ss.__x[1]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X2     : ");
    SafeWriteHex(fd, ss.__x[2]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X3     : ");
    SafeWriteHex(fd, ss.__x[3]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X4     : ");
    SafeWriteHex(fd, ss.__x[4]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X5     : ");
    SafeWriteHex(fd, ss.__x[5]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X6     : ");
    SafeWriteHex(fd, ss.__x[6]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X7     : ");
    SafeWriteHex(fd, ss.__x[7]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X8     : ");
    SafeWriteHex(fd, ss.__x[8]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X9     : ");
    SafeWriteHex(fd, ss.__x[9]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X10    : ");
    SafeWriteHex(fd, ss.__x[10]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X11    : ");
    SafeWriteHex(fd, ss.__x[11]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X12    : ");
    SafeWriteHex(fd, ss.__x[12]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X13    : ");
    SafeWriteHex(fd, ss.__x[13]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X14    : ");
    SafeWriteHex(fd, ss.__x[14]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X15    : ");
    SafeWriteHex(fd, ss.__x[15]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X16    : ");
    SafeWriteHex(fd, ss.__x[16]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X17    : ");
    SafeWriteHex(fd, ss.__x[17]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X18    : ");
    SafeWriteHex(fd, ss.__x[18]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X19    : ");
    SafeWriteHex(fd, ss.__x[19]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X20    : ");
    SafeWriteHex(fd, ss.__x[20]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X21    : ");
    SafeWriteHex(fd, ss.__x[21]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X22    : ");
    SafeWriteHex(fd, ss.__x[22]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X23    : ");
    SafeWriteHex(fd, ss.__x[23]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X24    : ");
    SafeWriteHex(fd, ss.__x[24]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X25    : ");
    SafeWriteHex(fd, ss.__x[25]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X26    : ");
    SafeWriteHex(fd, ss.__x[26]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X27    : ");
    SafeWriteHex(fd, ss.__x[27]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X28    : ");
    SafeWriteHex(fd, ss.__x[28]);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  FP     : ");
    SafeWriteHex(fd, ss.__fp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  LR     : ");
    SafeWriteHex(fd, ss.__lr);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  SP     : ");
    SafeWriteHex(fd, ss.__sp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  PC     : ");
    SafeWriteHex(fd, ss.__pc);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  CPSR   : ");
    SafeWriteHex(fd, ss.__cpsr);
    SafeWrite(fd, "\n");
#else
    (void)fd;
    (void)uctxPtr;
#endif
}

#elif defined(WFX_PLATFORM_WINDOWS)
void CrashTracer::WriteRegisters(int fd, CONTEXT* ctx) noexcept
{
    if(!ctx)
        return;

#if defined(WFX_ARCH_X64)
    SafeWrite(fd, "  RAX    : ");
    SafeWriteHex(fd, ctx->Rax);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RBX    : ");
    SafeWriteHex(fd, ctx->Rbx);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RCX    : ");
    SafeWriteHex(fd, ctx->Rcx);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RDX    : ");
    SafeWriteHex(fd, ctx->Rdx);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RSI    : ");
    SafeWriteHex(fd, ctx->Rsi);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RDI    : ");
    SafeWriteHex(fd, ctx->Rdi);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RBP    : ");
    SafeWriteHex(fd, ctx->Rbp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RSP    : ");
    SafeWriteHex(fd, ctx->Rsp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R8     : ");
    SafeWriteHex(fd, ctx->R8);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R9     : ");
    SafeWriteHex(fd, ctx->R9);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R10    : ");
    SafeWriteHex(fd, ctx->R10);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R11    : ");
    SafeWriteHex(fd, ctx->R11);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R12    : ");
    SafeWriteHex(fd, ctx->R12);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R13    : ");
    SafeWriteHex(fd, ctx->R13);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R14    : ");
    SafeWriteHex(fd, ctx->R14);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  R15    : ");
    SafeWriteHex(fd, ctx->R15);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  RIP    : ");
    SafeWriteHex(fd, ctx->Rip);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  EFLAGS : ");
    SafeWriteHex(fd, ctx->EFlags);
    SafeWrite(fd, "\n");

#elif defined(WFX_ARCH_ARM64)
    SafeWrite(fd, "  X0     : ");
    SafeWriteHex(fd, ctx->X0);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X1     : ");
    SafeWriteHex(fd, ctx->X1);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X2     : ");
    SafeWriteHex(fd, ctx->X2);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X3     : ");
    SafeWriteHex(fd, ctx->X3);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X4     : ");
    SafeWriteHex(fd, ctx->X4);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X5     : ");
    SafeWriteHex(fd, ctx->X5);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X6     : ");
    SafeWriteHex(fd, ctx->X6);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X7     : ");
    SafeWriteHex(fd, ctx->X7);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X8     : ");
    SafeWriteHex(fd, ctx->X8);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X9     : ");
    SafeWriteHex(fd, ctx->X9);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X10    : ");
    SafeWriteHex(fd, ctx->X10);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X11    : ");
    SafeWriteHex(fd, ctx->X11);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X12    : ");
    SafeWriteHex(fd, ctx->X12);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X13    : ");
    SafeWriteHex(fd, ctx->X13);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X14    : ");
    SafeWriteHex(fd, ctx->X14);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X15    : ");
    SafeWriteHex(fd, ctx->X15);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X16    : ");
    SafeWriteHex(fd, ctx->X16);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X17    : ");
    SafeWriteHex(fd, ctx->X17);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X18    : ");
    SafeWriteHex(fd, ctx->X18);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X19    : ");
    SafeWriteHex(fd, ctx->X19);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X20    : ");
    SafeWriteHex(fd, ctx->X20);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X21    : ");
    SafeWriteHex(fd, ctx->X21);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X22    : ");
    SafeWriteHex(fd, ctx->X22);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X23    : ");
    SafeWriteHex(fd, ctx->X23);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X24    : ");
    SafeWriteHex(fd, ctx->X24);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X25    : ");
    SafeWriteHex(fd, ctx->X25);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X26    : ");
    SafeWriteHex(fd, ctx->X26);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X27    : ");
    SafeWriteHex(fd, ctx->X27);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  X28    : ");
    SafeWriteHex(fd, ctx->X28);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  FP     : ");
    SafeWriteHex(fd, ctx->Fp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  LR     : ");
    SafeWriteHex(fd, ctx->Lr);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  SP     : ");
    SafeWriteHex(fd, ctx->Sp);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  PC     : ");
    SafeWriteHex(fd, ctx->Pc);
    SafeWrite(fd, "\n");
    SafeWrite(fd, "  CPSR   : ");
    SafeWriteHex(fd, ctx->Cpsr);
    SafeWrite(fd, "\n");
#endif
}
#endif

// vvv Crash body vvv
void CrashTracer::WriteCrashBody(int fd, int sig, void* siginfo, void* uctx, long long epoch) noexcept
{
    SafeWrite(fd, "==================== WFX CRASH REPORT ====================\n");
    SafeWrite(fd, "\n--- Basic Information ---\n");

    SafeWriteTimestamp(fd, epoch);
    SafeWrite(fd, "  Worker   : ");
    SafeWrite(fd, workerName_);
    SafeWrite(fd, "\n");

#if defined(WFX_PLATFORM_POSIX)
    SafeWrite(fd, "  PID      : ");
    SafeWriteInt(fd, ::getpid());
    SafeWrite(fd, "\n");

    SafeWrite(fd, "  Signal   : ");
    switch(sig) {
        case SIGSEGV:
            SafeWrite(fd, "SIGSEGV  (segmentation fault)\n");
            break;
        case SIGABRT:
            SafeWrite(fd, "SIGABRT  (abort)\n");
            break;
        case SIGBUS:
            SafeWrite(fd, "SIGBUS   (bus error)\n");
            break;
        case SIGILL:
            SafeWrite(fd, "SIGILL   (illegal instruction)\n");
            break;
        case SIGFPE:
            SafeWrite(fd, "SIGFPE   (arithmetic error)\n");
            break;
        default:
            SafeWrite(fd, "UNKNOWN\n");
            break;
    }

    if(siginfo) {
        auto* si = (siginfo_t*)siginfo;
        SafeWrite(fd, "  Fault    : ");
        SafeWriteHex(fd, (unsigned long long)si->si_addr);
        SafeWrite(fd, "\n");

        if(sig == SIGSEGV) {
            SafeWrite(fd, "  Reason   : ");
            switch(si->si_code) {
                case SEGV_MAPERR:
                    SafeWrite(fd, "address not mapped\n");
                    break;
                case SEGV_ACCERR:
                    SafeWrite(fd, "access permission denied\n");
                    break;
                default:
                    SafeWrite(fd, "unknown\n");
                    break;
            }
        }
        if(sig == SIGBUS) {
            SafeWrite(fd, "  Reason   : ");
            switch(si->si_code) {
                case BUS_ADRALN:
                    SafeWrite(fd, "invalid address alignment\n");
                    break;
                case BUS_ADRERR:
                    SafeWrite(fd, "nonexistent physical address\n");
                    break;
                default:
                    SafeWrite(fd, "unknown\n");
                    break;
            }
        }
    }

    SafeWrite(fd, "\n--- Registers (most recent snapshot) ---\n");
    WriteRegisters(fd, uctx);

#elif defined(WFX_PLATFORM_WINDOWS)
    if(siginfo) {
        auto* ep = (EXCEPTION_POINTERS*)siginfo;
        auto* rec = ep->ExceptionRecord;

        SafeWrite(fd, "  ExCode   : ");
        SafeWriteHex(fd, rec->ExceptionCode);
        SafeWrite(fd, "\n");
        SafeWrite(fd, "  ExAddr   : ");
        SafeWriteHex(fd, (unsigned long long)rec->ExceptionAddress);
        SafeWrite(fd, "\n");

        SafeWrite(fd, "  ExDesc   : ");
        switch(rec->ExceptionCode) {
            case EXCEPTION_ACCESS_VIOLATION:
                SafeWrite(fd, "access violation  (");
                SafeWrite(fd, rec->ExceptionInformation[0] == 0 ? "read" : "write");
                SafeWrite(fd, " at ");
                SafeWriteHex(fd, rec->ExceptionInformation[1]);
                SafeWrite(fd, ")\n");
                break;
            case EXCEPTION_STACK_OVERFLOW:
                SafeWrite(fd, "stack overflow\n");
                break;
            case EXCEPTION_ILLEGAL_INSTRUCTION:
                SafeWrite(fd, "illegal instruction\n");
                break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
                SafeWrite(fd, "divide by zero\n");
                break;
            case EXCEPTION_INT_OVERFLOW:
                SafeWrite(fd, "integer overflow\n");
                break;
            default:
                SafeWrite(fd, "unknown\n");
                break;
        }

        SafeWrite(fd, "\n--- Registers ---\n");
        WriteRegisters(fd, ep->ContextRecord);
    }
#endif

    SafeWrite(fd, "\n--- Traceback (most recent call last) ---\n");
    if(depth_ <= 0)
        SafeWrite(fd, "  (empty, no trace hit)\n");
    else
        for(int i = 0; i < depth_; i++)
            SafeWriteFrame(fd, frames_[i], i);

    SafeWrite(fd, "\n==================== END ====================");
}

// vvv Handlers vvv
#if defined(WFX_PLATFORM_POSIX)
void CrashTracer::PosixHandler(int sig, siginfo_t* info, void* uctx) noexcept
{
    long long epoch = GetEpochNow();

    char path[320];
    BuildLogPath(path, sizeof(path), epoch);

    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0)
        fd = STDERR_FILENO;

    WriteCrashBody(fd, sig, info, uctx, epoch);

    if(fd != STDERR_FILENO)
        ::close(fd);

    // Restore default and re-raise so OS can write core dump
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

void CrashTracer::InstallPosix() noexcept
{
    stack_t ss{};
    ss.ss_sp = altStack_;
    ss.ss_size = sizeof(altStack_);
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = PosixHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}

#elif defined(WFX_PLATFORM_WINDOWS)
LONG WINAPI CrashTracer::WindowsVEHandler(EXCEPTION_POINTERS* ep) noexcept
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if(code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
       code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
       code != EXCEPTION_INT_OVERFLOW && code != EXCEPTION_FLT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;

    long long epoch = GetEpochNow();

    char path[320];
    BuildLogPath(path, sizeof(path), epoch);

    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    int fd = (h == INVALID_HANDLE_VALUE) ? (int)(intptr_t)GetStdHandle(STD_ERROR_HANDLE) : (int)(intptr_t)h;

    WriteCrashBody(fd, 0, ep, nullptr, epoch);

    if(h != INVALID_HANDLE_VALUE)
        CloseHandle(h);

    return EXCEPTION_CONTINUE_SEARCH;
}

void CrashTracer::InstallWindows() noexcept
{
    AddVectoredExceptionHandler(1, WindowsVEHandler);
}
#endif

// vvv Public vvv
void CrashTracer::Install(const char* logDir) noexcept
{
    if(logDir)
        for(int i = 0; i < 255 && logDir[i]; i++)
            logDir_[i] = logDir[i];
    else {
#if defined(WFX_PLATFORM_WINDOWS)
        const char* def = "C:\\Temp";
#else
        const char* def = "/tmp";
#endif
        for(int i = 0; i < 255 && def[i]; i++)
            logDir_[i] = def[i];
    }

#if defined(WFX_PLATFORM_POSIX)
    InstallPosix();
#elif defined(WFX_PLATFORM_WINDOWS)
    InstallWindows();
#endif
}

void CrashTracer::SetWorkerName(const char* name) noexcept
{
    if(!name)
        return;

    for(int i = 0; i < 31 && name[i]; i++)
        workerName_[i] = name[i];
}

} // namespace WFX::Utils