// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <juce_core/juce_core.h>
#include <memory>

#if JUCE_WINDOWS

// Child process for the Downloader on Windows (yt-dlp, ffmpeg, ffprobe). Same subset of juce::ChildProcess that the
// Downloader uses, but:
//  - the child and all its descendants run in a Job Object: kill() terminates the whole tree (yt-dlp.exe is a
//    PyInstaller bootloader that runs the real yt-dlp as a child, which runs ffmpeg), and destroying the object or
//    the host process going away kills the tree too (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
//  - argv is quoted per the CommandLineToArgvW rules (backslashes before quotes / at the end, tabs, empty args)
//  - the environment gets PYTHONUTF8=1 and PYTHONIOENCODING=utf-8 (Python writes pipes in the ANSI codepage otherwise)
//  - only the pipe and NUL handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), parent pipe end not inheritable
// Thread use as in the Downloader: start/destruction under its procLock, kill() from another thread while the owner
// thread reads output.
namespace sg
{
class WinProcess
{
public:
    WinProcess();
    ~WinProcess();   // kills a still running tree (closes the job)

    // streamFlags: juce::ChildProcess::wantStdOut / wantStdErr; both go to one pipe (no deadlock between two pipes)
    bool start (const juce::StringArray& args, int streamFlags);

    bool isRunning() const;
    // Blocks until some output is available, returns what is there (at most numBytes), 0 when the process has
    // ended and its output is drained (or on error / not started)
    int readProcessOutput (void* dest, int numBytes);
    juce::String readAllProcessOutput();   // until the process ends, decoded as UTF-8
    bool waitForProcessToFinish (int timeoutMs) const;
    juce::uint32 getExitCode() const;      // STILL_ACTIVE (259) while running, 1 after kill()
    bool kill();                           // whole tree, waits up to 2 s until it is gone; safe from another thread while reading

    // Command line for CreateProcessW from argv (exposed for tests)
    static juce::String quoteCommandLine (const juce::StringArray& args);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (WinProcess)
};

namespace winproc
{
    // "<pid> <image path>" of every running process (except this one) whose image lies in one of dirs and whose
    // file name without extension is one of names (case-insensitive). Used by the grab self test after CANCEL.
    juce::StringArray findProcessesIn (const juce::Array<juce::File>& dirs, const juce::StringArray& names);

    // argv as CommandLineToArgvW splits it (process self test of quoteCommandLine)
    juce::StringArray parseCommandLine (const juce::String& commandLine);
}
}

#endif
