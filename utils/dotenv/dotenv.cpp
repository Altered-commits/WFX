#include "dotenv.hpp"

#include "utils/backport/string.hpp"

#include <cstring>
#include <cerrno>

#if defined(_WIN32) || defined(_WIN64)
    #define WFX_USE_WINDOWS 1
    #include <windows.h>
    #include <sys/types.h>
    #include <sys/stat.h>
#else
    #define WFX_USE_POSIX 1
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <pwd.h>
    #include <sys/mman.h>
#endif

namespace WFX::Utils {

// vvv Internal Functions vvv
#if WFX_USE_POSIX
static bool CheckFileSecurityPosix(int fd, const EnvConfig& opt)
{
    struct stat st;
    if(fstat(fd, &st) != 0)
        return false;

    if(opt.GetFlag(EnvFlags::REQUIRE_OWNER_UID)) {
        uid_t euid = geteuid();
        if(st.st_uid != euid)
            return false;
    }

    if(
        opt.GetFlag(EnvFlags::REQUIRE_PERMS_600)
        && (st.st_mode & (S_IRWXG | S_IRWXO))
    )
        return false;

    return true;
}

static bool SetEnvVar(const std::string& k,
    const std::string& v, const EnvConfig& opt)
{
    bool overwriteExisting = opt.GetFlag(EnvFlags::OVERWRITE_EXISTING);

    if(
        !overwriteExisting
        && (getenv(k.c_str()) != nullptr)
    )
        return true;

    return setenv(k.c_str(), v.c_str(), overwriteExisting ? 1 : 0) == 0;
}
#else
static bool SetEnvVar(const std::string& k,
    const std::string& v, const EnvConfig& opt)
{
    if(!opt.GetFlag(EnvFlag::OVERWRITE_EXISTING)) {
        size_t required = 0;
        getenv_s(&required, nullptr, 0, k.c_str());
        if(required != 0)
            return true;
    }

    return _putenv_s(k.c_str(), v.c_str()) == 0;
}
#endif

// vvv Main Function vvv
bool Dotenv::LoadFromFile(const std::string& path, const EnvConfig& opts)
{
#if WFX_USE_POSIX
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path.c_str(), flags);
    if(fd < 0)
        return false;

    if(!CheckFileSecurityPosix(fd, opts)) {
        close(fd);
        return false;
    }

    struct stat st;
    if(fstat(fd, &st) != 0) {
        close(fd);
        return false;
    }

    std::size_t sz = static_cast<std::size_t>(st.st_size);
    if(sz == 0) {
        close(fd);
        return true;
    }

    std::vector<char> buf;
    buf.resize(sz);
    ssize_t r = 0;
    size_t off = 0;

    while(off < sz) {
        r = read(fd, buf.data() + off, sz - off);
        if(r < 0) {
            close(fd);
            return false;
        }
        if(r == 0)
            break;
        off += static_cast<size_t>(r);
    }
    close(fd);

    bool mlocked = false;
    if(
        opts.GetFlag(EnvFlags::MLOCK_BUFFER)
        && (mlock(buf.data(), buf.size()) == 0)
    )
        mlocked = true;

    auto kv = ParseFromBuffer(buf);

    for(const auto &p : kv)
        (void)SetEnvVar(p.first, p.second, opts);

    if(opts.GetFlag(EnvFlags::UNLINK_AFTER_LOAD))
        unlink(path.c_str());

    // Zero out kv strings
    for(auto &p : kv) {
        volatile char *kptr = const_cast<volatile char*>(p.first.data());
        for(std::size_t i = 0; i < p.first.size(); ++i)
            kptr[i] = 0;

        volatile char *vptr = const_cast<volatile char*>(p.second.data());
        for(std::size_t i = 0; i < p.second.size(); ++i)
            vptr[i] = 0;
    }

    if(mlocked)
        munlock(buf.data(), buf.size());

    return true;

#elif WFX_USE_WINDOWS
    // Minimal Windows implementation
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if(!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
        return false;

    std::ifstream ifs(path, std::ios::binary);
    if(!ifs.is_open())
        return false;
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    std::vector<char> buf(content.begin(), content.end());
    auto kv = ParseFromBuffer(buf);

    for(const auto &p : kv)
        (void)SetEnvVar(p.first, p.second, opts);

    if(opts.GetFlag(EnvFlags::UNLINK_AFTER_LOAD))
        DeleteFileA(path.c_str());

    // Zero out kv strings
    for(auto &p : kv) {
        volatile char *kptr = const_cast<volatile char*>(p.first.data());
        for(std::size_t i = 0; i < p.first.size(); ++i)
            kptr[i] = 0;

        volatile char *vptr = const_cast<volatile char*>(p.second.data());
        for(std::size_t i = 0; i < p.second.size(); ++i)
            vptr[i] = 0;
    }

    return true;
#else
    (void)path; (void)opts;
    return false;
#endif
}

// vvv Helper Function vvv
StringMap Dotenv::ParseFromBuffer(const std::vector<char>& buf_)
{
    // Work on a copy because we will inspect content
    std::vector<char> buf = buf_;
    StringMap out;
    
    std::string line;
    line.reserve(256);

    std::size_t n = buf.size();

    for(std::size_t i = 0; i <= n; ++i) {
        char c = (i < n) ? buf[i] : '\n';

        if(c == '\r')
            continue;

        if(c == '\n') {
            TrimInline(line);

            if(!line.empty() && line[0] != '#') {
                std::size_t eq = line.find('=');

                if(eq != std::string::npos) {
                    std::string key = line.substr(0, eq);
                    std::string val = line.substr(eq + 1);

                    TrimInline(key); TrimInline(val);

                    if(!val.empty()
                        && ((val.front() == '"' && val.back() == '"')
                        || (val.front() == '\'' && val.back() == '\'')))
                    {
                        val = val.substr(1, val.size() - 2);
                    }

                    if(!key.empty())
                        out.emplace(std::move(key), std::move(val));
                }
            }

            line.clear();
            continue;
        }

        line.push_back(c);
    }

    // Attempt to zero internal buffer copy
    if(!buf.empty()) {
        volatile char *p = buf.data();
        for(std::size_t i = 0; i < buf.size(); ++i)
            p[i] = 0;
    }

    return out;
}

} // namespace WFX::Utils