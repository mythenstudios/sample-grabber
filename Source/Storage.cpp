#include "Storage.h"
#include "UserSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
 #include <unistd.h>
#endif

namespace
{
    // Files modified more recently than this are never deleted or moved: a running grab (yt-dlp .part files,
    // possibly in another process), a staging file being written, a conversion that just finished.
    constexpr juce::int64 kProtectYoungMs = 2 * 60 * 1000;
    constexpr juce::int64 kStagingMaxAgeMs = 24 * 60 * 60 * 1000;   // staging leftovers (crashed instances) after 24 h
    constexpr juce::int64 kDays7Ms = (juce::int64) 7 * 24 * 60 * 60 * 1000;

    std::mutex inUseMutex;
    std::map<const void*, juce::StringArray>& inUseMap()
    {
        static std::map<const void*, juce::StringArray> m;
        return m;
    }

    std::atomic<bool> startupDone { false };   // once per process, even if the CacheManager is recreated

    juce::File envDir (const char* name)
    {
        const auto env = juce::SystemStats::getEnvironmentVariable (name, {}).trim();
        return env.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (env) : juce::File();
    }

    void logLine (const juce::String& s)
    {
        std::fprintf (stderr, "[SG] %s\n", s.toRawUTF8());
    }
}

namespace sg::storage
{
    juce::File appDataDir()
    {
        // Same folder as settings.xml and audio.xml
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SampleGrabber");
    }

    juce::File cacheRoot()
    {
        const auto env = envDir ("SG_CACHE_DIR");
        return env != juce::File() ? env : appDataDir().getChildFile ("cache");
    }

    juce::File cacheSubdir (const juce::String& sub)
    {
        ensureCacheRoot();
        auto d = cacheRoot().getChildFile (sub);
        d.createDirectory();
        return d;
    }

    bool hasCacheMarker (const juce::File& root)
    {
        return root.getChildFile (cacheMarkerName).existsAsFile();
    }

    bool ensureCacheRoot()
    {
        const auto root = cacheRoot();
        if (hasCacheMarker (root))
            return true;

        juce::String why;
        const bool isDefault = envDir ("SG_CACHE_DIR") == juce::File();
        bool onlyOurs = true;
        if (root.isDirectory())
            for (const auto& c : root.findChildFiles (juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no))
                if (! (c.isDirectory() && (c.getFileName() == "downloads" || c.getFileName() == "imports" || c.getFileName() == "staging")))
                    onlyOurs = false;

        if (! isDefault && ! onlyOurs)
        {
            logLine ("cache: " + root.getFullPathName() + " is not empty and has no " + cacheMarkerName + " - it will never be cleaned");
            root.createDirectory();
            return false;
        }
        if (! root.createDirectory())
            return false;
        // Unsafe locations (home, Music, ...) never get a marker either
        const bool unsafe = ! isSafeCacheRoot (root, why) && why != "no marker";
        if (unsafe)
        {
            logLine ("cache: refusing to mark " + root.getFullPathName() + " (" + why + ")");
            return false;
        }
        return root.getChildFile (cacheMarkerName).replaceWithText ("Sample Grabber cache. Everything in this folder may be deleted by the app.\n");
    }

    bool isSafeCacheRoot (const juce::File& root, juce::String& whyNot)
    {
        auto same = [] (const juce::File& a, const juce::File& b)
        {
            return a == b || (a.exists() && b.exists() && a.getLinkedTarget() == b.getLinkedTarget());
        };
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        const auto exp = exportRoot();

        if (root == juce::File() || root.getFullPathName().length() <= 3 || root.getParentDirectory() == root)
            whyNot = "filesystem root";
        else if (same (root, home))
            whyNot = "home folder";
        else if (same (root, music))
            whyNot = "music folder";
        else if (same (root, appDataDir()) || same (root, juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)))
            whyNot = "user data folder";
        else if (same (root, exp) || exp.isAChildOf (root))
            whyNot = "export folder or its parent";
        else if (home.isAChildOf (root))
            whyNot = "parent of the home folder";
        else if (! hasCacheMarker (root))
            whyNot = "no marker";
        else
            return true;
        return false;
    }

    juce::File exportRoot()
    {
        const auto env = envDir ("SG_EXPORT_DIR");
        if (env != juce::File())
            return env;
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Sample Grabber");
    }

    bool isInsideCache (const juce::File& f)
    {
        return f != juce::File() && f.isAChildOf (cacheRoot());
    }

    juce::File remapLegacyPath (const juce::File& f)
    {
        if (f == juce::File() || f.existsAsFile())
            return f;

        const auto parent = f.getParentDirectory();
        const auto sub = parent.getFileName();
        if ((sub == "downloads" || sub == "imports") && parent.getParentDirectory() == appDataDir())
        {
            const auto moved = cacheRoot().getChildFile (sub).getChildFile (f.getFileName());
            if (moved.existsAsFile())
                return moved;
        }
        return f;
    }

    juce::String sanitizeTitle (const juce::String& in)
    {
        // Replace only what is invalid on some OS; keep Unicode letters/digits and ordinary punctuation
        juce::String out;
        bool lastUnderscore = false;
        for (auto p = in.getCharPointer(); ! p.isEmpty(); ++p)
        {
            const juce::juce_wchar ch = *p;
            const bool invalid = ch < 32 || ch == 127 || juce::String ("<>:\"/\\|?*").containsChar (ch);
            if (! invalid)
            {
                out << juce::String::charToString (ch);
                lastUnderscore = (ch == '_');
            }
            else if (! lastUnderscore)
            {
                out << "_";
                lastUnderscore = true;
            }
        }

        // substring counts characters (code points), so the result stays valid UTF-8
        auto trimEdges = [] (juce::String t) { return t.trimCharactersAtStart (" ._").trimCharactersAtEnd (" ._"); };
        out = trimEdges (trimEdges (out).substring (0, 60));
        if (out.isEmpty())
            return "Untitled";

        // Windows reserved device names (also with an extension, e.g. "con.txt")
        const auto stem = out.upToFirstOccurrenceOf (".", false, false).trim().toUpperCase();
        static const juce::StringArray reserved { "CON", "PRN", "AUX", "NUL",
                                                  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                                  "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        if (reserved.contains (stem))
            out = "_" + out;
        return out;
    }

    juce::String titleFromFile (const juce::File& f)
    {
        auto n = f.getFileNameWithoutExtension();

        // Own export names: <title>_padNN_<8 hex> or <title>_sel_<8 hex> (pad dragged back into the plugin)
        auto isHex = [] (const juce::String& s)
        {
            return s.length() == 8 && s.containsOnly ("0123456789abcdef");
        };
        for (;;)
        {
            const int last = n.lastIndexOfChar ('_');
            if (last <= 0 || ! isHex (n.substring (last + 1)))
                break;
            const auto head = n.substring (0, last);
            const int prev = head.lastIndexOfChar ('_');
            const auto tag = head.substring (prev + 1);
            const bool padTag = tag.length() == 5 && tag.startsWith ("pad") && tag.substring (3).containsOnly ("0123456789");
            if (prev <= 0 || ! (tag == "sel" || padTag))
                break;
            n = head.substring (0, prev);
        }

        // yt-dlp appends "_<11-char id>" - only for our own downloads (a local "My_Song_Final_Mix12" keeps its name)
        const auto parent = f.getParentDirectory();
        const bool fromDownloads = parent == cacheRoot().getChildFile ("downloads") || parent == appDataDir().getChildFile ("downloads");
        const int us = n.lastIndexOfChar ('_');
        if (fromDownloads && us > 0 && n.length() - us - 1 == 11)
            n = n.substring (0, us);

        return sanitizeTitle (n);
    }

    bool copyIfAbsent (const juce::File& src, const juce::File& dst)
    {
        if (dst.existsAsFile())
            return true;
        if (! src.existsAsFile() || ! dst.getParentDirectory().createDirectory())
            return false;

        const auto tmp = dst.getSiblingFile (dst.getFileName() + "." + juce::String::toHexString (juce::Random::getSystemRandom().nextInt()) + ".tmp");
        if (! src.copyFileTo (tmp))
        {
            tmp.deleteFile();
            return false;
        }
        if (dst.existsAsFile())   // another instance was faster: keep its file (a DAW may already reference it)
        {
            tmp.deleteFile();
            return true;
        }
        if (! tmp.moveFileTo (dst))
        {
            tmp.deleteFile();
            return false;
        }
        return true;
    }

    void removeStaleTemps (const juce::File& dir, juce::int64 maxAgeMs)
    {
        if (! dir.isDirectory())
            return;
        const auto now = juce::Time::currentTimeMillis();
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav.*.tmp", juce::File::FollowSymlinks::no))
            if (now - f.getLastModificationTime().toMilliseconds() > maxAgeMs)
                f.deleteFile();
    }

    void setInUse (const void* owner, const juce::StringArray& paths)
    {
        const std::lock_guard<std::mutex> g (inUseMutex);
        if (paths.isEmpty())
            inUseMap().erase (owner);
        else
            inUseMap()[owner] = paths;
    }

    juce::StringArray getInUse()
    {
        const std::lock_guard<std::mutex> g (inUseMutex);
        juce::StringArray all;
        for (const auto& kv : inUseMap())
            all.addArray (kv.second);
        return all;
    }

    bool isInUse (const juce::File& f)
    {
        const auto target = f.isSymbolicLink() ? f.getLinkedTarget() : f;
        for (const auto& p : getInUse())
        {
            const juce::File u (p);   // juce::File normalises the path; == is case-insensitive on Windows/macOS
            if (u == f || u == target || (u.isSymbolicLink() && u.getLinkedTarget() == target))
                return true;
        }
        return false;
    }

    AutoClean getAutoClean()
    {
        const auto v = sg::settings::get ("autoClean", "1gb");
        if (v == "off") return AutoClean::off;
        if (v == "7d")  return AutoClean::days7;
        return AutoClean::maxSize;
    }

    void setAutoClean (AutoClean m)
    {
        sg::settings::set ("autoClean", m == AutoClean::off ? "off" : m == AutoClean::days7 ? "7d" : "1gb");
    }

    juce::String formatBytes (juce::int64 b)
    {
        if (b < 0)
            return "...";
        if (b < 1024 * 1024)
            return juce::String ((juce::int64) std::llround ((double) b / 1024.0)) + " KB";
        if (b < (juce::int64) 1024 * 1024 * 1024)
            return juce::String ((juce::int64) std::llround ((double) b / (1024.0 * 1024.0))) + " MB";
        return juce::String ((double) b / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
    }
}

//==============================================================================
CacheManager::CacheManager() : juce::Thread ("cache-manager")
{
    startThread (juce::Thread::Priority::low);
}

CacheManager::~CacheManager()
{
    signalThreadShouldExit();
    notify();
    stopThread (10000);
}

void CacheManager::requestStartup()
{
    if (startupDone.exchange (true))
        return;
    autoMode = (int) sg::storage::getAutoClean();
    wantStartup = true;
    notify();
}

void CacheManager::requestAutoClean()
{
    autoMode = (int) sg::storage::getAutoClean();
    wantAuto = true;
    notify();
}

void CacheManager::requestClearNow()
{
    wantClear = true;
    notify();
}

void CacheManager::requestSize()
{
    wantSize = true;
    notify();
}

juce::String CacheManager::getLastNote() const
{
    const juce::ScopedLock sl (noteLock);
    return lastNote;
}

void CacheManager::run()
{
    while (! threadShouldExit())
    {
        if (! (wantStartup.load() || wantAuto.load() || wantClear.load() || wantSize.load()))
            wait (-1);
        if (threadShouldExit())
            break;

        busy = true;
        sendChangeMessage();
        const auto root = sg::storage::cacheRoot();
        juce::String note;

        if (wantStartup.exchange (false))
        {
            // Size first (cheap), so an overlay opened right at startup shows a number instead of "..."
            sizeBytes = computeSize (root);
            busy = false;
            sendChangeMessage();

            // Do not compete with editor/sample loading right at startup (sleep, a notify() must not cut this short)
            for (int i = 0; i < 20 && ! threadShouldExit(); ++i)
                juce::Thread::sleep (100);
            if (threadShouldExit())
                break;
            busy = true;
            sendChangeMessage();
            sg::storage::ensureCacheRoot();
            migrateLegacy();
            wantAuto = true;
        }

        if (wantClear.exchange (false))
        {
            wantAuto = false;   // clearing covers it
            const auto r = clean (root, sg::storage::AutoClean::off, true);
            note = "CLEARED " + juce::String (r.deleted) + " FILES, " + sg::storage::formatBytes (r.freed)
                   + (r.kept > 0 ? ", " + juce::String (r.kept) + " IN USE KEPT" : juce::String());
        }
        else if (wantAuto.exchange (false))
        {
            const auto r = clean (root, (sg::storage::AutoClean) autoMode.load(), false);
            if (r.deleted > 0)
                note = "AUTO-CLEAN: " + juce::String (r.deleted) + " FILES, " + sg::storage::formatBytes (r.freed);
        }

        wantSize = false;
        sizeBytes = computeSize (root);

        if (note.isNotEmpty())
        {
            const juce::ScopedLock sl (noteLock);
            lastNote = note;
        }
        busy = false;
        sendChangeMessage();
    }
}

juce::int64 CacheManager::computeSize (const juce::File& root)
{
    if (! root.isDirectory())
        return 0;

    juce::int64 total = 0;
    for (const auto& f : root.findChildFiles (juce::File::findFiles, true, "*", juce::File::FollowSymlinks::no))
        total += f.getSize();
    return total;
}

CacheManager::CleanResult CacheManager::clean (const juce::File& root, sg::storage::AutoClean mode, bool clearAll)
{
    CleanResult res;
    if (! root.isDirectory())
        return res;

    juce::String whyNot;
    if (! sg::storage::isSafeCacheRoot (root, whyNot))
    {
        logLine ("cache clean refused for " + root.getFullPathName() + ": " + whyNot);
        return res;
    }

    const auto now = juce::Time::currentTimeMillis();
    const auto staging = root.getChildFile ("staging");
    const char* modeName = clearAll ? "CLEAR NOW" : mode == sg::storage::AutoClean::off ? "OFF"
                         : mode == sg::storage::AutoClean::days7 ? "7 DAYS" : "1 GB MAX";

    struct Entry { juce::File file; juce::int64 size, mtime; bool isProtected, isStaging; };
    std::vector<Entry> entries;
    juce::int64 total = 0;

    // Only regular files below our own cache root, symlinks are not followed (never delete anything outside)
    for (const auto& f : root.findChildFiles (juce::File::findFiles, true, "*", juce::File::FollowSymlinks::no))
    {
        if (! f.isAChildOf (root))
            continue;
        const auto mtime = f.getLastModificationTime().toMilliseconds();
        const bool young = now - mtime < kProtectYoungMs;
        if (f.getFileName() == sg::storage::cacheMarkerName)
            continue;
        const bool used = sg::storage::isInUse (f);
        entries.push_back ({ f, f.getSize(), mtime, young || used, f.isAChildOf (staging) });
        total += entries.back().size;
    }

    auto remove = [&] (Entry& e, const char* why)
    {
        if (e.file.deleteFile())
        {
            ++res.deleted;
            res.freed += e.size;
            total -= e.size;
            logLine (juce::String ("cache clean (") + modeName + "): deleted " + e.file.getFullPathName()
                     + " (" + sg::storage::formatBytes (e.size) + ", " + why + ")");
            e.size = 0;
            e.isProtected = true;   // handled
        }
    };

    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.mtime < b.mtime; });

    for (auto& e : entries)
    {
        if (e.isProtected)
        {
            if (clearAll)
                ++res.kept;
            continue;
        }

        if (clearAll)
            remove (e, "clear now");
        else if (e.isStaging && now - e.mtime > kStagingMaxAgeMs)
            remove (e, "staging older than 24 h");
        else if (mode == sg::storage::AutoClean::days7 && now - e.mtime > kDays7Ms)
            remove (e, "older than 7 days");
    }

    if (! clearAll && mode == sg::storage::AutoClean::maxSize)
        for (auto& e : entries)   // oldest first
        {
            if (total <= sg::storage::maxCacheBytes)
                break;
            if (! e.isProtected)
                remove (e, "over 1 GB, oldest first");
        }

    // Empty staging folders of instances that no longer exist (live instances recreate theirs before writing)
    if (staging.isDirectory())
        for (const auto& d : staging.findChildFiles (juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::no))
            if (d.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0
                && (clearAll || now - d.getLastModificationTime().toMilliseconds() > kStagingMaxAgeMs))
                d.deleteFile();

    if (res.deleted > 0 || clearAll)
        logLine (juce::String ("cache clean (") + modeName + "): " + juce::String (res.deleted) + " files, "
                 + sg::storage::formatBytes (res.freed) + " freed, cache now " + sg::storage::formatBytes (total)
                 + (res.kept > 0 ? ", " + juce::String (res.kept) + " protected (in use or < 2 min old)" : juce::String()));
    return res;
}

void CacheManager::migrateLegacy()
{
    // Only the real default location; with SG_CACHE_DIR nothing is moved (tests never touch the user's folders)
    if (juce::SystemStats::getEnvironmentVariable ("SG_CACHE_DIR", {}).trim().isNotEmpty())
        return;

    const auto now = juce::Time::currentTimeMillis();
    for (auto sub : { "downloads", "imports" })
    {
        const auto oldDir = sg::storage::appDataDir().getChildFile (sub);
        if (! oldDir.isDirectory())
            continue;

        const auto newDir = sg::storage::cacheSubdir (sub);
        int moved = 0, skipped = 0;
        // Flat folders; plain rename within the same parent folder (same volume), no copying
        for (const auto& f : oldDir.findChildFiles (juce::File::findFiles, false, "*", juce::File::FollowSymlinks::no))
        {
            const auto target = newDir.getChildFile (f.getFileName());
            if (target.exists() || now - f.getLastModificationTime().toMilliseconds() < kProtectYoungMs
                || ! f.moveFileTo (target))
            {
                ++skipped;
                continue;
            }
            ++moved;
        }

        if (oldDir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            oldDir.deleteFile();   // only if empty

        if (moved > 0 || skipped > 0)
            logLine ("cache migration: " + juce::String (moved) + " files moved " + oldDir.getFullPathName() + " -> "
                     + newDir.getFullPathName() + (skipped > 0 ? ", " + juce::String (skipped) + " left in place" : juce::String()));
    }
}

//==============================================================================
int CacheManager::runSelfTest()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("SG_CACHE_DIR", {}).trim();
    if (env.isEmpty())
    {
        std::fprintf (stderr, "[SG-CACHE] SG_CACHE_DIR must point to a scratch folder\n");
        return 2;
    }

    const auto root = sg::storage::cacheRoot();
    const auto now = juce::Time::currentTimeMillis();
    constexpr juce::int64 MB = 1024 * 1024;

    // Never wipe a folder that is not ours: it must be new, empty or carry our marker
    {
        juce::String why;
        const bool empty = ! root.exists() || root.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0;
        if (! empty && ! sg::storage::hasCacheMarker (root))
        {
            std::fprintf (stderr, "[SG-CACHE] refusing: %s is not empty and has no marker\n", root.getFullPathName().toRawUTF8());
            return 2;
        }
        if (! sg::storage::isSafeCacheRoot (root, why) && why != "no marker")
        {
            std::fprintf (stderr, "[SG-CACHE] refusing: %s (%s)\n", root.getFullPathName().toRawUTF8(), why.toRawUTF8());
            return 2;
        }
    }

    // Fake cache: name, size MB, age in hours
    struct Fake { const char* path; int mb; double hours; };
    const Fake fakes[] = {
        { "downloads/old_10d.wav",     300, 240.0 },
        { "downloads/old_8d.wav",      300, 192.0 },
        { "downloads/mid_3d.wav",      300,  72.0 },
        { "downloads/inuse_9d.wav",    300, 216.0 },   // registered as in use -> never deleted
        { "downloads/new_1min.part",   300,   0.01 },  // younger than 2 min -> never deleted
        { "imports/conv_2d.wav",       100,  48.0 },
        { "staging/inst-dead/x.wav",     1,  30.0 },   // staging leftover > 24 h
        { "staging/inst-live/y.wav",     1,   1.0 },
    };

    auto build = [&]
    {
        if (sg::storage::hasCacheMarker (root) || ! root.exists())
            root.deleteRecursively();
        sg::storage::ensureCacheRoot();
        for (const auto& fk : fakes)
        {
            const auto f = root.getChildFile (fk.path);
            f.getParentDirectory().createDirectory();
            juce::FileOutputStream os (f);
            if (os.openedOk())
            {
                os.setPosition ((juce::int64) fk.mb * MB - 1);   // sparse file, cheap
                os.writeByte (0);
            }
            os.flush();
            f.setLastModificationTime (juce::Time (now - (juce::int64) (fk.hours * 3600.0 * 1000.0)));
        }
    };

    auto listing = [&] (const char* title)
    {
        std::fprintf (stderr, "[SG-CACHE] %s: size %s\n", title, sg::storage::formatBytes (computeSize (root)).toRawUTF8());
        for (const auto& fk : fakes)
            std::fprintf (stderr, "[SG-CACHE]   %-28s %s\n", fk.path, root.getChildFile (fk.path).existsAsFile() ? "present" : "DELETED");
    };

    int failures = 0;
    auto expect = [&] (const char* path, bool present)
    {
        if (root.getChildFile (path).existsAsFile() != present)
        {
            std::fprintf (stderr, "[SG-CACHE] FAIL: %s should be %s\n", path, present ? "present" : "deleted");
            ++failures;
        }
    };

    const juce::String inUsePath = root.getChildFile ("downloads/inuse_9d.wav").getFullPathName();
    static int owner = 0;

    using AC = sg::storage::AutoClean;
    for (const auto mode : { AC::off, AC::days7, AC::maxSize })
    {
        build();
        sg::storage::setInUse (&owner, { inUsePath });
        const char* name = mode == AC::off ? "OFF" : mode == AC::days7 ? "7 DAYS" : "1 GB MAX";
        std::fprintf (stderr, "[SG-CACHE] ---- mode %s ----\n", name);
        clean (root, mode, false);
        listing (name);

        expect ("downloads/inuse_9d.wav", true);
        expect ("downloads/new_1min.part", true);
        expect ("staging/inst-dead/x.wav", false);
        expect ("staging/inst-live/y.wav", true);
        if (mode == AC::off)   { expect ("downloads/old_10d.wav", true); expect ("downloads/old_8d.wav", true); }
        if (mode == AC::days7) { expect ("downloads/old_10d.wav", false); expect ("downloads/old_8d.wav", false); expect ("downloads/mid_3d.wav", true); }
        if (mode == AC::maxSize)
        {
            // 1602 MB total (in use and young files count but are protected): dead staging, then old_10d, (inuse_9d
            // skipped), old_8d -> 1001 MB <= 1024 MB, stop. mid_3d and conv_2d stay.
            expect ("downloads/old_10d.wav", false);
            expect ("downloads/old_8d.wav", false);
            expect ("downloads/mid_3d.wav", true);
            expect ("imports/conv_2d.wav", true);
        }
    }

    build();
    sg::storage::setInUse (&owner, { inUsePath });
    std::fprintf (stderr, "[SG-CACHE] ---- CLEAR NOW ----\n");
    clean (root, AC::off, true);
    listing ("CLEAR NOW");
    expect ("downloads/inuse_9d.wav", true);
    expect ("downloads/new_1min.part", true);
    expect ("downloads/old_10d.wav", false);
    expect ("imports/conv_2d.wav", false);

    // Marker missing -> nothing may be deleted, not even by CLEAR NOW
    build();
    root.getChildFile (sg::storage::cacheMarkerName).deleteFile();
    std::fprintf (stderr, "[SG-CACHE] ---- CLEAR NOW without marker ----\n");
    const auto noMarker = clean (root, AC::off, true);
    if (noMarker.deleted != 0)
    {
        std::fprintf (stderr, "[SG-CACHE] FAIL: deleted %d files without marker\n", noMarker.deleted);
        ++failures;
    }
    expect ("downloads/old_10d.wav", true);
    expect ("staging/inst-dead/x.wav", true);

    // Unsafe roots are refused even with a marker check
    {
        juce::String why;
        for (const auto& bad : { juce::File::getSpecialLocation (juce::File::userHomeDirectory), juce::File ("/"),
                                 juce::File::getSpecialLocation (juce::File::userMusicDirectory), sg::storage::exportRoot(),
                                 sg::storage::exportRoot().getParentDirectory() })
        {
            const bool safe = sg::storage::isSafeCacheRoot (bad, why);
            std::fprintf (stderr, "[SG-CACHE] safe(%s) = %d (%s)\n", bad.getFullPathName().toRawUTF8(), safe ? 1 : 0, why.toRawUTF8());
            if (safe)
                ++failures;
        }
    }

    sg::storage::setInUse (&owner, {});
    root.getChildFile (sg::storage::cacheMarkerName).replaceWithText ("x");   // ours again -> allowed to remove
    root.deleteRecursively();

    // Title helper
    const auto dl = root.getChildFile ("downloads");
    const auto local = juce::File::getCurrentWorkingDirectory();
    const std::pair<juce::File, juce::String> names[] = {
        { dl.getChildFile ("Adele_-_Rolling_in_the_Deep_rYEDA3JcQqw.wav"),   "Adele_-_Rolling_in_the_Deep" },
        { local.getChildFile ("My_Song_Final_Mix12.wav"),                    "My_Song_Final_Mix12" },   // not a download: keep
        { local.getChildFile ("My Song_pad05_0a1b2c3d.wav"),                 "My Song" },
        { local.getChildFile ("My_Song_pad05_0a1b2c3d_sel_ffffffff.wav"),    "My_Song" },
        { local.getChildFile (juce::String::fromUTF8 ("\xc3\x9c" "ber: Track?.wav")), juce::String::fromUTF8 ("\xc3\x9c" "ber_ Track") },
        { local.getChildFile ("con.wav"),                                    "_con" },
        { local.getChildFile ("LPT9.mix.wav"),                               "_LPT9.mix" },
        { local.getChildFile ("trailing dots... .wav"),                      "trailing dots" },
        { local.getChildFile ("___.wav"),                                    "Untitled" },
    };
    for (const auto& [file, want] : names)
    {
        const auto got = sg::storage::titleFromFile (file);
        std::fprintf (stderr, "[SG-CACHE] title(%s) = '%s'%s\n", file.getFileName().toRawUTF8(), got.toRawUTF8(),
                      got == want ? "" : (" FAIL, expected '" + want + "'").toRawUTF8());
        if (got != want)
            ++failures;
    }

    std::fprintf (stderr, "[SG-CACHE] %s\n", failures == 0 ? "ALL OK" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
