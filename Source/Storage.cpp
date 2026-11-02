#include "Storage.h"
#include "ToolManager.h"
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

    constexpr juce::int64 kProbeJsonKeepMs = 15 * 60 * 1000;

    std::mutex idMutex;
    std::vector<std::pair<juce::String, juce::String>>& urlIds()   // url -> video id, newest last
    {
        static std::vector<std::pair<juce::String, juce::String>> v;
        return v;
    }
    juce::StringArray& fetchesInFlight()
    {
        static juce::StringArray a;
        return a;
    }
    std::map<const void*, juce::StringArray>& jobFiles()
    {
        static std::map<const void*, juce::StringArray> m;
        return m;
    }
    constexpr juce::int64 kClearWritingMs = 5000;   // CLEAR NOW: files modified this recently may still be written (other processes)

    juce::File envDir (const char* name)
    {
        const auto env = juce::SystemStats::getEnvironmentVariable (name, {}).trim();
        return env.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (env) : juce::File();
    }

    void logLine (const juce::String& s)
    {
        std::fprintf (stderr, "[SG] %s\n", s.toRawUTF8());
    }

    const char* folderEnvName (sg::storage::Folder f)  { return f == sg::storage::Folder::cache ? "SG_CACHE_DIR" : "SG_EXPORT_DIR"; }
    const char* folderKey (sg::storage::Folder f)      { return f == sg::storage::Folder::cache ? "cacheDir" : "exportDir"; }

   #if JUCE_WINDOWS
    // Same key and value names as installer/SampleGrabber.iss
    juce::String folderRegPath (sg::storage::Folder f)
    {
        return juce::String ("HKEY_CURRENT_USER\\Software\\Mythen Studios\\Sample Grabber\\")
               + (f == sg::storage::Folder::cache ? "CacheDir" : "ExportDir");
    }
   #endif

    // Missing or completely empty folder. Only such a folder (or one with our marker) may become a cache: a folder with
    // just "downloads/" in it can be anybody's.
    bool isEmptyOrMissing (const juce::File& dir)
    {
        return ! dir.exists() || (dir.isDirectory() && dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
    }

    // The only folders below the cache root that cleaning touches
    constexpr const char* cacheSubfolders[] = { "downloads", "imports", "staging", "tmp" };
}

namespace sg::storage
{
    juce::File appDataDir()
    {
        // Same folder as settings.xml and audio.xml
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SampleGrabber");
    }

    juce::File defaultFolder (Folder f)
    {
        if (f == Folder::exports)
            return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Sample Grabber");
       #if JUCE_WINDOWS
        // Large downloads must not roam: Local AppData (settings.xml/audio.xml stay in Roaming via appDataDir())
        return juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("SampleGrabber").getChildFile ("cache");
       #else
        return appDataDir().getChildFile ("cache");
       #endif
    }

    juce::String storedFolder (Folder f)
    {
       #if JUCE_WINDOWS
        return juce::WindowsRegistry::getValue (folderRegPath (f), {}).trim();
       #else
        return sg::settings::get (folderKey (f), {}).trim();
       #endif
    }

    bool setStoredFolder (Folder f, const juce::String& absPath)
    {
        const auto v = absPath.trim();
       #if JUCE_WINDOWS
        if (v.isEmpty())
            return ! juce::WindowsRegistry::valueExists (folderRegPath (f)) || juce::WindowsRegistry::deleteValue (folderRegPath (f));
        return juce::WindowsRegistry::setValue (folderRegPath (f), v);
       #else
        if (v.isEmpty())
            return sg::settings::remove (folderKey (f));
        return sg::settings::set (folderKey (f), v);
       #endif
    }

    bool folderFromEnv (Folder f)
    {
        return envDir (folderEnvName (f)) != juce::File();
    }

    juce::File resolveFolder (Folder f)
    {
        const auto env = envDir (folderEnvName (f));
        if (env != juce::File())
            return env;

        const auto stored = storedFolder (f);
        if (stored.isNotEmpty())
        {
            if (! juce::File::isAbsolutePath (stored))
            {
                logLine (juce::String ("folder setting ignored (not an absolute path): ") + folderKey (f) + "=" + stored);
                return defaultFolder (f);
            }
            juce::File dir (stored);
            // Same rules as CHANGE (the installer writes CacheDir unchecked): unsafe -> default, foreign content -> subfolder.
            // The default location stored explicitly stays the default (adopted with old content, see ensureCacheRoot).
            if (f == Folder::cache && dir == defaultFolder (f))
                return dir;
            if (f == Folder::cache)
            {
                juce::File use;
                const auto r = cacheFolderFor (dir, use);
                if (r.failed())
                {
                    logLine ("cache folder setting ignored (" + r.getErrorMessage().toLowerCase() + "): " + dir.getFullPathName()
                             + ", using the default");
                    return defaultFolder (f);
                }
                dir = use;
            }
            // A missing drive (USB disk unplugged) or a path that cannot be created: fall back to the default
            if (dir.isDirectory() || dir.createDirectory().wasOk())
                return dir;
            logLine (juce::String ("folder setting ignored (cannot create): ") + dir.getFullPathName() + ", using the default");
        }
        return defaultFolder (f);
    }

    juce::Result checkChosenFolder (Folder f, const juce::String& typed, juce::File& result)
    {
        const auto t = typed.trim().unquoted().trim();
        if (t.isEmpty())
            return juce::Result::fail ("ENTER A FOLDER PATH");
        if (! juce::File::isAbsolutePath (t))
            return juce::Result::fail ("NEEDS A FULL PATH");

        juce::File dir (t);
        if (dir.getParentDirectory() == dir || dir.getFullPathName().length() <= 3)
            return juce::Result::fail ("NOT A DRIVE OR FILESYSTEM ROOT");
        if (dir.existsAsFile())
            return juce::Result::fail ("THIS IS A FILE, NOT A FOLDER");

        if (f == Folder::cache)
        {
            juce::File use;
            const auto r = cacheFolderFor (dir, use);
            if (r.failed())
                return r;
            dir = use;
        }
        else
        {
            // Exports inside the cache would be deleted by CLEAR NOW / auto-clean
            const auto tools = ToolManager::toolsDir();
            for (const auto& c : { cacheRoot(), resolveFolder (Folder::cache) })
                if (dir == c || dir.isAChildOf (c))
                    return juce::Result::fail ("NOT INSIDE THE CACHE FOLDER");
            if (dir == tools || dir.isAChildOf (tools))
                return juce::Result::fail ("NOT INSIDE THE TOOLS FOLDER");
        }

        if (! dir.isDirectory() && dir.createDirectory().failed())
            return juce::Result::fail ("CANNOT CREATE THIS FOLDER");
        result = dir;
        return juce::Result::ok();
    }

    juce::Result cacheFolderFor (const juce::File& chosen, juce::File& result)
    {
        auto dir = chosen;
        if (dir.existsAsFile())
            return juce::Result::fail ("THIS IS A FILE, NOT A FOLDER");

        const auto tools = ToolManager::toolsDir();
        auto checkSafe = [&] (const juce::File& d)
        {
            juce::String why;
            if (! isSafeCacheRoot (d, why) && why != "no marker")
                return juce::Result::fail ("NOT ALLOWED AS CACHE: " + why.toUpperCase());
            // The tool installer wipes bin/.tmp, REMOVE TOOLS the bin folder: no cache inside it either
            if (d.isAChildOf (tools))
                return juce::Result::fail ("NOT ALLOWED AS CACHE: TOOLS FOLDER");
            return juce::Result::ok();
        };

        auto r = checkSafe (dir);
        if (r.failed())
            return r;
        // Foreign files: never mark that folder, use our own subfolder inside it
        if (! hasCacheMarker (dir) && ! isEmptyOrMissing (dir))
        {
            dir = dir.getChildFile ("Sample Grabber Cache");
            if (dir.existsAsFile())
                return juce::Result::fail ("THIS IS A FILE, NOT A FOLDER");
            r = checkSafe (dir);
            if (r.failed())
                return r;
            if (! hasCacheMarker (dir) && ! isEmptyOrMissing (dir))
                return juce::Result::fail ("SAMPLE GRABBER CACHE SUBFOLDER HAS FOREIGN FILES");
        }
        result = dir;
        return juce::Result::ok();
    }

    juce::File cacheRoot()
    {
        // Once per process: downloads, staging and the cleaner must agree on one folder. A new setting applies after
        // a restart, the old cache is not moved (CLEAR NOW before changing, or delete it by hand).
        // The marker is written right here, before staging/downloads can put anything into a new, empty folder
        // (afterwards it would no longer count as empty).
        static const juce::File root = []
        {
            auto r = resolveFolder (Folder::cache);
            const bool env = folderFromEnv (Folder::cache);
            const bool isDefault = ! env && r == defaultFolder (Folder::cache);
            if (! markCacheRoot (r, isDefault) && ! env && ! isDefault)
            {
                // Stored folder changed between the check and here, or not writable: never use it unmarked
                logLine ("cache: cannot use " + r.getFullPathName() + ", using the default");
                r = defaultFolder (Folder::cache);
                markCacheRoot (r, true);
            }
            return r;
        }();
        return root;
    }

    juce::File cacheSubdir (const juce::String& sub)
    {
        auto d = cacheRoot().getChildFile (sub);
        // SG_CACHE_DIR (tests) is used even without a marker (never cleaned then). Otherwise nothing is created in a
        // folder that could not be marked: the caller's write fails and reports it.
        if (ensureCacheRoot() || folderFromEnv (Folder::cache))
            d.createDirectory();
        else
            logLine ("cache: not creating " + d.getFullPathName() + " (cache folder has no " + cacheMarkerName + ")");
        return d;
    }

    bool hasCacheMarker (const juce::File& root)
    {
        return root.getChildFile (cacheMarkerName).existsAsFile();
    }

    bool markCacheRoot (const juce::File& root, bool allowAnyContent)
    {
        if (hasCacheMarker (root))
            return true;

        if (! allowAnyContent && ! isEmptyOrMissing (root))
        {
            logLine ("cache: " + root.getFullPathName() + " is not empty and has no " + cacheMarkerName + " - not marking it, it will never be cleaned");
            return false;
        }
        if (! root.createDirectory())
            return false;
        // Unsafe locations (home, Music, ...) never get a marker either
        juce::String why;
        if (! isSafeCacheRoot (root, why) && why != "no marker")
        {
            logLine ("cache: refusing to mark " + root.getFullPathName() + " (" + why + ")");
            return false;
        }
        return root.getChildFile (cacheMarkerName).replaceWithText ("Sample Grabber cache. Everything in this folder may be deleted by the app.\n");
    }

    bool ensureCacheRoot()
    {
        const auto root = cacheRoot();
        // Only the built-in default location may be adopted with other content (old installs)
        return markCacheRoot (root, ! folderFromEnv (Folder::cache) && root == defaultFolder (Folder::cache));
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
        else if (same (root, appDataDir()) || same (root, juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory))
                #if JUCE_WINDOWS
                 || same (root, juce::File::getSpecialLocation (juce::File::windowsLocalAppData))
                 || same (root, juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("SampleGrabber"))
                #endif
                 )
            whyNot = "user data folder";
        else if (same (root, exp) || exp.isAChildOf (root))
            whyNot = "export folder or its parent";
        else if (home.isAChildOf (root))
            whyNot = "parent of the home folder";
        else if (same (root, ToolManager::toolsDir()) || ToolManager::toolsDir().isAChildOf (root))
            whyNot = "tools folder or its parent";
        else if (! hasCacheMarker (root))
            whyNot = "no marker";
        else
            return true;
        return false;
    }

    juce::File exportRoot()
    {
        return resolveFolder (Folder::exports);
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

    juce::String sanitizeDisplayTitle (const juce::String& in)
    {
        juce::String out;
        out.preallocateBytes (in.getNumBytesAsUTF8());
        for (auto p = in.getCharPointer(); ! p.isEmpty(); ++p)
        {
            const juce::juce_wchar ch = *p;
            if (ch >= 32 && ch != 127)
                out << juce::String::charToString (ch);
        }
        return out.trim().substring (0, 300).trim();
    }

    bool isCacheSource (const juce::File& f)
    {
        const auto parent = f.getParentDirectory();
        for (auto sub : { "downloads", "imports" })
            if (parent == cacheRoot().getChildFile (sub) || parent == appDataDir().getChildFile (sub))
                return true;
        return false;
    }

    juce::String stripRangeTag (const juce::File& f, sg::range::Range* rangeOut)
    {
        auto n = f.getFileNameWithoutExtension();
        if (rangeOut != nullptr)
            *rangeOut = {};
        if (! isCacheSource (f))
            return n;

        const int us = n.lastIndexOfChar ('_');
        sg::range::Range r;
        if (us > 0 && sg::range::parseFileTag (n.substring (us + 1), r))
        {
            n = n.substring (0, us);
            if (rangeOut != nullptr)
                *rangeOut = r;
        }
        return n;
    }

    juce::String titleFromFile (const juce::File& f)
    {
        auto n = stripRangeTag (f);   // "<title>_<id>_r3720-3930" -> "<title>_<id>" (cache sources only)

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
        // By position, not by the last '_': YouTube IDs may contain '_' themselves ("fJrJ2_ZsydE")
        if (fromDownloads && hasVideoIdTail (n))
            n = n.substring (0, n.length() - 12);

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

    bool getKeepDownloads()
    {
        return sg::settings::get ("keepDownloads", "1") != "0";
    }

    void setKeepDownloads (bool keep)
    {
        sg::settings::set ("keepDownloads", keep ? "1" : "0");
    }

    bool deleteCacheSource (const juce::File& f)
    {
        juce::String why;
        const auto root = cacheRoot();
        const auto parent = f.getParentDirectory();
        const bool ours = parent == root.getChildFile ("downloads") || parent == root.getChildFile ("imports");
        if (! ours || ! f.existsAsFile() || f.isSymbolicLink() || ! isSafeCacheRoot (root, why))
        {
            logLine ("keep downloads off: not deleting " + f.getFullPathName() + (why.isNotEmpty() ? " (" + why + ")" : juce::String()));
            return false;
        }
        const bool ok = f.deleteFile();
        logLine (juce::String (ok ? "keep downloads off: deleted " : "keep downloads off: could not delete ") + f.getFullPathName());
        return ok;
    }

    bool isValidVideoId (const juce::String& id)
    {
        return id.length() >= 6 && id.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");
    }

    bool hasVideoIdTail (const juce::String& name)
    {
        const int n = name.length();
        return n > 12 && name[n - 12] == '_' && isValidVideoId (name.substring (n - 11));
    }

    void rememberVideoId (const juce::String& url, const juce::String& id)
    {
        if (url.isEmpty() || ! isValidVideoId (id))
            return;
        const std::lock_guard<std::mutex> g (idMutex);
        auto& v = urlIds();
        v.erase (std::remove_if (v.begin(), v.end(), [&] (const auto& e) { return e.first == url; }), v.end());
        v.emplace_back (url, id);
        if (v.size() > 64)
            v.erase (v.begin());
    }

    juce::String videoIdFor (const juce::String& url)
    {
        const std::lock_guard<std::mutex> g (idMutex);
        for (const auto& e : urlIds())
            if (e.first == url)
                return e.second;
        return {};
    }

    bool tryBeginFetch (const juce::String& id)
    {
        const std::lock_guard<std::mutex> g (idMutex);
        if (fetchesInFlight().contains (id))
            return false;
        fetchesInFlight().add (id);
        return true;
    }

    void endFetch (const juce::String& id)
    {
        const std::lock_guard<std::mutex> g (idMutex);
        fetchesInFlight().removeString (id);
    }

    void setJobFiles (const void* owner, const juce::StringArray& nameParts)
    {
        const std::lock_guard<std::mutex> g (idMutex);
        if (nameParts.isEmpty())
            jobFiles().erase (owner);
        else
            jobFiles()[owner] = nameParts;
    }

    bool isBeingWritten (const juce::File& f)
    {
        const auto n = f.getFileName();
        if (n.endsWith (".part") || n.contains (".part-Frag") || n.contains (".temp.") || n.endsWith (".ytdl") || n.endsWith (".tmp"))
            return true;
        const std::lock_guard<std::mutex> g (idMutex);
        for (const auto& id : fetchesInFlight())
            if (n.contains ("_" + id))
                return true;
        for (const auto& kv : jobFiles())
            for (const auto& part : kv.second)
                if (part.isNotEmpty() && n.contains (part))
                    return true;
        return false;
    }

    bool isProbeJson (const juce::File& f)
    {
        const auto n = f.getFileName();
        return f.getParentDirectory() == cacheRoot().getChildFile ("tmp") && n.startsWith ("probe-") && n.endsWith (".info.json");
    }

    void sweepProbeJsons (const juce::File& root, juce::int64 maxAgeMs)
    {
        const auto dir = root.getChildFile ("tmp");
        if (! dir.isDirectory())
            return;
        const auto now = juce::Time::currentTimeMillis();
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "probe-*.info.json", juce::File::FollowSymlinks::no))
            if (now - f.getLastModificationTime().toMilliseconds() > maxAgeMs && f.deleteFile())
                logLine ("cache: removed old probe info " + f.getFileName());
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
            sg::storage::sweepProbeJsons (root, 10 * 60 * 1000);
            wantAuto = true;
        }

        if (wantClear.exchange (false))
        {
            wantAuto = false;   // clearing covers it
            // Short enough for the overlay's note line (<= 40 characters): size dropped when a download runs
            const auto r = clean (root, sg::storage::AutoClean::off, true);
            const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
            note = "CLEARED " + juce::String (r.deleted) + (r.keptBusy > 0 ? juce::String() : dot + sg::storage::formatBytes (r.freed))
                   + (r.keptInUse > 0 ? dot + juce::String (r.keptInUse) + " IN USE" : juce::String())
                   + (r.keptBusy > 0 ? dot + juce::String (r.keptBusy) + " BEING WRITTEN" : juce::String());
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

    struct Entry { juce::File file; juce::int64 size, mtime; bool isProtected, isStaging, inUse; };
    std::vector<Entry> entries;
    juce::int64 total = 0;

    // Only regular files in our own subfolders, symlinks are not followed (never delete anything outside). Files
    // directly in the root (the marker, anything the user put there) are never touched.
    std::vector<juce::File> files;
    for (auto sub : cacheSubfolders)
    {
        const auto dir = root.getChildFile (sub);
        if (dir.isDirectory() && ! dir.isSymbolicLink())
            for (const auto& f : dir.findChildFiles (juce::File::findFiles, true, "*", juce::File::FollowSymlinks::no))
                files.push_back (f);
    }

    for (const auto& f : files)
    {
        if (! f.isAChildOf (root))
            continue;
        const auto mtime = f.getLastModificationTime().toMilliseconds();
        if (f.getFileName() == sg::storage::cacheMarkerName)
            continue;
        // Auto-clean: everything younger than 2 min. CLEAR NOW (user asked for it): only what is really being written.
        const bool young = sg::storage::isProbeJson (f) ? now - mtime < kProbeJsonKeepMs
                         : clearAll ? (now - mtime < kClearWritingMs || sg::storage::isBeingWritten (f))
                                    : now - mtime < kProtectYoungMs;
        const bool used = sg::storage::isInUse (f);
        entries.push_back ({ f, f.getSize(), mtime, young || used, f.isAChildOf (staging), used });
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
            {
                ++res.kept;
                ++(e.inUse ? res.keptInUse : res.keptBusy);
            }
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
                 + (res.kept > 0 ? ", " + juce::String (res.keptInUse) + " in use + " + juce::String (res.keptBusy)
                                       + (clearAll ? " being written" : " < 2 min old") + " kept" : juce::String()));
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
        { "downloads/new_1min.part",   300,   0.01 },  // being written (.part) -> never deleted
        { "downloads/fresh_10s.wav",     1,  10.0 / 3600.0 },   // 10 s old, not in use: auto keeps it, CLEAR NOW deletes it
        { "downloads/now_1s.wav",        1,   1.0 / 3600.0 },   // modified 1 s ago: may be written by another process
        { "downloads/T_jobid1.m4a",      1,  60.0 / 3600.0 },   // file of a running job (setJobFiles) -> kept
        { "imports/conv_2d.wav",       100,  48.0 },
        { "staging/inst-dead/x.wav",     1,  30.0 },   // staging leftover > 24 h
        { "staging/inst-live/y.wav",     1,   1.0 },
        { "user_note_10d.txt",           1, 240.0 },   // directly in the root, not ours -> never deleted (not even CLEAR NOW)
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
            {
                juce::FileOutputStream os (f);
                if (os.openedOk())
                {
                    os.setPosition ((juce::int64) fk.mb * MB - 1);   // sparse file, cheap
                    os.writeByte (0);
                }
            }
            // Only after closing: Windows stamps the write time when a written handle is closed
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
        expect ("downloads/fresh_10s.wav", true);   // auto-clean keeps the 2 min rule
        expect ("staging/inst-dead/x.wav", false);
        expect ("staging/inst-live/y.wav", true);
        expect ("user_note_10d.txt", true);
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
    static int jobOwner = 0;
    sg::storage::setJobFiles (&jobOwner, { "_jobid1." });
    std::fprintf (stderr, "[SG-CACHE] ---- CLEAR NOW ----\n");
    const auto cleared = clean (root, AC::off, true);
    listing ("CLEAR NOW");
    std::fprintf (stderr, "[SG-CACHE] CLEAR NOW: deleted %d, kept %d in use + %d being written\n", cleared.deleted, cleared.keptInUse, cleared.keptBusy);
    expect ("downloads/inuse_9d.wav", true);
    expect ("downloads/new_1min.part", true);
    expect ("downloads/now_1s.wav", true);
    expect ("downloads/T_jobid1.m4a", true);
    expect ("downloads/fresh_10s.wav", false);
    expect ("downloads/old_10d.wav", false);
    expect ("imports/conv_2d.wav", false);
    expect ("user_note_10d.txt", true);
    if (! sg::storage::hasCacheMarker (root))
    {
        std::fprintf (stderr, "[SG-CACHE] FAIL: CLEAR NOW removed the marker\n");
        ++failures;
    }
    if (cleared.keptInUse != 1 || cleared.keptBusy != 3)
    {
        std::fprintf (stderr, "[SG-CACHE] FAIL: kept counts %d/%d, expected 1 in use / 3 being written\n", cleared.keptInUse, cleared.keptBusy);
        ++failures;
    }
    sg::storage::setJobFiles (&jobOwner, {});

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

    // Foreign folders (outside SG_CACHE_DIR, in the temp folder): only an empty folder may be adopted
    {
        const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("sg-cache-foreign-" + juce::String (juce::Time::currentTimeMillis()));
        auto check = [&] (bool ok, const char* what)
        {
            std::fprintf (stderr, "[SG-CACHE] %s %s\n", ok ? "ok  " : "FAIL", what);
            if (! ok)
                ++failures;
        };
        const auto foreign = scratch.getChildFile ("only-downloads");
        foreign.getChildFile ("downloads").getChildFile ("myfile").create();
        check (! sg::storage::markCacheRoot (foreign, false) && ! sg::storage::hasCacheMarker (foreign),
               "folder with only downloads/myfile is not marked");
        juce::File use;
        check (sg::storage::cacheFolderFor (foreign, use).wasOk() && use == foreign.getChildFile ("Sample Grabber Cache"),
               "folder with only downloads/myfile -> own subfolder");
        check (foreign.getChildFile ("downloads").getChildFile ("myfile").existsAsFile(), "downloads/myfile untouched");

        const auto empty = scratch.getChildFile ("empty");
        empty.createDirectory();
        check (sg::storage::cacheFolderFor (empty, use).wasOk() && use == empty, "empty folder -> used as is");
        check (sg::storage::markCacheRoot (empty, false) && sg::storage::hasCacheMarker (empty), "empty folder is marked");
        check (sg::storage::cacheFolderFor (empty, use).wasOk() && use == empty, "marked folder -> used as is");

        const auto both = scratch.getChildFile ("both");
        both.getChildFile ("song.wav").create();
        both.getChildFile ("Sample Grabber Cache").getChildFile ("other.wav").create();
        check (sg::storage::cacheFolderFor (both, use).failed(), "foreign subfolder 'Sample Grabber Cache' refused");

        check (sg::storage::cacheFolderFor (juce::File::getSpecialLocation (juce::File::userHomeDirectory), use).failed(),
               "home refused");
        scratch.deleteRecursively();
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
