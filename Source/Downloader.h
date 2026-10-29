#pragma once
#include <juce_core/juce_core.h>
#include "ToolManager.h"
#include "TimeRange.h"
#include "WinProcess.h"   // Windows only, empty elsewhere
#include <atomic>
#include <functional>
#include <memory>

// External processes (yt-dlp, ffmpeg) on a background thread - never on the audio thread!
// Tool paths come from the ToolManager (own bin folder, else PATH).
//  - probe():      URL -> yt-dlp metadata only (duration, title), nothing downloaded
//  - probeFile():  local file -> ffprobe duration
//  - grab():       URL -> yt-dlp -> WAV (optionally only a time range)
//  - importFile(): mp3/mp4/m4a/... (or any file with a range) -> ffmpeg -> WAV
class Downloader : private juce::Thread
{
public:
    struct Result
    {
        bool ok = false;
        juce::File file;
        juce::String message;
        double duration = -1.0;   // probe: seconds, < 0 = unknown (live, NA)
        juce::String title;       // probe (URL): title of the source
    };

    Downloader() : juce::Thread ("external-process") {}
    ~Downloader() override { cancel(); }

    // Both callbacks arrive on the message thread.
    // onProgress: 0..1 and a phase ("DOWNLOADING", "CONVERTING")
    // durationHint: probed length (< 0 unknown). A range on a source <= 30 min downloads the whole audio (reused if it is
    // already in the cache) and cuts locally; longer sources use yt-dlp's section download. deleteFullAfterCut: KEEP
    // DOWNLOADS off - the whole audio is deleted after the cut unless it is in use. The probe's info JSON of the same link
    // (< 10 min old) is reused via --load-info-json.
    void grab (const juce::String& url, const sg::range::Range& range, double durationHint, bool deleteFullAfterCut,
               std::function<void (float, juce::String)> onProgress,
               std::function<void (Result)> onDone);

    // pcmBits: sample format of the WAV (16, 24, 32 = float), e.g. to keep a 24-bit source at 24 bit when cutting a range
    void importFile (const juce::File& file, const sg::range::Range& range, int pcmBits,
                     std::function<void (float, juce::String)> onProgress,
                     std::function<void (Result)> onDone);

    // Length check before a grab/import (result: ok, duration, title, message on failure)
    void probe (const juce::String& url, std::function<void (Result)> onDone);
    void probeFile (const juce::File& file, std::function<void (Result)> onDone);

    // true from the start of a job until its result callback has been posted (the thread may still be winding down)
    bool isBusy() const { return jobActive.load(); }
    // Stops the running job (message thread): kills the whole process tree (yt-dlp is a bootloader + child, sections
    // run an ffmpeg grandchild), waits for the thread, no result callback. A download/convert job's partial files
    // (new in its cache folder since the job started) are deleted.
    void cancel();

    // Stereo at most: 5.1/7.1 are downmixed by ffmpeg's resampler (centre and surrounds included), mono stays mono
    static constexpr const char* channelFilter = "aformat=channel_layouts=mono|stereo";

    static constexpr double localCutMaxSeconds = 30.0 * 60.0;
    static bool cutsLocally (const sg::range::Range&, double duration);   // range via whole audio + local cut?

private:
    enum class Job { download, convert, probeUrl, probeLocal };

    void run() override;
    void runDownload (Result& r);
    void runConvert (Result& r);
    void runProbeUrl (Result& r);
    void runProbeFile (Result& r);
    bool startProcess (const juce::StringArray& args);
    bool claim();   // message thread: false if a job is still active; waits for a finished thread to exit
    void rememberJobDir (const juce::File& dir);   // message thread, before startThread: folder + its current files
    bool ownFfmpeg() const;                        // ffmpeg is our managed build (knows -rf64)
    bool cutWithFfmpeg (const juce::File& in, const juce::File& out, const sg::range::Range&, int bits, juce::String& err);
    void dropInfoJson();                           // delete the kept probe JSON (job thread finished or message thread)
    juce::File findCachedWhole (const juce::File& dir, const juce::String& id) const;   // finished whole audio of id, or {}
    void reportProgress (float p, const juce::String& phase);
    static juce::File dataDir (const juce::String& sub);

    Job job = Job::download;
    juce::String pendingUrl;
    juce::File pendingFile;
    sg::range::Range pendingRange;
    int pendingBits = 16;
    double pendingDuration = -1.0;
    bool pendingDeleteFull = false;
    juce::File pendingInfoJson;                    // probe JSON used by the running grab (empty = extract)
    juce::String pendingVideoId;

    // Info JSON of the last probe (cache/tmp). Written by the probe job, read/deleted by the message thread only when
    // no job runs (claim() joins the thread first) or by the download job that uses it.
    juce::File infoJson;
    juce::String infoJsonUrl, infoJsonId;
    juce::int64 infoJsonTime = 0;
    std::function<void (float, juce::String)> progressCb;
    std::function<void (Result)> doneCb;

    std::atomic<bool> jobActive { false };
    juce::CriticalSection procLock;
   #if JUCE_WINDOWS
    using Process = sg::WinProcess;               // Job Object: kill() ends the whole tree; UTF-8 env, exact argv quoting
   #else
    using Process = juce::ChildProcess;
   #endif
    std::unique_ptr<Process> proc;                 // started/destroyed under procLock, read by the job thread
    juce::StringArray procArgs;                    // argv of proc (identifies it in /proc for the tree kill)
    juce::File jobDir;                             // cache folder the job writes to (empty for probes)
    juce::StringArray jobPreexisting;              // file names in jobDir when the job started
    juce::int64 jobStartMs = 0;
    juce::String jobPattern;                       // "_<id>." or "_<id>_<tag>.": yt-dlp names (incl. .part/.temp) of this job
    juce::StringArray jobProduced;                 // exact paths this job wrote (yt-dlp result, cut), guarded by procLock
    void addProduced (const juce::File&);

    juce::SharedResourcePointer<ToolManager> tools;
};
