// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <juce_core/juce_core.h>
#include <vector>

#if JUCE_LINUX || JUCE_MAC

// Process tree helpers for the Downloader on Linux and macOS. juce::ChildProcess (fork/execvp, no process group)
// exposes no pid and kill() only hits the direct child. yt-dlp's release binary is a PyInstaller bootloader that
// runs the real yt-dlp as its child, which in turn runs ffmpeg - those would keep running and writing.
// Linux reads /proc, macOS uses sysctl (KERN_PROC_ALL + KERN_PROCARGS2). Not for the audio thread (allocates).
namespace sg::proctree
{
struct Process
{
    int pid = 0;
    int ppid = 0;
    juce::StringArray argv;   // empty if it could not be read (other user, zombie, kernel thread)
};

// Snapshot of all processes visible to us
std::vector<Process> list();

// Finds our direct child whose argv equals args and SIGKILLs it with all its descendants
void killTree (const juce::StringArray& args);

// Processes whose argv joined with spaces contains marker (self tests: "no process of the job survives")
std::vector<Process> findByMarker (const juce::String& marker);
}

#endif
