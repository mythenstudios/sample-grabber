// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Downloader.h"
#include "Storage.h"
#include <juce_events/juce_events.h>
#include <fstream>
#include <map>
#include <vector>

#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

#if JUCE_LINUX
 #include <signal.h>
 #include <sys/types.h>
 #include <unistd.h>
#endif

namespace
{
    // yt-dlp error line for the status line. "ERROR: [youtube] <id>: Sign in to confirm your age..." -> the reason only.
    // Age-restricted and login-only videos get a clear hint (yt-dlp needs browser cookies for those).
    juce::String toolErrorText (const juce::String& line)
    {
        auto text = line.trim();
        if (text.startsWithIgnoreCase ("ERROR:"))
            text = text.substring (6).trim();
        if (text.startsWithChar ('['))
        {
            const int close = text.indexOfChar (']');
            const int colon = text.indexOfChar (close, ':');
            if (close > 0 && colon > close)
                text = text.substring (colon + 1).trim();
        }
        const auto low = text.toLowerCase();
        if (low.contains ("confirm your age") || low.contains ("age-restricted") || low.contains ("inappropriate for some users"))
            return "VIDEO IS AGE-RESTRICTED - SAVE IT AND DROP THE FILE HERE";
        if (low.contains ("sign in") || low.contains ("login required") || low.contains ("private video"))
            return "VIDEO NEEDS A LOGIN - SAVE IT AND DROP THE FILE HERE";
        return text;
    }

   #if JUCE_LINUX
    std::string readProcFile (const std::string& path)
    {
        std::ifstream in (path, std::ios::binary);
        return std::string ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    }

    // ChildProcess::kill() only hits the direct child. yt-dlp's release binary is a PyInstaller bootloader that runs
    // the real yt-dlp as its child, which in turn runs ffmpeg for sections/conversion - those would keep running and
    // writing. Find our child by its argv (JUCE exposes no pid) and SIGKILL it with all descendants.
    void killProcessTree (const juce::StringArray& args)
    {
        if (args.isEmpty())
            return;

        std::string want;
        for (const auto& a : args)
        {
            want += a.toStdString();
            want.push_back ('\0');
        }

        std::map<int, int> parentOf;
        for (const auto& d : juce::File ("/proc").findChildFiles (juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::no))
        {
            const auto name = d.getFileName();
            if (! name.containsOnly ("0123456789"))
                continue;
            const auto stat = readProcFile ("/proc/" + name.toStdString() + "/stat");
            const auto close = stat.rfind (')');   // the command name may contain spaces and ')'
            if (close == std::string::npos || close + 4 >= stat.size())
                continue;
            parentOf[name.getIntValue()] = std::atoi (stat.c_str() + close + 4);   // ") S <ppid>"
        }

        const int self = (int) ::getpid();
        std::vector<int> tree;
        for (const auto& [pid, ppid] : parentOf)
            if (ppid == self && readProcFile ("/proc/" + std::to_string (pid) + "/cmdline") == want)
                tree.push_back (pid);

        for (size_t i = 0; i < tree.size(); ++i)   // breadth-first: append children of every collected pid
            for (const auto& [pid, ppid] : parentOf)
                if (ppid == tree[i])
                    tree.push_back (pid);

        for (const int pid : tree)
            ::kill (pid, SIGKILL);
    }
   #endif
}

juce::File Downloader::dataDir (const juce::String& sub)
{
    // Persistent cache folder (not /tmp) so saved projects can find the sample again; cleaned only by CacheManager
    return sg::storage::cacheSubdir (sub);
}

void Downloader::grab (const juce::String& url, const sg::range::Range& range, double durationHint, bool deleteFullAfterCut,
                       std::function<void (float, juce::String)> onProgress,
                       std::function<void (Result)> onDone)
{
    if (! claim())
        return;

    job = Job::download;
    pendingUrl = url.trim();
    pendingRange = range;
    pendingDuration = durationHint;
    pendingDeleteFull = deleteFullAfterCut;

    // Probe info of this link, younger than 10 min (stream URLs in it expire): reuse instead of extracting again
    // (only our own file: directly in cacheRoot/tmp, not a symlink)
    pendingInfoJson = juce::File();
    if (infoJsonUrl == pendingUrl && infoJson.existsAsFile() && ! infoJson.isSymbolicLink()
        && infoJson.getParentDirectory() == sg::storage::cacheRoot().getChildFile ("tmp")
        && juce::Time::currentTimeMillis() - infoJsonTime < 10 * 60 * 1000)
        pendingInfoJson = infoJson;
    // Video id from this process' earlier probes: a later range of the same video still finds the cached whole audio
    pendingVideoId = sg::storage::videoIdFor (pendingUrl);
    rememberJobDir (dataDir ("downloads"));
    if (pendingVideoId.isNotEmpty())
        jobPattern = "_" + pendingVideoId + (pendingRange.set && ! cutsLocally (pendingRange, pendingDuration)
                                                 ? "_" + sg::range::fileTag (pendingRange) : juce::String()) + ".";
    if (jobPattern.isNotEmpty())
        sg::storage::setJobFiles (this, { jobPattern });   // CLEAR NOW keeps this job's files
    progressCb = std::move (onProgress);
    doneCb = std::move (onDone);
    if (! startThread())
        jobActive = false;
}

void Downloader::importFile (const juce::File& file, const sg::range::Range& range, int pcmBits,
                             std::function<void (float, juce::String)> onProgress,
                             std::function<void (Result)> onDone)
{
    if (! claim())
        return;

    job = Job::convert;
    pendingFile = file;
    pendingRange = range;
    pendingBits = pcmBits;
    rememberJobDir (dataDir ("imports"));
    progressCb = std::move (onProgress);
    doneCb = std::move (onDone);
    if (! startThread())
        jobActive = false;
}

void Downloader::probe (const juce::String& url, std::function<void (Result)> onDone)
{
    if (! claim())
        return;

    job = Job::probeUrl;
    pendingUrl = url.trim();
    dropInfoJson();
    rememberJobDir ({});
    progressCb = nullptr;
    doneCb = std::move (onDone);
    if (! startThread())
        jobActive = false;
}

void Downloader::probeFile (const juce::File& file, std::function<void (Result)> onDone)
{
    if (! claim())
        return;

    job = Job::probeLocal;
    pendingFile = file;
    rememberJobDir ({});
    progressCb = nullptr;
    doneCb = std::move (onDone);
    if (! startThread())
        jobActive = false;
}

bool Downloader::claim()
{
    if (jobActive.load())
        return false;
    // The previous job already posted its result (that callback may be what calls us): the thread only has to return
    if (isThreadRunning() && ! waitForThreadToExit (2000))
        return false;
    jobActive = true;
    return true;
}

void Downloader::addProduced (const juce::File& f)
{
    juce::StringArray parts;
    {
        const juce::ScopedLock sl (procLock);
        jobProduced.addIfNotAlreadyThere (f.getFullPathName());
        for (const auto& p : jobProduced)
            parts.add (juce::File (p).getFileName());
    }
    if (jobPattern.isNotEmpty())
        parts.add (jobPattern);
    sg::storage::setJobFiles (this, parts);
}

void Downloader::rememberJobDir (const juce::File& dir)
{
    jobDir = dir;
    jobPattern.clear();
    sg::storage::setJobFiles (this, {});
    {
        const juce::ScopedLock sl (procLock);
        jobProduced.clear();
    }
    jobPreexisting.clear();
    jobStartMs = juce::Time::currentTimeMillis();
    if (dir != juce::File())
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*", juce::File::FollowSymlinks::no))
            jobPreexisting.add (f.getFileName());
}

// Finished whole audio of a video id in dir: "<stem>_<id>.wav" with a non-empty stem, not a range cut, not empty,
// and no unfinished yt-dlp file (.part/.temp/.ytdl) of the same name next to it
juce::File Downloader::findCachedWhole (const juce::File& dir, const juce::String& id) const
{
    if (! sg::storage::isValidVideoId (id))
        return {};
    const auto files = dir.findChildFiles (juce::File::findFiles, false, "*", juce::File::FollowSymlinks::no);
    for (const auto& f : files)
    {
        const auto stem = f.getFileNameWithoutExtension();
        sg::range::Range dummy;
        if (f.getFileExtension() != ".wav" || f.isSymbolicLink() || f.getSize() <= 0 || ! stem.endsWith ("_" + id)
            || stem.length() <= id.length() + 1 || sg::range::parseFileTag (stem.fromLastOccurrenceOf ("_", false, false), dummy))
            continue;
        bool unfinished = false;
        for (const auto& g : files)
        {
            const auto n = g.getFileName();
            if (n != f.getFileName() && n.startsWith (stem + ".") && (n.contains (".part") || n.contains (".temp") || n.contains (".ytdl")))
                unfinished = true;
        }
        if (! unfinished)
            return f;
    }
    return {};
}

bool Downloader::cutsLocally (const sg::range::Range& range, double duration)
{
    return range.set && duration >= 0.0 && duration <= localCutMaxSeconds;
}

bool Downloader::ownFfmpeg() const
{
    const auto in = tools->getInfo (ToolManager::Tool::ffmpeg);
    return in.own && in.path != juce::File();
}

void Downloader::cancel()
{
    const bool wasActive = jobActive.load();
    signalThreadShouldExit();
    {
        const juce::ScopedLock sl (procLock);
        if (proc != nullptr)
        {
           #if JUCE_LINUX
            killProcessTree (procArgs);
           #endif
            proc->kill();   // Windows: WinProcess terminates its Job Object, i.e. the whole tree
        }
    }
    stopThread (3000);

    // Partial files of the cancelled job, only its own: exact paths it wrote, plus yt-dlp names with this job's
    // id (+ range tag) - result and .part/.temp/.ytdl variants - that were not there when it started. Never files in use.
    if (wasActive && jobDir != juce::File())
    {
        juce::StringArray produced;
        {
            const juce::ScopedLock sl (procLock);
            produced = jobProduced;
        }
        for (const auto& f : jobDir.findChildFiles (juce::File::findFiles, false, "*", juce::File::FollowSymlinks::no))
        {
            const auto name = f.getFileName();
            const bool ours = produced.contains (f.getFullPathName())
                           || (jobPattern.isNotEmpty() && name.contains (jobPattern) && ! jobPreexisting.contains (name)
                               && f.getLastModificationTime().toMilliseconds() >= jobStartMs - 2000);
            if (ours && ! sg::storage::isInUse (f))
            {
                std::fprintf (stderr, "[SG] cancel: removing partial %s\n", f.getFullPathName().toRawUTF8());
                if (! f.deleteFile())
                    std::fprintf (stderr, "[SG] cancel: could not remove partial %s\n", f.getFullPathName().toRawUTF8());
            }
        }
    }

    jobDir = juce::File();
    dropInfoJson();
    sg::storage::setJobFiles (this, {});
    jobActive = false;
}

bool Downloader::startProcess (const juce::StringArray& args)
{
    const juce::ScopedLock sl (procLock);
    if (threadShouldExit())
        return false;   // cancelled: cancel() holds procLock while killing, so nothing starts after it
    proc = std::make_unique<Process>();
    procArgs = args;
    return proc->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr);
}

void Downloader::reportProgress (float p, const juce::String& phase)
{
    auto cb = progressCb;
    if (cb == nullptr)
        return;
    juce::MessageManager::callAsync ([cb, p, phase] { if (cb) cb (p, phase); });
}

void Downloader::run()
{
    Result r;

    // Do not start a process while the ToolManager is replacing tools (and vice versa)
    tools->refresh();
    if (! tools->tryBeginExternalJob())
    {
        r.message = "TOOLS ARE UPDATING - TRY AGAIN IN A MOMENT";
    }
    else
    {
        switch (job)
        {
            case Job::download:   runDownload (r);  break;
            case Job::convert:    runConvert (r);   break;
            case Job::probeUrl:   runProbeUrl (r);  break;
            case Job::probeLocal: runProbeFile (r); break;
        }

        tools->endExternalJob();
    }

    {
        const juce::ScopedLock sl (procLock);
        proc.reset();
    }

    if (threadShouldExit())
    {
        jobActive = false;
        return;
    }

    if (job == Job::download)
        dropInfoJson();   // used up (or failed): the next grab probes again
    sg::storage::setJobFiles (this, {});   // finished: the result is loaded right away (CLEAR NOW: 5 s rule + in use)

    auto cb = doneCb;
    jobActive = false;   // before posting: the callback may start the next job right away (claim waits for this thread)
    juce::MessageManager::callAsync ([cb, r] { if (cb) cb (r); });
}

//==============================================================================
void Downloader::runDownload (Result& r)
{
    const auto outDir = dataDir ("downloads");

    const auto ytdlp = tools->getExecutable (ToolManager::Tool::ytdlp);
    const auto ffmpeg = tools->getExecutable (ToolManager::Tool::ffmpeg);
    const auto deno = tools->getExecutable (ToolManager::Tool::deno);

    if (ytdlp == juce::File())
    {
        r.message = "YT-DLP MISSING - CLICK INSTALL TOOLS";
        return;
    }
    if (ffmpeg == juce::File())
    {
        r.message = "FFMPEG MISSING - CLICK INSTALL TOOLS";
        return;
    }

    // Short source with a range: whole audio (reused from the cache) + local cut - faster than yt-dlp's
    // ffmpeg-based section download. Long sources keep --download-sections.
    const bool cutLocally = cutsLocally (pendingRange, pendingDuration);
    const auto ytRange = cutLocally ? sg::range::Range() : pendingRange;
    std::fprintf (stderr, "[SG] grab: %s, %s\n", pendingRange.set ? (cutLocally ? "range via full audio + local cut" : "range via --download-sections")
                                                                   : "whole source",
                  pendingInfoJson != juce::File() ? "reusing probe info (--load-info-json)" : "extracting");

    // Same video in flight in another job of this process: wait (cancellable), then reuse its file
    const juce::String fetchId = cutLocally ? pendingVideoId : juce::String();
    if (fetchId.isNotEmpty())
    {
        bool told = false;
        while (! sg::storage::tryBeginFetch (fetchId))
        {
            if (threadShouldExit())
                return;
            if (! told)
            {
                reportProgress (0.0f, "WAITING FOR THE SAME DOWNLOAD");
                told = true;
            }
            wait (200);
        }
    }
    struct FetchGuard
    {
        juce::String id;
        const void* owner;
        ~FetchGuard()
        {
            sg::storage::setInUse (owner, {});
            if (id.isNotEmpty())
                sg::storage::endFetch (id);
        }
    } fetchGuard { fetchId, this };

    juce::File full = fetchId.isNotEmpty() ? findCachedWhole (outDir, fetchId) : juce::File();
    bool downloadedHere = false;
    if (full != juce::File())
        std::fprintf (stderr, "[SG] grab: whole audio already in the cache: %s\n", full.getFileName().toRawUTF8());

    if (full == juce::File())
    {
        juce::StringArray args {
            ytdlp.getFullPathName(), "--no-playlist", "--no-warnings", "--restrict-filenames",
            "-x", "--audio-format", "wav",
            // Folder instead of file: yt-dlp also finds ffprobe there (in our own bin folder or next to it in the PATH folder)
            "--ffmpeg-location", ffmpeg.getParentDirectory().getFullPathName(),
            "--progress", "--newline", "--progress-template", "download:PROG %(progress._percent_str)s",
            // Range in the name: different sections of one video never collide (Storage strips the tag again for titles)
            "-o", outDir.getChildFile ("%(title).80s_%(id)s"
                                       + (ytRange.set ? "_" + sg::range::fileTag (ytRange) : juce::String())
                                       + ".%(ext)s").getFullPathName(),
            "--print", "after_move:filepath", "--no-simulate",
            "--socket-timeout", "15",
            // Stereo at most (JUCE keeps only 2 channels: a 5.1 centre with the dialogue would be lost). ffmpeg's WAV muxer
            // defaults to rf64=never (header overflow above 4 GB); -rf64 only with our own ffmpeg build (older ones lack it).
            "--postprocessor-args", juce::String ("ExtractAudio:-af ") + channelFilter + (ownFfmpeg() ? " -rf64 auto" : "")
        };

       #if JUCE_WINDOWS
        // Printed lines (filepath, errors) as UTF-8 instead of the ANSI codepage (WinProcess also sets PYTHONUTF8)
        args.addArray ({ "--encoding", "utf-8" });
       #endif

        if (ytRange.set)
            args.addArray ({ "--download-sections",
                             "*" + sg::range::toolSeconds (ytRange.from) + "-"
                                 + (ytRange.hasEnd() ? sg::range::toolSeconds (ytRange.to) : juce::String ("inf")) });

        // JS runtime for YouTube (EJS); without deno yt-dlp still tries (limited)
        if (deno != juce::File())
            args.addArray ({ "--js-runtimes", "deno:" + deno.getFullPathName() });

        if (pendingInfoJson != juce::File())
            args.addArray ({ "--load-info-json", pendingInfoJson.getFullPathName() });   // no URL: nothing to extract again
        else
            args.addArray ({ "--",   // End of options: text with a leading '-' is never read as a yt-dlp option (e.g. --exec=...)
                             pendingUrl });

        if (! startProcess (args))
        {
            r.message = "YT-DLP COULD NOT BE STARTED - CLICK INSTALL TOOLS";
            return;
        }

        const float scale = cutLocally ? 0.85f : 0.9f;
        std::string pending;   // raw bytes: a read may end inside a multi-byte UTF-8 character, decode whole lines only
        juce::String lastLine, errorLine;   // errorLine: the "ERROR:" line, later lines must not hide it
        int lastPct = -1;
        char buf[1024];

        for (;;)
        {
            const int n = proc->readProcessOutput (buf, (int) sizeof (buf));
            if (n <= 0)
                break;

            pending.append (buf, (size_t) n);

            size_t nl;
            while ((nl = pending.find_first_of ("\r\n")) != std::string::npos)
            {
                const auto line = juce::String::fromUTF8 (pending.data(), (int) nl).trim();
                pending.erase (0, nl + 1);

                if (line.isEmpty())
                    continue;

                if (line.startsWith ("PROG "))
                {
                    const float pct = line.substring (5).retainCharacters ("0123456789.").getFloatValue() / 100.0f;
                    const int ipct = (int) (pct * 100.0f);
                    if (ipct != lastPct)
                    {
                        lastPct = ipct;
                        if (pct >= 0.999f)
                            reportProgress (scale + 0.02f, "CONVERTING");
                        else
                            reportProgress (juce::jlimit (0.0f, scale, pct * scale), "DOWNLOADING");
                    }
                }
                else
                {
                    lastLine = line;
                    if (line.startsWithIgnoreCase ("ERROR:"))
                        errorLine = line;
                    const juce::File f (line);
                    if (f.existsAsFile() && f.getParentDirectory() == outDir)
                    {
                        r.file = f;
                        addProduced (f);
                    }
                }
            }
        }

        proc->waitForProcessToFinish (5000);
        const auto exitCode = proc->getExitCode();

        if (exitCode != 0 || ! r.file.existsAsFile())
        {
            r.ok = false;
            r.message = errorLine.isNotEmpty() ? toolErrorText (errorLine)
                      : lastLine.isNotEmpty()  ? toolErrorText (lastLine)
                                               : juce::String ("DOWNLOAD FAILED");
            return;
        }
        full = r.file;
        downloadedHere = true;
    }

    if (! cutLocally)
    {
        r.ok = true;
        r.file = full;
        r.message = full.getFileName();
        return;
    }

    if (threadShouldExit())
        return;

    // Cut next to the whole audio, same tagged name a section download would get
    reportProgress (0.95f, "CUTTING");
    const auto out = outDir.getChildFile (full.getFileNameWithoutExtension() + "_" + sg::range::fileTag (pendingRange) + ".wav");
    addProduced (out);
    sg::storage::setInUse (this, { full.getFullPathName() });   // another instance's KEEP-OFF delete skips it meanwhile
    juce::String err;
    r.ok = cutWithFfmpeg (full, out, pendingRange, 16, err);
    sg::storage::setInUse (this, {});
    r.file = out;
    r.message = r.ok ? out.getFileName() : err;

    // KEEP DOWNLOADS off: the whole audio goes right away - only if this job downloaded it and nothing uses it
    if (r.ok && pendingDeleteFull && downloadedHere && ! sg::storage::isInUse (full))
    {
        full.deleteFile();
        std::fprintf (stderr, "[SG] keep downloads off: deleted whole audio %s\n", full.getFullPathName().toRawUTF8());
    }
}

// ffmpeg: optional range (-ss/-t as INPUT options: seek, then read at most the duration), stereo at most, PCM WAV.
// The input is only read, never modified.
bool Downloader::cutWithFfmpeg (const juce::File& in, const juce::File& out, const sg::range::Range& rg, int bits, juce::String& err)
{
    const auto ffmpeg = tools->getExecutable (ToolManager::Tool::ffmpeg);
    if (ffmpeg == juce::File())
    {
        err = "FFMPEG MISSING - CLICK INSTALL TOOLS";
        return false;
    }
    out.deleteFile();

    juce::StringArray args { ffmpeg.getFullPathName(), "-y", "-hide_banner", "-loglevel", "error" };
    if (rg.set)
    {
        args.addArray ({ "-ss", sg::range::toolSeconds (rg.from) });
        if (rg.hasEnd())
            args.addArray ({ "-t", sg::range::toolSeconds (rg.to - rg.from) });
    }

    const juce::String codec = bits >= 32 ? "pcm_f32le" : bits > 16 ? "pcm_s24le" : "pcm_s16le";
    args.addArray ({ "-i", in.getFullPathName(),   // absolute path: never read as an option
                     "-vn", "-af", channelFilter, "-acodec", codec });
    if (ownFfmpeg())
        args.addArray ({ "-rf64", "auto" });        // WAV above 4 GB: RF64 header instead of an overflow
    args.add (out.getFullPathName());

    if (! startProcess (args))
    {
        err = "FFMPEG COULD NOT BE STARTED - CLICK INSTALL TOOLS";
        return false;
    }

    const auto output = proc->readAllProcessOutput().trim();
    proc->waitForProcessToFinish (10000);

    if (proc->getExitCode() == 0 && out.existsAsFile() && out.getSize() > 44)
        return true;
    out.deleteFile();   // no half-written result in the cache
    err = output.isEmpty() ? juce::String ("Konvertierung fehlgeschlagen") : output.fromLastOccurrenceOf ("\n", false, false);
    return false;
}

//==============================================================================
void Downloader::runConvert (Result& r)
{
    const auto tag = pendingRange.set ? "_" + sg::range::fileTag (pendingRange) : juce::String();
    const auto out = dataDir ("imports").getChildFile (pendingFile.getFileNameWithoutExtension() + tag + ".wav");
    if (out == pendingFile)
    {
        r.message = "SOURCE IS ALREADY THE CONVERTED FILE";
        return;
    }

    reportProgress (0.5f, "CONVERTING");
    addProduced (out);
    juce::String err;
    r.ok = cutWithFfmpeg (pendingFile, out, pendingRange, pendingBits, err);
    r.file = out;
    r.message = r.ok ? out.getFileName() : err;
}

//==============================================================================
// Metadata only: "<duration>|<title>" (duration "NA" for live streams or unknown). Same tool locations and "--" handling
// as the real grab, so the probe fails exactly where the grab would.
void Downloader::runProbeUrl (Result& r)
{
    const auto ytdlp = tools->getExecutable (ToolManager::Tool::ytdlp);
    const auto ffmpeg = tools->getExecutable (ToolManager::Tool::ffmpeg);
    const auto deno = tools->getExecutable (ToolManager::Tool::deno);

    if (ytdlp == juce::File())
    {
        r.message = "YT-DLP MISSING - CLICK INSTALL TOOLS";
        return;
    }

    // -J: the full info JSON (one line). Kept for the grab (--load-info-json skips a second extraction, ~3 s on YouTube).
    juce::StringArray args { ytdlp.getFullPathName(), "--no-playlist", "--no-warnings", "--skip-download",
                             "--socket-timeout", "15", "-J" };
   #if JUCE_WINDOWS
    args.addArray ({ "--encoding", "utf-8" });   // see runDownload
   #endif
    if (ffmpeg != juce::File())
        args.addArray ({ "--ffmpeg-location", ffmpeg.getParentDirectory().getFullPathName() });
    if (deno != juce::File())
        args.addArray ({ "--js-runtimes", "deno:" + deno.getFullPathName() });
    args.addArray ({ "--", pendingUrl });

    if (! startProcess (args))
    {
        r.message = "YT-DLP COULD NOT BE STARTED - CLICK INSTALL TOOLS";
        return;
    }

    const auto output = proc->readAllProcessOutput();
    proc->waitForProcessToFinish (5000);
    const auto exitCode = proc->getExitCode();

    juce::String lastLine, errorLine, json;   // -J prints "null" on stdout after a failed extraction: keep the ERROR line
    for (const auto& raw : juce::StringArray::fromLines (output))
    {
        const auto line = raw.trim();
        if (line.isEmpty())
            continue;
        if (line.startsWithChar ('{'))
            json = line;   // stderr lines (warnings/errors) are mixed in; the JSON is the line that starts with '{'
        else if (line.startsWithIgnoreCase ("ERROR:"))
            errorLine = line;
        else if (line != "null")
            lastLine = line;
    }

    juce::var info;
    if (exitCode == 0 && json.isNotEmpty() && juce::JSON::parse (json, info).wasOk() && info.isObject())
    {
        const auto d = info.getProperty ("duration", {});
        r.duration = (d.isDouble() || d.isInt() || d.isInt64()) && (double) d >= 0.0 ? (double) d : -1.0;
        r.title = info.getProperty ("title", {}).toString();
        const auto id = info.getProperty ("id", {}).toString();
        if (sg::storage::isValidVideoId (id))
            sg::storage::rememberVideoId (pendingUrl, id);

        // Kept for the grab. Contains signed stream URLs: owner-only (0600) on POSIX, created exclusively (new random name)
        const auto file = sg::storage::cacheSubdir ("tmp").getChildFile ("probe-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64())
                                                                         + ".info.json");
        bool written = false;
       #if JUCE_LINUX || JUCE_MAC || JUCE_BSD
        const int fd = ::open (file.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0)
        {
            const auto* data = json.toRawUTF8();
            const auto size = (size_t) json.getNumBytesAsUTF8();
            written = ::write (fd, data, size) == (ssize_t) size;
            ::close (fd);
            if (! written)
                file.deleteFile();
        }
       #else
        written = file.replaceWithText (json);
       #endif
        if (written)
        {
            infoJson = file;
            infoJsonUrl = pendingUrl;
            infoJsonId = id;
            infoJsonTime = juce::Time::currentTimeMillis();
        }
        r.ok = true;
    }

    if (! r.ok)
        r.message = errorLine.isNotEmpty() ? toolErrorText (errorLine)
                  : lastLine.isNotEmpty()  ? toolErrorText (lastLine)
                                           : juce::String ("LINK CHECK FAILED");
}

void Downloader::dropInfoJson()
{
    if (infoJson != juce::File())
        infoJson.deleteFile();
    infoJson = juce::File();
    infoJsonUrl.clear();
    infoJsonId.clear();
    infoJsonTime = 0;
}

// Duration of a local file via ffprobe (next to ffmpeg). Missing ffprobe or no duration -> ok with unknown duration.
void Downloader::runProbeFile (Result& r)
{
    r.file = pendingFile;
    const auto ffmpeg = tools->getExecutable (ToolManager::Tool::ffmpeg);
    const auto ffprobe = ffmpeg != juce::File() ? ffmpeg.getSiblingFile ("ffprobe" + ffmpeg.getFileExtension()) : juce::File();
    if (! ffprobe.existsAsFile())
    {
        r.ok = true;   // length unknown: the import itself still works (or reports ffmpeg missing)
        return;
    }

    const juce::StringArray args { ffprobe.getFullPathName(), "-v", "error", "-show_entries", "format=duration",
                                   "-of", "default=noprint_wrappers=1:nokey=1",
                                   pendingFile.getFullPathName() };   // absolute path: never read as an option
    if (! startProcess (args))
    {
        r.ok = true;
        return;
    }

    const auto output = proc->readAllProcessOutput().trim();
    proc->waitForProcessToFinish (5000);

    const auto first = output.upToFirstOccurrenceOf ("\n", false, false).trim();
    if (proc->getExitCode() == 0 && first.isNotEmpty() && first.containsOnly ("0123456789."))
        r.duration = first.getDoubleValue();
    r.ok = true;
}
