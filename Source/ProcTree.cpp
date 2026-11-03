// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ProcTree.h"

#if JUCE_LINUX || JUCE_MAC

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#if JUCE_LINUX
 #include <fstream>
 #include <iterator>
 #include <string>
#endif

#if JUCE_MAC
 #include <sys/sysctl.h>
#endif

namespace
{
    // argv from NUL-terminated strings in [begin, end): at most maxArgs (negative = no limit)
    juce::StringArray splitNulStrings (const char* begin, const char* end, int maxArgs)
    {
        juce::StringArray out;
        const char* p = begin;
        while (p < end && (maxArgs < 0 || out.size() < maxArgs))
        {
            const char* q = p;
            while (q < end && *q != '\0')
                ++q;
            out.add (juce::String::fromUTF8 (p, (int) (q - p)));
            p = q + 1;
        }
        return out;
    }

   #if JUCE_LINUX
    std::string readProcFile (const std::string& path)
    {
        std::ifstream in (path, std::ios::binary);
        return std::string ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    }

    juce::StringArray argvOf (int pid)
    {
        const auto cmd = readProcFile ("/proc/" + std::to_string (pid) + "/cmdline");
        return splitNulStrings (cmd.data(), cmd.data() + cmd.size(), -1);
    }

    std::vector<sg::proctree::Process> listImpl (bool withArgv)
    {
        std::vector<sg::proctree::Process> out;
        for (const auto& d : juce::File ("/proc").findChildFiles (juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::no))
        {
            const auto name = d.getFileName();
            if (! name.containsOnly ("0123456789"))
                continue;
            const auto base = "/proc/" + name.toStdString();
            const auto stat = readProcFile (base + "/stat");
            const auto close = stat.rfind (')');   // the command name may contain spaces and ')'
            if (close == std::string::npos || close + 4 >= stat.size())
                continue;
            sg::proctree::Process p;
            p.pid = name.getIntValue();
            p.ppid = std::atoi (stat.c_str() + close + 4);   // ") S <ppid>"
            if (withArgv)
                p.argv = argvOf (p.pid);
            out.push_back (std::move (p));
        }
        return out;
    }
   #endif

   #if JUCE_MAC
    int argMax()
    {
        int mib[2] = { CTL_KERN, KERN_ARGMAX };
        int value = 0;
        size_t size = sizeof (value);
        if (::sysctl (mib, 2, &value, &size, nullptr, 0) != 0 || value <= 0)
            return 1024 * 1024;   // fallback, macOS default is 1 MB
        return value;
    }

    // KERN_PROCARGS2 layout: int argc, exec path, NUL padding, then argc NUL-terminated argv strings, then env.
    // Fails with EPERM/EINVAL for other users' processes and zombies -> empty argv.
    juce::StringArray readArgv (int pid, std::vector<char>& buf)
    {
        int mib[3] = { CTL_KERN, KERN_PROCARGS2, pid };
        size_t size = buf.size();
        if (::sysctl (mib, 3, buf.data(), &size, nullptr, 0) != 0 || size < sizeof (int))
            return {};

        int argc = 0;
        std::memcpy (&argc, buf.data(), sizeof (argc));
        const char* p = buf.data() + sizeof (int);
        const char* end = buf.data() + size;
        while (p < end && *p != '\0')   // exec path
            ++p;
        while (p < end && *p == '\0')   // padding
            ++p;
        if (argc <= 0)
            return {};
        return splitNulStrings (p, end, argc);
    }

    juce::StringArray argvOf (int pid)
    {
        std::vector<char> buf ((size_t) argMax());
        return readArgv (pid, buf);
    }

    std::vector<sg::proctree::Process> listImpl (bool withArgv)
    {
        std::vector<sg::proctree::Process> out;
        int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
        std::vector<struct kinfo_proc> procs;
        size_t size = 0;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            if (::sysctl (mib, 3, nullptr, &size, nullptr, 0) != 0)
                return out;
            size += size / 8 + sizeof (struct kinfo_proc) * 16;   // processes may start between the two calls
            procs.resize (size / sizeof (struct kinfo_proc));
            size = procs.size() * sizeof (struct kinfo_proc);
            if (::sysctl (mib, 3, procs.data(), &size, nullptr, 0) == 0)
            {
                procs.resize (size / sizeof (struct kinfo_proc));
                break;
            }
            const int err = errno;
            procs.clear();
            if (err != ENOMEM)
                return out;
        }

        std::vector<char> buf (withArgv ? (size_t) argMax() : 0);
        for (const auto& kp : procs)
        {
            sg::proctree::Process p;
            p.pid = (int) kp.kp_proc.p_pid;
            p.ppid = (int) kp.kp_eproc.e_ppid;
            if (p.pid <= 0)   // kernel_task
                continue;
            if (withArgv)
                p.argv = readArgv (p.pid, buf);
            out.push_back (std::move (p));
        }
        return out;
    }
   #endif
}

namespace sg::proctree
{
std::vector<Process> list()
{
    return listImpl (true);
}

void killTree (const juce::StringArray& args)
{
    if (args.isEmpty())
        return;

    // argv only for our direct children: cheap enough to run under the Downloader's procLock
    const auto all = listImpl (false);
    const int self = (int) ::getpid();
    std::vector<int> tree;
    for (const auto& p : all)
        if (p.ppid == self && argvOf (p.pid) == args)
            tree.push_back (p.pid);

    for (size_t i = 0; i < tree.size(); ++i)   // breadth-first: append children of every collected pid
        for (const auto& p : all)
            if (p.ppid == tree[i])
                tree.push_back (p.pid);

    for (const int pid : tree)
        ::kill (pid, SIGKILL);
}

std::vector<Process> findByMarker (const juce::String& marker)
{
    std::vector<Process> out;
    for (auto& p : list())
        if (p.argv.joinIntoString (" ").contains (marker))
            out.push_back (std::move (p));
    return out;
}
}

#endif
