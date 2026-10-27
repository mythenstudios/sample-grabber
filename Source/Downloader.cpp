#include "Downloader.h"
#include <juce_events/juce_events.h>

juce::File Downloader::dataDir (const juce::String& sub)
{
    // Persistent folder (not /tmp) so saved projects can find the sample again
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("SampleGrabber").getChildFile (sub);
    dir.createDirectory();
    return dir;
}

void Downloader::grab (const juce::String& url,
                       std::function<void (float, juce::String)> onProgress,
                       std::function<void (Result)> onDone)
{
    if (isThreadRunning())
        return;

    job = Job::download;
    pendingUrl = url.trim();
    progressCb = std::move (onProgress);
    doneCb = std::move (onDone);
    startThread();
}

void Downloader::importFile (const juce::File& file,
                             std::function<void (float, juce::String)> onProgress,
                             std::function<void (Result)> onDone)
{
    if (isThreadRunning())
        return;

    job = Job::convert;
    pendingFile = file;
    progressCb = std::move (onProgress);
    doneCb = std::move (onDone);
    startThread();
}

void Downloader::cancel()
{
    signalThreadShouldExit();
    {
        const juce::ScopedLock sl (procLock);
        if (proc != nullptr)
            proc->kill();
    }
    stopThread (3000);
}

void Downloader::reportProgress (float p, const juce::String& phase)
{
    auto cb = progressCb;
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
        if (job == Job::download)
            runDownload (r);
        else
            runConvert (r);

        tools->endExternalJob();
    }

    {
        const juce::ScopedLock sl (procLock);
        proc.reset();
    }

    if (threadShouldExit())
        return;

    auto cb = doneCb;
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

    juce::StringArray args {
        ytdlp.getFullPathName(), "--no-playlist", "--no-warnings", "--restrict-filenames",
        "-x", "--audio-format", "wav",
        // Folder instead of file: yt-dlp also finds ffprobe there (in our own bin folder or next to it in the PATH folder)
        "--ffmpeg-location", ffmpeg.getParentDirectory().getFullPathName(),
        "--progress", "--newline", "--progress-template", "download:PROG %(progress._percent_str)s",
        "-o", outDir.getChildFile ("%(title).80s_%(id)s.%(ext)s").getFullPathName(),
        "--print", "after_move:filepath", "--no-simulate"
    };

    // JS runtime for YouTube (EJS); without deno yt-dlp still tries (limited)
    if (deno != juce::File())
        args.addArray ({ "--js-runtimes", "deno:" + deno.getFullPathName() });

    args.addArray ({ "--",   // End of options: text with a leading '-' is never read as a yt-dlp option (e.g. --exec=...)
                     pendingUrl });

    bool started = false;
    {
        const juce::ScopedLock sl (procLock);
        proc = std::make_unique<juce::ChildProcess>();
        started = proc->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr);
    }

    if (! started)
    {
        r.message = "YT-DLP COULD NOT BE STARTED - CLICK INSTALL TOOLS";
        return;
    }

    juce::String pending, lastLine;
    int lastPct = -1;
    char buf[1024];

    for (;;)
    {
        const int n = proc->readProcessOutput (buf, (int) sizeof (buf));
        if (n <= 0)
            break;

        pending += juce::String::fromUTF8 (buf, n);

        int nl;
        while ((nl = pending.indexOfAnyOf ("\r\n")) >= 0)
        {
            const auto line = pending.substring (0, nl).trim();
            pending = pending.substring (nl + 1);

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
                        reportProgress (0.92f, "CONVERTING");
                    else
                        reportProgress (juce::jlimit (0.0f, 0.9f, pct * 0.9f), "DOWNLOADING");
                }
            }
            else
            {
                lastLine = line;
                const juce::File f (line);
                if (f.existsAsFile())
                    r.file = f;
            }
        }
    }

    proc->waitForProcessToFinish (5000);
    const auto exitCode = proc->getExitCode();

    r.ok = (exitCode == 0 && r.file.existsAsFile());
    if (r.ok)
        r.message = r.file.getFileName();
    else
        r.message = lastLine.isEmpty() ? juce::String ("Download fehlgeschlagen") : lastLine;
}

//==============================================================================
void Downloader::runConvert (Result& r)
{
    const auto outDir = dataDir ("imports");
    const auto out = outDir.getChildFile (pendingFile.getFileNameWithoutExtension() + ".wav");
    out.deleteFile();

    const auto ffmpeg = tools->getExecutable (ToolManager::Tool::ffmpeg);
    if (ffmpeg == juce::File())
    {
        r.message = "FFMPEG MISSING - CLICK INSTALL TOOLS";
        return;
    }

    juce::StringArray args {
        ffmpeg.getFullPathName(), "-y", "-hide_banner", "-loglevel", "error",
        "-i", pendingFile.getFullPathName(),
        "-vn", "-acodec", "pcm_s16le",
        out.getFullPathName()
    };

    reportProgress (0.5f, "CONVERTING");

    bool started = false;
    {
        const juce::ScopedLock sl (procLock);
        proc = std::make_unique<juce::ChildProcess>();
        started = proc->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr);
    }

    if (! started)
    {
        r.message = "FFMPEG COULD NOT BE STARTED - CLICK INSTALL TOOLS";
        return;
    }

    const auto output = proc->readAllProcessOutput().trim();
    proc->waitForProcessToFinish (10000);

    r.ok = (proc->getExitCode() == 0 && out.existsAsFile());
    r.file = out;

    if (r.ok)
        r.message = out.getFileName();
    else
        r.message = output.isEmpty() ? juce::String ("Konvertierung fehlgeschlagen")
                                     : output.fromLastOccurrenceOf ("\n", false, false);
}
