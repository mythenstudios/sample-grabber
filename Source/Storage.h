// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "TimeRange.h"
#include <atomic>

// Storage layout (never used on the audio thread):
//  - Cache root: <user data>/SampleGrabber/cache (Windows: Local AppData; user setting, test override SG_CACHE_DIR) with
//      downloads/  yt-dlp results          imports/  ffmpeg conversions
//      staging/    pre-rendered drag-out files, one subfolder per plugin instance       tmp/  probe info JSONs
//    Everything in these four subfolders may be deleted (CLEAR NOW, auto-clean), except files in use by a processor of
//    this process. Files directly in the root are never deleted.
//  - Export root: <Music>/Sample Grabber/<title>/ (user setting, test override SG_EXPORT_DIR). Files dragged into a DAW live here and
//    are NEVER deleted automatically (DAWs often only reference the dragged file).
namespace sg::storage
{
    juce::File appDataDir();                               // <user data>/SampleGrabber

    // Folder settings. Priority: env var (SG_EXPORT_DIR / SG_CACHE_DIR) > stored setting > default.
    // Stored: Windows registry HKCU\Software\Mythen Studios\Sample Grabber, REG_SZ "ExportDir"/"CacheDir" (written by
    // the installer and by CHANGE in the settings overlay); other systems settings.xml keys "exportDir"/"cacheDir".
    // An empty or non-absolute stored value counts as not set (default). Any thread except audio.
    enum class Folder { exports, cache };
    juce::File defaultFolder (Folder);
    juce::String storedFolder (Folder);                    // raw stored value (trimmed), empty = none
    bool setStoredFolder (Folder, const juce::String& absPath);   // empty = remove (default again); message thread
    bool folderFromEnv (Folder);                           // env var set -> it wins, CHANGE has no effect
    juce::File resolveFolder (Folder);                     // env > stored > default, evaluated on every call
    // Checks a folder typed/picked by the user: absolute, creatable, not a filesystem root; for the cache cacheFolderFor
    // (not home/Music/user data/export/tools folder; a non-empty foreign folder gets a subfolder "Sample Grabber Cache",
    // never mark or clean foreign files). Creates it. Returns the folder to store or an error text.
    juce::Result checkChosenFolder (Folder, const juce::String& typed, juce::File& result);
    // Cache rule shared by CHANGE and stored values (registry/settings.xml, e.g. written by the installer; env vars are
    // not checked): unsafe folder (see isSafeCacheRoot, tools folder) -> error; a folder that is neither empty nor marked
    // -> "<dir>/Sample Grabber Cache" (error if that one holds foreign files too). Does not create anything.
    juce::Result cacheFolderFor (const juce::File& dir, juce::File& result);

    // resolveFolder (cache), read ONCE per process (change = restart) and marked right away (ensureCacheRoot). A stored
    // folder that cannot be marked is replaced by the default.
    juce::File cacheRoot();
    // cacheRoot()/sub, created only if the root carries the marker (or comes from SG_CACHE_DIR); else logged, not created
    juce::File cacheSubdir (const juce::String& sub);
    constexpr const char* cacheMarkerName = ".sg-cache";

    // Creates root and our marker file. The marker is only added to a folder that is new or completely empty, or with
    // allowAnyContent (built-in default location, old installs); never to unsafe folders. true = marker present.
    bool markCacheRoot (const juce::File& root, bool allowAnyContent);
    // markCacheRoot (cacheRoot()), any content allowed only for the default location, so a mistaken SG_CACHE_DIR never
    // becomes cleanable.
    bool ensureCacheRoot();
    bool hasCacheMarker (const juce::File& root);
    // false (with reason) if root must never be cleaned: no marker, home, filesystem root, Music folder, user data folder,
    // export root or a parent of it, tools folder or a parent of it
    bool isSafeCacheRoot (const juce::File& root, juce::String& whyNot);
    juce::File exportRoot();                               // resolveFolder (exports), every call (change applies at once)
    bool isInsideCache (const juce::File&);

    // Saved projects may still point to the pre-cache locations appDataDir()/downloads|imports/<name>.
    // Returns the matching file in the cache if the old one is gone, otherwise f unchanged.
    juce::File remapLegacyPath (const juce::File& f);

    // File-name safe title on every OS: Unicode letters/digits kept, only <>:"/\|?* and control characters replaced,
    // no leading/trailing dots or spaces, Windows reserved names (CON, NUL, COM1, ...) prefixed; max 60 characters; never empty
    juce::String sanitizeTitle (const juce::String&);
    // Title for display/state only (yt-dlp title, project file): control characters (< 0x20, 0x7F) removed, trimmed,
    // at most 300 characters (code points). Unicode kept. May be empty.
    juce::String sanitizeDisplayTitle (const juce::String&);
    // Title of a source file: our own export suffix ("_pad05_<hash>", "_sel_<hash>") removed, the yt-dlp id suffix
    // ("_<11 chars>") only for files in our downloads folder
    juce::String titleFromFile (const juce::File&);
    // true for files directly in our downloads/ or imports/ folder (also the pre-cache locations)
    bool isCacheSource (const juce::File&);
    // File name without extension; for cache sources the range tag "_r<from>-<to>" (sg::range::fileTag) is removed
    // and returned in rangeOut (unset if there is none). Other files: name unchanged, no range.
    juce::String stripRangeTag (const juce::File&, sg::range::Range* rangeOut = nullptr);

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

    // KEEP DOWNLOADS (settings.xml key "keepDownloads", default on). Off: a grab/import result in our cache is deleted
    // once it has been loaded into RAM (the plugin state then relies on the embedded selection + link).
    bool getKeepDownloads();
    void setKeepDownloads (bool);
    // Deletes f only if it is a cache source (downloads/imports) under a safe, marked cache root. Message thread.
    bool deleteCacheSource (const juce::File& f);

    // Video ids from yt-dlp probes (any thread). Only ids matching [A-Za-z0-9_-]{6,} are used for file matching.
    bool isValidVideoId (const juce::String&);
    // "<title>_<11-char YouTube id>" (the id itself may contain '_')
    bool hasVideoIdTail (const juce::String& name);
    void rememberVideoId (const juce::String& url, const juce::String& id);   // per process, small (oldest dropped)
    juce::String videoIdFor (const juce::String& url);
    // Whole-audio downloads in flight, per video id, process-wide: a second job for the same id waits, then reuses the file
    bool tryBeginFetch (const juce::String& id);
    void endFetch (const juce::String& id);
    // Name parts of files a running job of this process writes (e.g. "_<id>." for yt-dlp outputs, the cut's file name)
    void setJobFiles (const void* owner, const juce::StringArray& nameParts);
    // true if f is being written: yt-dlp/ffmpeg intermediate (.part, .temp., .ytdl, .tmp), a file of a running job or
    // in-flight video id of this process. CLEAR NOW keeps these (plus anything modified in the last 5 s).
    bool isBeingWritten (const juce::File& f);

    // Probe info JSONs (cacheRoot/tmp/probe-*.info.json): the cleaner keeps them 15 min, the startup sweep removes
    // those older than 10 min (stream URLs in them have expired by then)
    bool isProbeJson (const juce::File&);
    void sweepProbeJsons (const juce::File& root, juce::int64 maxAgeMs);

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

    struct CleanResult { int deleted = 0, kept = 0, keptInUse = 0, keptBusy = 0; juce::int64 freed = 0; };
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
