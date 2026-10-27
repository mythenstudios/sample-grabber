#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>

// Storage layout (never used on the audio thread):
//  - Cache root: <user data>/SampleGrabber/cache (test override SG_CACHE_DIR) with
//      downloads/  yt-dlp results          imports/  ffmpeg conversions
//      staging/    pre-rendered drag-out files, one subfolder per plugin instance
//    Everything in the cache may be deleted (CLEAR NOW, auto-clean), except files in use by a processor of this process.
//  - Export root: <Music>/Sample Grabber/<title>/ (test override SG_EXPORT_DIR). Files dragged into a DAW live here and
//    are NEVER deleted automatically (DAWs often only reference the dragged file).
namespace sg::storage
{
    juce::File appDataDir();                               // <user data>/SampleGrabber
    juce::File cacheRoot();                                // SG_CACHE_DIR or appDataDir()/cache
    juce::File cacheSubdir (const juce::String& sub);      // cacheRoot()/sub, created (+ marker, see ensureCacheRoot)
    constexpr const char* cacheMarkerName = ".sg-cache";

    // Creates the cache root and our marker file. The marker is only added to a folder that is new, empty or contains
    // nothing but our own subfolders (or is the default location), so a mistaken SG_CACHE_DIR never becomes cleanable.
    bool ensureCacheRoot();
    bool hasCacheMarker (const juce::File& root);
    // false (with reason) if root must never be cleaned: no marker, home, filesystem root, Music folder, user data folder,
    // export root or a parent of it
    bool isSafeCacheRoot (const juce::File& root, juce::String& whyNot);
    juce::File exportRoot();                               // SG_EXPORT_DIR or <Music>/Sample Grabber
    bool isInsideCache (const juce::File&);

    // Saved projects may still point to the pre-cache locations appDataDir()/downloads|imports/<name>.
    // Returns the matching file in the cache if the old one is gone, otherwise f unchanged.
    juce::File remapLegacyPath (const juce::File& f);

    // File-name safe title on every OS: Unicode letters/digits kept, only <>:"/\|?* and control characters replaced,
    // no leading/trailing dots or spaces, Windows reserved names (CON, NUL, COM1, ...) prefixed; max 60 characters; never empty
    juce::String sanitizeTitle (const juce::String&);
    // Title of a source file: our own export suffix ("_pad05_<hash>", "_sel_<hash>") removed, the yt-dlp id suffix
    // ("_<11 chars>") only for files in our downloads folder
    juce::String titleFromFile (const juce::File&);

    // Make dst a complete, independent copy of src (copy, never a hard link: destructive edits in a DAW must not change
    // the staging file). Written to a temp file and renamed only if dst is still absent; an existing dst is kept.
    bool copyIfAbsent (const juce::File& src, const juce::File& dst);
    // Our own leftovers "<name>.wav.<hex>.tmp" older than maxAgeMs in dir (interrupted exports)
    void removeStaleTemps (const juce::File& dir, juce::int64 maxAgeMs);

    // Registry of files that cleaning must not delete (loaded sample + BACK history of each processor).
    // LIMITATION: per process only. A Standalone cleaning its cache does not know what a DAW process has loaded
    // (that sample stays playable from RAM; a later project save relies on the embedded selection audio).
    void setInUse (const void* owner, const juce::StringArray& paths);
    juce::StringArray getInUse();
    bool isInUse (const juce::File&);   // normalised paths, symlinks resolved, case-insensitive where the OS is

    enum class AutoClean { off, days7, maxSize };
    constexpr juce::int64 maxCacheBytes = (juce::int64) 1024 * 1024 * 1024;   // 1 GB MAX
    AutoClean getAutoClean();          // settings.xml key "autoClean", default maxSize
    void setAutoClean (AutoClean);

    juce::String formatBytes (juce::int64);
}

// Cache maintenance on its own background thread: size, CLEAR NOW, auto-clean, one-time migration of the old
// downloads/imports folders. One object per process (juce::SharedResourcePointer). Change messages arrive on the message thread.
class CacheManager : public juce::ChangeBroadcaster,
                     private juce::Thread
{
public:
    CacheManager();
    ~CacheManager() override;

    void requestStartup();      // once per process: migrate old folders, then auto-clean with the saved setting
    void requestAutoClean();    // reads the setting on the calling thread
    void requestClearNow();
    void requestSize();

    juce::int64 getSize() const noexcept { return sizeBytes.load(); }   // -1 = not computed yet
    bool isBusy() const noexcept         { return busy.load(); }
    juce::String getLastNote() const;

    struct CleanResult { int deleted = 0, kept = 0; juce::int64 freed = 0; };
    // Blocking (worker thread or self test). clearAll = CLEAR NOW (mode ignored).
    static CleanResult clean (const juce::File& root, sg::storage::AutoClean mode, bool clearAll);
    static juce::int64 computeSize (const juce::File& root);
    static void migrateLegacy();

    // SG_CACHE_SELFTEST: needs SG_CACHE_DIR (scratch folder), returns 0 = ok
    static int runSelfTest();

private:
    void run() override;

    std::atomic<bool> wantStartup { false }, wantAuto { false }, wantClear { false }, wantSize { false };
    std::atomic<int> autoMode { (int) sg::storage::AutoClean::maxSize };
    std::atomic<bool> busy { false };
    std::atomic<juce::int64> sizeBytes { -1 };
    mutable juce::CriticalSection noteLock;
    juce::String lastNote;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CacheManager)
};
