// Windows only (added to the build in CMakeLists.txt under if(WIN32)). windows.h stays in this file: its macros
// never reach the rest of the code.
#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0A00   // Windows 10 API set (as JUCE's own Windows code; MSVC defaults to it anyway)
 #endif
 #ifndef WINVER
  #define WINVER _WIN32_WINNT
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN 1
 #endif
 #include <windows.h>
 #include <shellapi.h>   // CommandLineToArgvW (self test)
 #include <tlhelp32.h>
 #pragma comment (lib, "shell32.lib")
#endif

#include "WinProcess.h"

#if JUCE_WINDOWS

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace
{
    void logError (const char* what)
    {
        std::fprintf (stderr, "[SG] process: %s failed (error %lu)\n", what, (unsigned long) ::GetLastError());
    }

    // One argument per the rules CommandLineToArgvW and the MSVC runtime use: quote if empty or if it contains
    // space, tab, newline, vertical tab or '"'; inside quotes, 2n backslashes + '"' -> n backslashes + end/begin
    // quote, 2n+1 backslashes + '"' -> n backslashes + literal '"', backslashes elsewhere are literal.
    void appendQuoted (std::wstring& cmd, const std::wstring& arg)
    {
        if (! arg.empty() && arg.find_first_of (L" \t\n\v\"") == std::wstring::npos)
        {
            cmd += arg;
            return;
        }

        cmd.push_back (L'"');
        for (size_t i = 0; ; ++i)
        {
            size_t backslashes = 0;
            while (i < arg.size() && arg[i] == L'\\')
            {
                ++backslashes;
                ++i;
            }

            if (i == arg.size())
            {
                cmd.append (backslashes * 2, L'\\');   // before the closing quote: all escaped
                break;
            }
            if (arg[i] == L'"')
            {
                cmd.append (backslashes * 2 + 1, L'\\');   // escaped backslashes + escaped quote
                cmd.push_back (L'"');
            }
            else
            {
                cmd.append (backslashes, L'\\');   // literal
                cmd.push_back (arg[i]);
            }
        }
        cmd.push_back (L'"');
    }

    std::wstring buildCommandLine (const juce::StringArray& args)
    {
        std::wstring cmd;
        for (int i = 0; i < args.size(); ++i)
        {
            const std::wstring a (args[i].toWideCharPointer());
            if (i > 0)
                cmd.push_back (L' ');
            if (i == 0)
            {
                // argv[0] is parsed without backslash escapes (up to the next quote); a Windows path has no '"'
                if (a.empty() || a.find_first_of (L" \t") != std::wstring::npos)
                    cmd += L"\"" + a + L"\"";
                else
                    cmd += a;
            }
            else
            {
                appendQuoted (cmd, a);
            }
        }
        return cmd;
    }

    bool sameNameIgnoreCase (const std::wstring& entry, const wchar_t* name)
    {
        const auto eq = entry.find (L'=', 1);   // entries like "=C:=C:\x" start with '='
        if (eq == std::wstring::npos)
            return false;
        size_t i = 0;
        for (; name[i] != 0; ++i)
            if (i >= eq || std::towupper (entry[i]) != std::towupper (name[i]))
                return false;
        return i == eq;
    }

    // Current environment + Python UTF-8 output, as a UTF-16 block ("a=b\0c=d\0\0")
    std::wstring buildEnvironment()
    {
        static const wchar_t* const extra[] = { L"PYTHONIOENCODING=utf-8", L"PYTHONUTF8=1" };
        static const wchar_t* const extraNames[] = { L"PYTHONIOENCODING", L"PYTHONUTF8" };

        std::vector<std::wstring> entries;
        if (auto* env = ::GetEnvironmentStringsW())
        {
            for (const wchar_t* p = env; *p != 0; p += std::wcslen (p) + 1)
            {
                const std::wstring e (p);
                bool replaced = false;
                for (const auto* n : extraNames)
                    replaced = replaced || sameNameIgnoreCase (e, n);
                if (! replaced)
                    entries.push_back (e);
            }
            ::FreeEnvironmentStringsW (env);
        }
        for (const auto* e : extra)
            entries.push_back (e);

        std::wstring block;
        for (const auto& e : entries)
        {
            block += e;
            block.push_back (L'\0');
        }
        block.push_back (L'\0');
        return block;
    }

    void closeHandle (HANDLE& h)
    {
        if (h != nullptr && h != INVALID_HANDLE_VALUE)
            ::CloseHandle (h);
        h = nullptr;
    }
}

namespace sg
{
struct WinProcess::Impl
{
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE readPipe = nullptr;   // parent end, not inheritable

    ~Impl()
    {
        // Without a job (assignment failed) at least the direct child must not outlive us
        if (job == nullptr && process != nullptr && ::WaitForSingleObject (process, 0) != WAIT_OBJECT_0)
            ::TerminateProcess (process, 1);
        closeHandle (job);   // KILL_ON_JOB_CLOSE: ends a still running tree
        closeHandle (thread);
        closeHandle (process);
        closeHandle (readPipe);
    }
};

WinProcess::WinProcess() : impl (std::make_unique<Impl>()) {}
WinProcess::~WinProcess() = default;

juce::String WinProcess::quoteCommandLine (const juce::StringArray& args)
{
    return juce::String (buildCommandLine (args).c_str());
}

bool WinProcess::start (const juce::StringArray& args, int streamFlags)
{
    impl = std::make_unique<Impl>();
    if (args.isEmpty())
        return false;

    SECURITY_ATTRIBUTES inherit {};
    inherit.nLength = sizeof (inherit);
    inherit.bInheritHandle = TRUE;

    HANDLE writePipe = nullptr;
    if (! ::CreatePipe (&impl->readPipe, &writePipe, &inherit, 0))
    {
        logError ("CreatePipe");
        impl->readPipe = nullptr;
        return false;
    }
    ::SetHandleInformation (impl->readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = ::CreateFileW (L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE)
    {
        logError ("CreateFileW(NUL)");
        closeHandle (writePipe);
        closeHandle (impl->readPipe);
        return false;
    }

    // Job first: the process starts suspended and joins it before it can spawn anything
    impl->job = ::CreateJobObjectW (nullptr, nullptr);   // no security attributes: handle not inheritable
    if (impl->job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (! ::SetInformationJobObject (impl->job, JobObjectExtendedLimitInformation, &limits, (DWORD) sizeof (limits)))
        {
            logError ("SetInformationJobObject");
            closeHandle (impl->job);
        }
    }
    else
    {
        logError ("CreateJobObjectW");
    }

    // Only these handles go to the child (not every inheritable handle of the host)
    HANDLE inherited[2] = { nul, writePipe };
    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList (nullptr, 1, 0, &attrSize);   // fails by design, returns the size
    std::vector<char> attrBuffer (attrSize > 0 ? (size_t) attrSize : 1);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST> (attrBuffer.data());
    bool haveAttrs = attrSize > 0 && ::InitializeProcThreadAttributeList (attrs, 1, 0, &attrSize);
    if (haveAttrs && ! ::UpdateProcThreadAttribute (attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof (inherited),
                                                    nullptr, nullptr))
    {
        logError ("UpdateProcThreadAttribute");
        ::DeleteProcThreadAttributeList (attrs);
        haveAttrs = false;   // plain handle inheritance as juce::ChildProcess does
    }

    STARTUPINFOEXW si {};
    si.StartupInfo.cb = haveAttrs ? (DWORD) sizeof (si) : (DWORD) sizeof (STARTUPINFOW);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = (streamFlags & juce::ChildProcess::wantStdOut) != 0 ? writePipe : nul;
    si.StartupInfo.hStdError  = (streamFlags & juce::ChildProcess::wantStdErr) != 0 ? writePipe : nul;
    if (haveAttrs)
        si.lpAttributeList = attrs;

    auto cmd = buildCommandLine (args);
    auto env = buildEnvironment();
    PROCESS_INFORMATION pi {};
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT
                      | (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const bool created = ::CreateProcessW (nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, env.data(), nullptr,
                                           &si.StartupInfo, &pi) != FALSE;
    if (! created)
        logError ("CreateProcessW");

    if (haveAttrs)
        ::DeleteProcThreadAttributeList (attrs);
    closeHandle (writePipe);   // only the child holds the write end now: EOF when the tree has ended
    closeHandle (nul);

    if (! created)
    {
        closeHandle (impl->readPipe);
        closeHandle (impl->job);
        return false;
    }

    impl->process = pi.hProcess;
    impl->thread = pi.hThread;

    if (impl->job != nullptr && ! ::AssignProcessToJobObject (impl->job, impl->process))
    {
        // E.g. a host job that forbids nesting: run without, kill() then only reaches the direct child
        logError ("AssignProcessToJobObject");
        closeHandle (impl->job);
    }

    if (::ResumeThread (impl->thread) == (DWORD) -1)
    {
        logError ("ResumeThread");
        kill();
        impl = std::make_unique<Impl>();
        return false;
    }
    return true;
}

bool WinProcess::isRunning() const
{
    return impl->process != nullptr && ::WaitForSingleObject (impl->process, 0) != WAIT_OBJECT_0;
}

int WinProcess::readProcessOutput (void* dest, int numBytes)
{
    if (impl->readPipe == nullptr || dest == nullptr || numBytes <= 0)
        return 0;

    for (;;)
    {
        // Running state before peeking: once it has ended, the peek sees everything it wrote
        const bool running = isRunning();
        DWORD available = 0;
        if (! ::PeekNamedPipe (impl->readPipe, nullptr, 0, nullptr, &available, nullptr))
            return 0;   // ERROR_BROKEN_PIPE: every writer is gone and the pipe is drained

        if (available > 0)
        {
            DWORD numRead = 0;
            const DWORD want = (DWORD) juce::jmin ((juce::int64) available, (juce::int64) numBytes);
            if (! ::ReadFile (impl->readPipe, dest, want, &numRead, nullptr))
                return 0;
            return (int) numRead;
        }

        // Ended (normally or by kill) and drained. Polling instead of a blocking read: a writer handle that leaked
        // into an unrelated process can never make this hang.
        if (! running)
            return 0;

        // Up to 10 ms, returns at once when the process ends (no 1 ms wakeups under a high timer resolution)
        ::WaitForSingleObject (impl->process, 10);
    }
}

juce::String WinProcess::readAllProcessOutput()
{
    juce::MemoryOutputStream result;
    char buffer[512];
    for (;;)
    {
        const int n = readProcessOutput (buffer, (int) sizeof (buffer));
        if (n <= 0)
            break;
        result.write (buffer, (size_t) n);
    }
    return result.toUTF8();
}

bool WinProcess::waitForProcessToFinish (int timeoutMs) const
{
    if (impl->process == nullptr)
        return true;
    return ::WaitForSingleObject (impl->process, timeoutMs < 0 ? INFINITE : (DWORD) timeoutMs) == WAIT_OBJECT_0;
}

juce::uint32 WinProcess::getExitCode() const
{
    DWORD code = 0;
    if (impl->process != nullptr)
        ::GetExitCodeProcess (impl->process, &code);
    return (juce::uint32) code;
}

bool WinProcess::kill()
{
    // Exit code 1: a killed run never reads as success. Termination is asynchronous: wait (up to 2 s) until the
    // processes are really gone, so the caller can delete the files they wrote (no sharing violation from a
    // grandchild like ffmpeg that still holds its output open). Only waits on kernel objects, takes no lock.
    constexpr ULONGLONG maxWaitMs = 2000;
    if (impl->job != nullptr)
    {
        const bool ok = ::TerminateJobObject (impl->job, 1) != FALSE;
        if (! ok)
            logError ("TerminateJobObject");

        const ULONGLONG deadline = ::GetTickCount64() + maxWaitMs;
        for (;;)
        {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info {};
            if (! ::QueryInformationJobObject (impl->job, JobObjectBasicAccountingInformation, &info,
                                               (DWORD) sizeof (info), nullptr))
            {
                logError ("QueryInformationJobObject");
                break;
            }
            if (info.ActiveProcesses == 0)
                break;
            if (::GetTickCount64() >= deadline)
            {
                std::fprintf (stderr, "[SG] process: %lu process(es) still active 2 s after kill\n",
                              (unsigned long) info.ActiveProcesses);
                break;
            }
            ::Sleep (5);
        }
        return ok;
    }
    if (impl->process != nullptr)
    {
        const bool ok = ::TerminateProcess (impl->process, 1) != FALSE;
        ::WaitForSingleObject (impl->process, (DWORD) maxWaitMs);
        return ok;
    }
    return true;
}

namespace winproc
{
    juce::StringArray parseCommandLine (const juce::String& commandLine)
    {
        juce::StringArray result;
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW (commandLine.toWideCharPointer(), &argc);
        if (argv == nullptr)
        {
            logError ("CommandLineToArgvW");
            return result;
        }
        for (int i = 0; i < argc; ++i)
            result.add (juce::String (argv[i]));
        ::LocalFree (argv);
        return result;
    }

    juce::StringArray findProcessesIn (const juce::Array<juce::File>& dirs, const juce::StringArray& names)
    {
        juce::StringArray found;
        HANDLE snap = ::CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            logError ("CreateToolhelp32Snapshot");
            return found;
        }

        const DWORD self = ::GetCurrentProcessId();
        PROCESSENTRY32W pe {};
        pe.dwSize = sizeof (pe);
        for (BOOL more = ::Process32FirstW (snap, &pe); more; more = ::Process32NextW (snap, &pe))
        {
            if (pe.th32ProcessID == self || pe.th32ProcessID == 0)
                continue;
            HANDLE h = ::OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h == nullptr)
                continue;   // e.g. system processes: never ours
            wchar_t path[MAX_PATH * 4] = {};
            DWORD len = (DWORD) (sizeof (path) / sizeof (path[0]));
            const bool ok = ::QueryFullProcessImageNameW (h, 0, path, &len) != FALSE;
            ::CloseHandle (h);
            if (! ok)
                continue;

            const juce::File image (juce::String (path, (size_t) len));
            if (! names.contains (image.getFileNameWithoutExtension(), true))
                continue;
            for (const auto& d : dirs)
                if (d != juce::File() && image.isAChildOf (d))
                {
                    found.add (juce::String ((juce::int64) pe.th32ProcessID) + " " + image.getFullPathName());
                    break;
                }
        }
        ::CloseHandle (snap);
        return found;
    }
}
}

#endif
