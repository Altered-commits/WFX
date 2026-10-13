// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "crash_tracer.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>

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
    ::write(fd, s, SafeStrLen(s));
}

void CrashTracer::SafeWriteInt(int fd, long long v) noexcept
{
    char buf[32];
    int i = 31;
    buf[i--] = '\0';

    const bool neg = v < 0;
    unsigned long long x = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;

    if(x == 0)
        buf[i--] = '0';

    while(x && i >= 0) {
        buf[i--] = static_cast<char>('0' + (x % 10));
        x /= 10;
    }

    if(neg)
        buf[i--] = '-';

    SafeWrite(fd, buf + i + 1);
}

void CrashTracer::SafeWriteHex(int fd, unsigned long long v) noexcept
{
    static constexpr char K_HEX_CHARS[] = "0123456789abcdef";

    char buf[32];
    int i = 31;
    buf[i--] = '\0';

    if(v == 0)
        buf[i--] = '0';

    while(v && i >= 0) {
        buf[i--] = K_HEX_CHARS[v & 0xF];
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
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
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

    const int h = (int)(rem / 3600);
    rem %= 3600;
    const int m = (int)(rem / 60);
    const int s = (int)(rem % 60);

    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);

    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;

    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned mth = mp + (mp < 10 ? 3 : -9);

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
                buf[i--] = static_cast<char>('0' + (x % 10));
                x /= 10;
            }

        append(buf + i + 1);
    };

    append(GlobalLogDir);
    append("/");
    appendInt(epoch);
    append("_wfx_");
    append(GlobalWorkerName);
    append(".log");

    out[pos] = '\0';
}

// vvv Registers vvv
void CrashTracer::WriteRegisters(int fd, void* uctxPtr) noexcept
{
    if(!uctxPtr)
        return;

#if defined(WFX_ARCH_X64)
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

#elif defined(WFX_ARCH_ARM64)
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

#else
    (void)fd;
    (void)uctxPtr;
#endif
}

// vvv Crash body vvv
void CrashTracer::WriteCrashBody(int fd, int sig, void* siginfo, void* uctx, long long epoch) noexcept
{
    SafeWrite(fd, "==================== WFX CRASH REPORT ====================\n");
    SafeWrite(fd, "\n--- Basic Information ---\n");

    SafeWriteTimestamp(fd, epoch);
    SafeWrite(fd, "  Worker   : ");
    SafeWrite(fd, GlobalWorkerName);
    SafeWrite(fd, "\n");

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

    SafeWrite(fd, "\n--- Traceback (most recent call last) ---\n");
    if(GlobalDepth <= 0)
        SafeWrite(fd, "  (empty, no trace hit)\n");
    else
        for(int i = 0; i < GlobalDepth; i++)
            SafeWriteFrame(fd, GlobalFrames[i], i);

    SafeWrite(fd, "\n==================== END ====================");
}

// vvv Handlers vvv
void CrashTracer::PosixHandler(int sig, siginfo_t* info, void* uctx) noexcept
{
    const long long epoch = GetEpochNow();

    char path[320];
    BuildLogPath(path, sizeof(path), epoch);

    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0)
        fd = STDERR_FILENO;

    WriteCrashBody(fd, sig, info, uctx, epoch);

    if(fd != STDERR_FILENO)
        ::close(fd);

    // Restore default and re-raise so OS can write core dump
    struct sigaction sa {};
    sa.sa_handler = SIG_DFL;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(sig, &sa, nullptr);
    (void)::raise(sig);
}

void CrashTracer::InstallPosix() noexcept
{
    stack_t ss{};
    ss.ss_sp = GlobalAltStack;
    ss.ss_size = sizeof(GlobalAltStack);
    ::sigaltstack(&ss, nullptr);

    struct sigaction sa {};
    sa.sa_sigaction = PosixHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    ::sigemptyset(&sa.sa_mask);

    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
    ::sigaction(SIGILL, &sa, nullptr);
    ::sigaction(SIGFPE, &sa, nullptr);
}

// vvv Public vvv
void CrashTracer::Install(const char* logDir) noexcept
{
    if(logDir)
        for(int i = 0; i < 255 && logDir[i]; i++)
            GlobalLogDir[i] = logDir[i];
    else {
        const char* def = "/tmp";
        for(int i = 0; i < 255 && def[i]; i++)
            GlobalLogDir[i] = def[i];
    }

    // Under ASan, its own SIGSEGV/SIGABRT handler is more useful than ours (prints the actual
    // allocation/free site).
#ifndef WFX_ASAN_BUILD
    InstallPosix();
#endif
}

void CrashTracer::SetWorkerName(const char* name) noexcept
{
    if(!name)
        return;

    for(int i = 0; i < 31 && name[i]; i++)
        GlobalWorkerName[i] = name[i];
}

} // namespace WFX::Utils