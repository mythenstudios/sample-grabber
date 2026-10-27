#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>

// Install and update external tools (yt-dlp, deno, ffmpeg + ffprobe) ourselves.
//  - Official sources only (GitHub releases of yt-dlp/yt-dlp, denoland/deno, BtbN/FFmpeg-Builds), verified via SHA-256.
//    github.com URLs only, not the REST API (60 requests/h per IP).
//  - Folder: <user data folder>/SampleGrabber/bin (Windows: LOCALAPPDATA), test override SG_TOOLS_DIR.
//  - One object per process (juce::SharedResourcePointer<ToolManager>), installations protected across processes
//    by juce::InterProcessLock (VST3 and standalone at the same time).
//  - All network/file work runs on its own background thread, never on the message or audio thread.
//  - No automatic download when a tool is missing: only the user starts that (installMissing).
//    Automatic (at most once per 24 h): version check, yt-dlp update if it already lives in our own folder.
class ToolManager : public juce::ChangeBroadcaster,
                    private juce::Thread
{
public:
    enum class Tool { ytdlp = 0, deno, ffmpeg };
    static constexpr int numTools = 3;

    enum class State { missing, ok, working, error };

    struct Info
    {
        State state = State::missing;
        juce::File path;              // resolved executable (own folder or PATH), empty = missing
        bool own = false;             // true = lives in our own bin folder
        juce::String version;         // installed version (own folder, from tools.xml), "PATH" otherwise
        juce::String build;           // ffmpeg: build date of the archive
        juce::String sha256;          // checksum of the installed download
        juce::int64 installedAt = 0;  // ms since epoch
        juce::String latest;          // latest known version (after a check)
        bool updateAvailable = false;
        juce::String message;         // last error or result of the probe ("yt-dlp 2026.10.26")
        bool failed = false;          // last install/update attempt for this tool failed
    };

    ToolManager();
    ~ToolManager() override;

    static juce::File toolsDir();
    static juce::String toolName (Tool);            // "yt-dlp", "deno", "ffmpeg"
    static bool isPlatformSupported();

    // Re-read disk state + tools.xml (cheap: file existence only). Thread-safe.
    void refresh();
    Info getInfo (Tool) const;
    juce::File getExecutable (Tool) const;          // empty = missing; ffmpeg -> ffmpeg binary (ffprobe sits next to it)
    bool needsInstall() const;                      // yt-dlp or ffmpeg missing -> GRAB becomes INSTALL TOOLS
    bool anyMissing() const;                        // any tool missing (deno too, if the rest comes from PATH)
    bool needsAttention() const;                    // update available or last attempt failed (red dot on the gear)
    juce::int64 getLastCheck() const;               // ms since epoch, 0 = never

    // User setting (settings.xml): update yt-dlp automatically on the daily check (default on)
    static bool isAutoUpdateYtDlp();
    static void setAutoUpdateYtDlp (bool);

    // Start a run on the background thread; false = one is already running or platform not supported.
    bool installMissing();
    bool updateAll (bool force);
    bool checkForUpdates();
    void requestAutoCheck();                        // when the editor opens; silent, at most once per 24 h

    bool isWorking() const noexcept                 { return working.load(); }
    bool showsProgress() const noexcept             { return working.load() && visible.load(); }
    float getProgress() const noexcept              { return progress.load(); }   // 0..1, < 0 = indeterminate
    juce::String getStatusText() const;
    juce::String getLastNote() const;               // result of the last run, empty = nothing to show
    bool lastNoteIsError() const;
    int getRunCounter() const noexcept              { return runCounter.load(); }

    // Grab/import (downloader thread): nothing is replaced during a run and vice versa.
    bool tryBeginExternalJob();
    void endExternalJob();

    // SG_TOOLS_SELFTEST: blocking test run without GUI, output on stderr, return 0 = ok
    static int runSelfTest (const juce::String& mode);

private:
    enum class Job { none, installMissing, update, updateForce, check, autoCheck };

    struct Plan
    {
        Tool tool = Tool::ytdlp;
        juce::String assetName, assetUrl;   // fixed tag URL on github.com (BtbN: rolling "latest")
        juce::String sumsUrl, sumsText;     // checksum file; sumsText already loaded (ffmpeg)
        juce::String version;
    };

    void run() override;
    bool startJob (Job, bool showProgress);

    struct Staged
    {
        juce::File tmpDir;
        juce::Array<std::pair<juce::File, juce::String>> files;   // (file in tmp, target name in bin folder)
        juce::String sha, probe;
    };

    void doInstall (const juce::Array<Tool>& targets, juce::String& note, bool& isError, int maxWaitMs);
    void doCheck (juce::String& note, bool& isError);
    juce::String stateSummary() const;
    bool waitForExternalJobs (int maxWaitMs);
    void releaseExternalGate();

    static bool useCurl();
    juce::Result latestTag (const juce::String& repo, juce::String& tag);
    juce::int64 headSize (const juce::String& url);
    juce::Result makePlan (Tool, Plan&);
    juce::Result getPlan (Tool, Plan&, bool refetch);   // cached per run
    juce::Result prepareOne (const Plan&, const juce::String& step, Staged&);
    juce::Result commitOne (const Plan&, const Staged&);
    juce::Result download (const juce::String& url, const juce::File& dest, juce::int64 expectedSize,
                           std::function<void (juce::int64)> onBytes);
    int runTool (const juce::StringArray& args, juce::String& output, int timeoutMs);

    void setStatus (const juce::String& text, float p);
    void setToolState (Tool, State, const juce::String& message);
    void writeToolRecord (const Plan&, const juce::String& sha);
    std::unique_ptr<juce::XmlElement> loadXml() const;
    bool toolSelected (Tool) const;                 // SG_TOOLS_ONLY (test filter)

    mutable juce::CriticalSection lock;             // infos, statusText, lastNote
    Info infos[numTools];
    State jobState[numTools] { State::missing, State::missing, State::missing };
    juce::String jobMessage[numTools];
    juce::String statusText, lastNote;
    juce::int64 lastCheck = 0;
    bool lastError = false;

    std::atomic<Job> pendingJob { Job::none };
    std::atomic<bool> working { false }, visible { false };
    std::atomic<float> progress { -1.0f };
    std::atomic<int> runCounter { 0 };
    std::atomic<bool> autoCheckRequested { false };
    juce::uint32 lastNotify = 0;

    Plan planCache[numTools];                       // worker thread only
    bool planCached[numTools] { false, false, false };

    juce::CriticalSection gateLock;                 // externalJobs / replacing
    int externalJobs = 0;
    bool replacing = false;

    juce::CriticalSection procLock;
    std::unique_ptr<juce::ChildProcess> proc;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToolManager)
};
