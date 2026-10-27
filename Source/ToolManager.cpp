#include "ToolManager.h"
#include <juce_cryptography/juce_cryptography.h>
#include "UserSettings.h"
#include <iostream>
#if ! JUCE_WINDOWS
 #include <unistd.h>   // link(): hard link as a backup when replacing
#endif

namespace
{
    // ------------------------------------------------------------- Platform
   #if JUCE_LINUX && JUCE_64BIT && JUCE_INTEL
    constexpr bool kSupported = true;
    const char* const kYtDlpAsset  = "yt-dlp_linux";
    const char* const kDenoTriple  = "x86_64-unknown-linux-gnu";
    const char* const kFfmpegPlat  = "linux64";
    const char* const kFfmpegExt   = ".tar.xz";
   #elif JUCE_LINUX && JUCE_64BIT && JUCE_ARM
    constexpr bool kSupported = true;
    const char* const kYtDlpAsset  = "yt-dlp_linux_aarch64";
    const char* const kDenoTriple  = "aarch64-unknown-linux-gnu";
    const char* const kFfmpegPlat  = "linuxarm64";
    const char* const kFfmpegExt   = ".tar.xz";
   #elif JUCE_WINDOWS && JUCE_64BIT && JUCE_INTEL
    constexpr bool kSupported = true;
    const char* const kYtDlpAsset  = "yt-dlp.exe";
    const char* const kDenoTriple  = "x86_64-pc-windows-msvc";
    const char* const kFfmpegPlat  = "win64";
    const char* const kFfmpegExt   = ".zip";
   #else
    // macOS etc.: not supported yet (stage 4), PATH fallback remains
    constexpr bool kSupported = false;
    const char* const kYtDlpAsset  = "";
    const char* const kDenoTriple  = "";
    const char* const kFfmpegPlat  = "";
    const char* const kFfmpegExt   = "";
   #endif

   #if JUCE_WINDOWS
    const char* const kExeSuffix = ".exe";
   #else
    const char* const kExeSuffix = "";
   #endif

    const char* const kUserAgent = "SampleGrabber/0.1 (tool installer)";
    constexpr juce::int64 kDayMs = 24 * 60 * 60 * 1000LL;

    juce::String exeName (const char* base)   { return juce::String (base) + kExeSuffix; }

    juce::String mainExe (ToolManager::Tool t)
    {
        switch (t)
        {
            case ToolManager::Tool::ytdlp:  return exeName ("yt-dlp");
            case ToolManager::Tool::deno:   return exeName ("deno");
            case ToolManager::Tool::ffmpeg: return exeName ("ffmpeg");
        }
        return {};
    }

    bool envSet (const char* name)
    {
        return juce::SystemStats::getEnvironmentVariable (name, {}).isNotEmpty();
    }

    // Own folder via SG_TOOLS_DIR -> no PATH fallback (tests should only see the test folder)
    bool pathFallbackAllowed()  { return ! envSet ("SG_TOOLS_DIR"); }

    juce::File findInPath (const juce::String& exe)
    {
       #if JUCE_WINDOWS
        const auto parts = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ";", "\"");
       #else
        const auto parts = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", {});
       #endif
        for (auto dir : parts)
        {
            dir = dir.trim().unquoted();
            if (! juce::File::isAbsolutePath (dir))
                continue;
            const auto f = juce::File (dir).getChildFile (exe);
            if (f.existsAsFile())
                return f;
        }
        return {};
    }

    bool isHex64 (const juce::String& s)
    {
        return s.length() == 64 && s.containsOnly ("0123456789abcdefABCDEF");
    }

    // Expected checksum for 'assetName' from a checksum file:
    //  - "hash  name" or "hash *name" (sha256sum, yt-dlp SHA2-256SUMS, BtbN checksums.sha256, deno Linux)
    //  - PowerShell format "Hash : HASH" + "Path : ...\name" (deno Windows): exactly one 64-hex word in the file
    juce::String findExpectedHash (const juce::String& text, const juce::String& assetName)
    {
        juce::StringArray all;
        for (const auto& rawLine : juce::StringArray::fromLines (text))
        {
            const auto tokens = juce::StringArray::fromTokens (rawLine.trim(), " \t", {});
            juce::String hash;
            bool nameMatch = false;
            for (auto tok : tokens)
            {
                if (isHex64 (tok))
                {
                    hash = tok;
                    all.addIfNotAlreadyThere (tok.toLowerCase());
                }
                tok = tok.trimCharactersAtStart ("*");
                if (tok == assetName || tok.endsWith ("/" + assetName) || tok.endsWith ("\\" + assetName))
                    nameMatch = true;
            }
            if (nameMatch && hash.isNotEmpty())
                return hash.toLowerCase();
        }
        return all.size() == 1 ? all[0] : juce::String();
    }

    // Messages for the status line: short, uppercase, ASCII, say what to do
    const char* const kErrNoNet  = "NO INTERNET CONNECTION";
    const char* const kErrBroken = "DOWNLOAD BROKEN";                      // -> re-plan once and retry
    const char* const kErrCheck  = "FILE CHECK FAILED - NOT INSTALLED";    // SHA-256 does not match / missing
    const char* const kErrBusy   = "GITHUB BUSY - TRY AGAIN IN A FEW MINUTES";
    const char* const kErrWrite  = "CANNOT WRITE TOOLS FOLDER";
    const char* const kErrCurl   = "CURL MISSING - INSTALL CURL";
    const char* const kCancelled = "CANCELLED";

    const juce::String kSep = juce::String::fromUTF8 (" \xc2\xb7 ");      // middle dot as in the editor (kDot)

    // Only characters allowed in tags/file names (no path injection via Location)
    bool isSafeName (const juce::String& s)
    {
        return s.isNotEmpty() && s.length() < 120
            && s.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-");
    }

    juce::String curlError (int rc)
    {
        if (rc == 255 || rc < 0)          return kErrCurl;
        if (rc == 6 || rc == 7 || rc == 28) return kErrNoNet;   // DNS, Verbindung, Timeout
        return kErrBroken;
    }

    // Version parts: "2026.10.26" (yt-dlp), "v2.9.8"/"2.9.8" (deno), "n9.0" (ffmpeg). Empty = unreadable.
    juce::Array<int> versionParts (juce::String v)
    {
        v = v.trim().trimCharactersAtStart ("nNvV");
        juce::Array<int> parts;
        for (auto& tok : juce::StringArray::fromTokens (v, ".", {}))
        {
            if (tok.isEmpty() || ! tok.containsOnly ("0123456789"))
                return {};
            parts.add (tok.getIntValue());
        }
        return parts;
    }

    // > 0: a is newer than b. An unreadable version counts as the oldest.
    int compareVersions (const juce::String& a, const juce::String& b)
    {
        const auto pa = versionParts (a), pb = versionParts (b);
        if (pa.isEmpty() || pb.isEmpty())
            return pa.isEmpty() ? (pb.isEmpty() ? 0 : -1) : 1;
        for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
        {
            const int x = pa[i], y = pb[i];   // missing part = 0
            if (x != y)
                return x > y ? 1 : -1;
        }
        return 0;
    }

    juce::String httpError (int code)
    {
        if (code == 429 || code == 403 || code >= 500)
            return kErrBusy;
        if (code == 404)
            return "NOT FOUND ON GITHUB";
        if (code == 0)
            return kErrNoNet;
        return "GITHUB ERROR " + juce::String (code);
    }

    juce::String lockName()
    {
        return "SampleGrabberTools_" + juce::File::createLegalFileName (juce::SystemStats::getLogonName());
    }

}

//==============================================================================
ToolManager::ToolManager() : juce::Thread ("tool-manager")
{
    refresh();
}

ToolManager::~ToolManager()
{
    signalThreadShouldExit();
    notify();
    {
        const juce::ScopedLock sl (procLock);
        if (proc != nullptr)
            proc->kill();
    }
    stopThread (15000);
}

juce::File ToolManager::toolsDir()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("SG_TOOLS_DIR", {}).trim();
    if (env.isNotEmpty())
        return juce::File::getCurrentWorkingDirectory().getChildFile (env);

   #if JUCE_WINDOWS
    const auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
   #else
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);   // like settings.xml
   #endif
    return base.getChildFile ("SampleGrabber").getChildFile ("bin");
}

juce::String ToolManager::toolName (Tool t)
{
    switch (t)
    {
        case Tool::ytdlp:  return "yt-dlp";
        case Tool::deno:   return "deno";
        case Tool::ffmpeg: return "ffmpeg";
    }
    return {};
}

bool ToolManager::isPlatformSupported()  { return kSupported; }

// Linux: JUCE is built with JUCE_USE_CURL=0 -> juce::URL cannot do TLS there, hence the system curl.
// SG_TOOLS_HTTP=juce forces juce::URL (testing only).
bool ToolManager::useCurl()
{
   #if JUCE_LINUX
    return juce::SystemStats::getEnvironmentVariable ("SG_TOOLS_HTTP", {}) != "juce";
   #else
    return false;
   #endif
}

std::unique_ptr<juce::XmlElement> ToolManager::loadXml() const
{
    const auto f = toolsDir().getChildFile ("tools.xml");
    if (f.existsAsFile())
    {
        if (auto xml = juce::parseXMLIfTagMatches (f, "TOOLS"))
            return xml;

        // Unreadable: set aside (keep only one .bad) and start over; versions are then treated as unknown
        const auto bad = f.getSiblingFile ("tools.xml.bad");
        bad.deleteFile();
        const bool moved = f.moveFileTo (bad);
        std::cerr << "[SG-TOOLS] tools.xml unreadable -> " << (moved ? bad.getFullPathName() : juce::String ("(rename failed)")).toStdString()
                  << ", starting fresh" << std::endl;
    }
    return std::make_unique<juce::XmlElement> ("TOOLS");
}

bool ToolManager::toolSelected (Tool t) const
{
    const auto only = juce::SystemStats::getEnvironmentVariable ("SG_TOOLS_ONLY", {});
    return only.isEmpty() || juce::StringArray::fromTokens (only, ",", {}).contains (toolName (t));
}

//==============================================================================
void ToolManager::refresh()
{
    const auto dir = toolsDir();
    const auto xml = loadXml();

    Info fresh[numTools];
    for (int i = 0; i < numTools; ++i)
    {
        const auto t = (Tool) i;
        auto& in = fresh[i];
        const auto own = dir.getChildFile (mainExe (t));
        bool ownOk = own.existsAsFile();
        if (t == Tool::ffmpeg)
            ownOk = ownOk && dir.getChildFile (exeName ("ffprobe")).existsAsFile();

        if (auto* e = xml->getChildByAttribute ("id", toolName (t)))
        {
            in.latest = e->getStringAttribute ("latest");
            if (ownOk)
            {
                in.version = e->getStringAttribute ("version");
                in.build = e->getStringAttribute ("build");
                in.sha256 = e->getStringAttribute ("sha256");
                in.installedAt = e->getStringAttribute ("installed").getLargeIntValue();
            }
        }

        if (ownOk)
        {
            in.path = own;
            in.own = true;
            if (in.version.isEmpty())
                in.version = "UNKNOWN";
        }
        else if (pathFallbackAllowed())
        {
            in.path = findInPath (mainExe (t));
            if (in.path != juce::File())
                in.version = "PATH";
        }

        // ffmpeg: version = release branch (n9.0) -> update only on a new branch, not on every daily build
        if (in.own && in.latest.isNotEmpty())
            in.updateAvailable = compareVersions (in.latest, in.version) > 0;   // never downgrade
        in.state = in.path != juce::File() ? State::ok : State::missing;
    }

    const juce::ScopedLock sl (lock);
    lastCheck = xml->getStringAttribute ("lastCheck").getLargeIntValue();
    for (int i = 0; i < numTools; ++i)
    {
        fresh[i].message = jobMessage[i];
        fresh[i].failed = jobState[i] == State::error;
        if (jobState[i] == State::working)
            fresh[i].state = State::working;
        else if (jobState[i] == State::error && fresh[i].state == State::missing)
            fresh[i].state = State::error;
        infos[i] = fresh[i];
    }
}

ToolManager::Info ToolManager::getInfo (Tool t) const
{
    const juce::ScopedLock sl (lock);
    return infos[(int) t];
}

juce::File ToolManager::getExecutable (Tool t) const
{
    const juce::ScopedLock sl (lock);
    return infos[(int) t].path;
}

bool ToolManager::needsInstall() const
{
    const juce::ScopedLock sl (lock);
    return infos[(int) Tool::ytdlp].path == juce::File() || infos[(int) Tool::ffmpeg].path == juce::File();
}

bool ToolManager::anyMissing() const
{
    const juce::ScopedLock sl (lock);
    for (auto& in : infos)
        if (in.path == juce::File())
            return true;
    return false;
}

bool ToolManager::needsAttention() const
{
    const juce::ScopedLock sl (lock);
    for (auto& in : infos)
        if (in.updateAvailable || in.failed)
            return true;
    return false;
}

juce::int64 ToolManager::getLastCheck() const
{
    const juce::ScopedLock sl (lock);
    return lastCheck;
}

// settings.xml via the shared helper (UserSettings.h). Off only for "0"/"false"/"off", otherwise on.
bool ToolManager::isAutoUpdateYtDlp()
{
    const auto v = sg::settings::get ("autoUpdateYtDlp", "1").trim().toLowerCase();
    return ! (v == "0" || v == "false" || v == "off");
}

void ToolManager::setAutoUpdateYtDlp (bool on)
{
    sg::settings::set ("autoUpdateYtDlp", on ? "1" : "0");
}

juce::String ToolManager::getStatusText() const
{
    const juce::ScopedLock sl (lock);
    return statusText;
}

juce::String ToolManager::getLastNote() const
{
    const juce::ScopedLock sl (lock);
    return lastNote;
}

bool ToolManager::lastNoteIsError() const
{
    const juce::ScopedLock sl (lock);
    return lastError;
}

//==============================================================================
bool ToolManager::tryBeginExternalJob()
{
    const juce::ScopedLock sl (gateLock);
    if (replacing)
        return false;
    ++externalJobs;
    return true;
}

void ToolManager::endExternalJob()
{
    const juce::ScopedLock sl (gateLock);
    externalJobs = juce::jmax (0, externalJobs - 1);
}

// Before replacing: wait for running grabs (maxWaitMs < 0 = unlimited); afterwards no grab starts until releaseExternalGate()
bool ToolManager::waitForExternalJobs (int maxWaitMs)
{
    const auto t0 = juce::Time::getMillisecondCounter();
    bool told = false;
    while (! threadShouldExit())
    {
        {
            const juce::ScopedLock sl (gateLock);
            if (externalJobs == 0)
            {
                replacing = true;
                return true;
            }
        }
        if (maxWaitMs >= 0 && juce::Time::getMillisecondCounter() - t0 > (juce::uint32) maxWaitMs)
            return false;
        if (! told)
        {
            setStatus ("WAITING FOR THE CURRENT DOWNLOAD", -1.0f);
            told = true;
        }
        wait (250);
    }
    return false;
}

void ToolManager::releaseExternalGate()
{
    const juce::ScopedLock sl (gateLock);
    replacing = false;
}

//==============================================================================
bool ToolManager::installMissing()                 { return startJob (Job::installMissing, true); }
bool ToolManager::updateAll (bool force)           { return startJob (force ? Job::updateForce : Job::update, true); }
bool ToolManager::checkForUpdates()                { return startJob (Job::check, true); }

void ToolManager::requestAutoCheck()
{
    // Developer modes and tests: no background network
    if (envSet ("SG_SNAPSHOT") || envSet ("SG_TOOLS_SELFTEST") || ! kSupported)
        return;
    if (autoCheckRequested.exchange (true))
        return;   // once per process

    refresh();
    bool anyOwn = false;
    for (int i = 0; i < numTools; ++i)
        anyOwn = anyOwn || getInfo ((Tool) i).own;
    if (! anyOwn)
        return;   // nothing installed by us -> nothing to update, no network access

    const auto xml = loadXml();
    const auto last = xml->getStringAttribute ("lastCheck").getLargeIntValue();
    if (juce::Time::currentTimeMillis() - last < kDayMs && ! xml->getBoolAttribute ("retryAuto"))
        return;   // retryAuto: last automatic yt-dlp update was skipped -> retry despite 24 h

    startJob (Job::autoCheck, false);
}

bool ToolManager::startJob (Job j, bool showProgress)
{
    bool expected = false;
    if (! working.compare_exchange_strong (expected, true))
        return false;

    if (! kSupported)
    {
        working = false;
        {
            const juce::ScopedLock sl (lock);
            lastNote = "AUTO INSTALL NOT SUPPORTED ON THIS SYSTEM YET - PUT YT-DLP + FFMPEG IN PATH";
            lastError = true;
        }
        ++runCounter;
        sendChangeMessage();
        return false;
    }

    stopThread (2000);   // previous run is finished (working was false), release the thread handle
    pendingJob = j;
    visible = showProgress;
    progress = -1.0f;
    {
        const juce::ScopedLock sl (lock);
        statusText = j == Job::check ? "CHECKING FOR UPDATES" : "STARTING";
    }
    sendChangeMessage();
    startThread();
    return true;
}

void ToolManager::setStatus (const juce::String& text, float p)
{
    progress = p;
    bool changed = false;
    {
        const juce::ScopedLock sl (lock);
        changed = statusText != text;
        statusText = text;
    }
    // Report progress at most ~10x per second, text changes immediately
    const auto now = juce::Time::getMillisecondCounter();
    if (changed || now - lastNotify > 100)
    {
        lastNotify = now;
        sendChangeMessage();
    }
}

void ToolManager::setToolState (Tool t, State s, const juce::String& message)
{
    {
        const juce::ScopedLock sl (lock);
        jobState[(int) t] = s;
        jobMessage[(int) t] = message;
    }
    refresh();
    sendChangeMessage();
}

//==============================================================================
void ToolManager::run()
{
    const auto job = pendingJob.load();
    juce::String note;
    bool isError = false;
    for (auto& c : planCached)
        c = false;   // versions/plans are valid for this run only

    if (job == Job::autoCheck)
        wait (3000);   // do not compete with editor loading right at startup

    toolsDir().createDirectory();

    juce::InterProcessLock ipl (lockName());
    bool locked = false;
    while (! threadShouldExit())
    {
        if (ipl.enter (250))
        {
            locked = true;
            break;
        }
        if (job == Job::autoCheck)
            break;   // another process is working -> skip the automatic run
        setStatus ("WAITING FOR ANOTHER SAMPLE GRABBER", -1.0f);
    }

    if (locked && ! threadShouldExit())
    {
        refresh();   // another process may have installed already

        if (job == Job::installMissing)
        {
            juce::Array<Tool> targets;
            for (int i = 0; i < numTools; ++i)
                if (toolSelected ((Tool) i) && getExecutable ((Tool) i) == juce::File())
                    targets.add ((Tool) i);

            if (targets.isEmpty())
                note = "TOOLS READY";   // nothing missing (without a version query, no "up to date" claim)
            else
                doInstall (targets, note, isError, -1);
        }
        else if (job == Job::update || job == Job::updateForce)
        {
            juce::Array<Tool> targets;
            if (job == Job::update)
            {
                doCheck (note, isError);
                if (! isError)
                    note.clear();
            }
            for (int i = 0; i < numTools; ++i)
            {
                const auto in = getInfo ((Tool) i);
                if (toolSelected ((Tool) i) && (job == Job::updateForce || (in.own && in.updateAvailable)))
                    targets.add ((Tool) i);
            }
            if (! isError)
            {
                if (targets.isEmpty())
                    note = stateSummary();
                else
                    doInstall (targets, note, isError, -1);
            }
        }
        else if (job == Job::check)
        {
            doCheck (note, isError);
        }
        else if (job == Job::autoCheck)
        {
            doCheck (note, isError);
            note.clear();   // silent: no errors/notes in the status line
            isError = false;

            const auto yt = getInfo (Tool::ytdlp);
            if (yt.own && yt.updateAvailable && isAutoUpdateYtDlp())   // off: only mark (gear dot)
            {
                juce::String n;
                bool e = false;
                doInstall ({ Tool::ytdlp }, n, e, 120000);   // wait up to 2 min for a running grab, otherwise skip
                if (! e)
                {
                    note = "YT-DLP UPDATED TO " + getInfo (Tool::ytdlp).version;
                    auto xml = loadXml();
                    xml->removeAttribute ("retryAuto");
                    xml->writeTo (toolsDir().getChildFile ("tools.xml"));
                }
                else
                {
                    // Failed/skipped: next start tries again (lastCheck stays for the display)
                    auto xml = loadXml();
                    xml->setAttribute ("retryAuto", "1");
                    xml->writeTo (toolsDir().getChildFile ("tools.xml"));
                }
            }
        }

        // Clean up leftovers (aborted downloads) - only with the lock, otherwise it could be another process's folder
        toolsDir().getChildFile (".tmp").deleteRecursively();
        // Old programs from the Windows replace (were still running) - errors do not matter, next run tries again
        for (auto& f : toolsDir().findChildFiles (juce::File::findFiles, false, "*.old-*"))
            f.deleteFile();
        ipl.exit();
    }

    refresh();

    {
        const juce::ScopedLock sl (lock);
        lastNote = threadShouldExit() ? juce::String() : note;
        lastError = isError;
        statusText.clear();
    }
    progress = -1.0f;
    visible = false;
    working = false;
    ++runCounter;
    sendChangeMessage();
}

//==============================================================================
void ToolManager::doCheck (juce::String& note, bool& isError)
{
    setStatus ("LOOKING FOR UPDATES", -1.0f);

    auto xml = loadXml();
    bool anyFetched = false;

    for (int i = 0; i < numTools && ! threadShouldExit(); ++i)
    {
        const auto t = (Tool) i;
        if (! toolSelected (t))
            continue;

        Plan plan;
        auto r = getPlan (t, plan, false);
        if (r.failed())
        {
            note = "UPDATE CHECK FAILED: " + r.getErrorMessage();
            isError = true;
            continue;
        }

        anyFetched = true;
        auto* e = xml->getChildByAttribute ("id", toolName (t));
        if (e == nullptr)
        {
            e = xml->createNewChildElement ("TOOL");
            e->setAttribute ("id", toolName (t));
        }
        e->setAttribute ("latest", plan.version);

    }

    if (anyFetched)
    {
        xml->setAttribute ("lastCheck", juce::String (juce::Time::currentTimeMillis()));
        xml->writeTo (toolsDir().getChildFile ("tools.xml"));
    }
    refresh();

    if (! isError)
        note = stateSummary();
}

// State of all (selected) tools in one line. "UP TO DATE" only for installed tools:
//  all own up to date -> "ALL TOOLS UP TO DATE"; otherwise groups "... UP TO DATE", "X UPDATE AVAILABLE v",
//  "... FROM PATH", "... NOT INSTALLED" and a hint what to do.
juce::String ToolManager::stateSummary() const
{
    juce::StringArray current, fromPath, missing, updates;
    for (int i = 0; i < numTools; ++i)
    {
        const auto t = (Tool) i;
        if (! toolSelected (t))
            continue;
        const auto in = getInfo (t);
        const auto name = toolName (t).toUpperCase();
        if (in.path == juce::File())   missing.add (name);
        else if (! in.own)             fromPath.add (name);
        else if (in.updateAvailable)   updates.add (name + " UPDATE AVAILABLE " + in.latest);
        else                           current.add (name);
    }

    if (fromPath.isEmpty() && missing.isEmpty() && updates.isEmpty())
        return "ALL TOOLS UP TO DATE";

    juce::StringArray parts;
    if (current.size() > 0)  parts.add (current.joinIntoString (" + ") + " UP TO DATE");
    parts.addArray (updates);
    if (fromPath.size() > 0) parts.add (fromPath.joinIntoString (" + ") + " FROM PATH");
    if (missing.size() > 0)  parts.add (missing.joinIntoString (" + ") + " NOT INSTALLED");

    juce::String hint;
    if (missing.size() > 0)        hint = " - CLICK INSTALL";
    else if (updates.size() > 0)   hint = " - CLICK UPDATE";
    return parts.joinIntoString (kSep) + hint;
}

void ToolManager::doInstall (const juce::Array<Tool>& targets, juce::String& note, bool& isError, int maxWaitMs)
{
    // Collect the result per tool -> one line: "INSTALLED YT-DLP + DENO . FFMPEG FAILED: ... - CLICK TO RETRY"
    juce::StringArray okNames, reasons;
    juce::Array<juce::StringArray> failedPerReason;
    auto fail = [&] (Tool t, const juce::String& why)
    {
        setToolState (t, State::error, why);
        int idx = reasons.indexOf (why);
        if (idx < 0)
        {
            reasons.add (why);
            failedPerReason.add ({});
            idx = reasons.size() - 1;
        }
        failedPerReason.getReference (idx).add (toolName (t).toUpperCase());
    };

    // 1) Versions and download URLs (github.com, not the REST API)
    setStatus ("LOOKING UP VERSIONS", -1.0f);
    juce::Array<Plan> plans;
    for (auto t : targets)
    {
        if (threadShouldExit())
            return;
        Plan plan;
        const auto r = getPlan (t, plan, false);
        if (r.failed())
            fail (t, r.getErrorMessage());
        else
            plans.add (plan);
    }

    visible = true;   // visible; if a grab is running in the editor, it keeps the status line (syncTools)

    for (int n = 0; n < plans.size() && ! threadShouldExit(); ++n)
    {
        auto plan = plans[n];
        setToolState (plan.tool, State::working, {});
        const auto step = juce::String (n + 1) + "/" + juce::String (plans.size());

        // 2) Download, verify, unpack, test run - outside the grab lock
        Staged st;
        auto r = prepareOne (plan, step, st);

        // Aborted download or BtbN just rebuilt "latest" (checksum no longer matches): once more
        if (r.failed() && ! threadShouldExit()
            && (r.getErrorMessage() == kErrBroken || (plan.tool == Tool::ffmpeg && r.getErrorMessage() == kErrCheck)))
        {
            st.tmpDir.deleteRecursively();
            st = {};
            Plan fresh;
            if (getPlan (plan.tool, fresh, true).wasOk())
            {
                plan = fresh;
                r = prepareOne (plan, step, st);
            }
        }

        // 3) Only the replacing blocks grabs
        if (r.wasOk())
        {
            if (waitForExternalJobs (maxWaitMs))
            {
                setStatus ("INSTALLING " + toolName (plan.tool).toUpperCase() + " " + step, -1.0f);
                r = commitOne (plan, st);
                releaseExternalGate();
            }
            else if (! threadShouldExit())
            {
                r = juce::Result::fail ("SKIPPED - A DOWNLOAD WAS RUNNING");
            }
        }
        st.tmpDir.deleteRecursively();

        if (r.wasOk())
            okNames.add (toolName (plan.tool).toUpperCase());
        else if (! threadShouldExit())
            fail (plan.tool, r.getErrorMessage());
    }

    for (int i = 0; i < numTools; ++i)
    {
        bool stillWorking = false;
        {
            const juce::ScopedLock sl (lock);
            stillWorking = jobState[i] == State::working;
        }
        if (stillWorking)
            setToolState ((Tool) i, State::missing, {});   // aborted
    }

    // Summary in one line; if it does not fit (about 100 characters at 1x, Classic), use short reasons
    auto summary = [&] (bool shortForm)
    {
        juce::StringArray parts;
        if (okNames.size() > 0)
            parts.add ("INSTALLED " + okNames.joinIntoString (" + "));
        bool retryHint = false;
        for (int i = 0; i < reasons.size(); ++i)
        {
            auto why = reasons[i];
            if (shortForm)
                why = why.upToFirstOccurrenceOf (" - ", false, false)
                         .replace ("NO INTERNET CONNECTION", "NO INTERNET");
            // "FFMPEG FAILED: NO INTERNET CONNECTION", aber "YT-DLP: FILE CHECK FAILED - NOT INSTALLED" (no double FAILED)
            parts.add (failedPerReason[i].joinIntoString (" + ") + (why.contains ("FAILED") ? ": " : " FAILED: ") + why);
            retryHint = retryHint || ! reasons[i].contains ("TRY AGAIN");
        }
        return parts.joinIntoString (kSep) + (retryHint ? " - CLICK TO RETRY" : "");
    };
    isError = reasons.size() > 0;
    note = summary (false);
    if (note.length() > 100)
        note = summary (true);
}

juce::Result ToolManager::getPlan (Tool t, Plan& plan, bool refetch)
{
    const int i = (int) t;
    if (! refetch && planCached[i])
    {
        plan = planCache[i];
        return juce::Result::ok();
    }
    planCached[i] = false;
    auto r = makePlan (t, plan);
    if (r.wasOk())
    {
        planCache[i] = plan;
        planCached[i] = true;
    }
    return r;
}

//==============================================================================
// Latest version without the REST API (it only allows 60 requests/h per IP): github.com/<repo>/releases/latest
// answers with 302 to .../releases/tag/<tag>; the redirect is NOT followed, only Location is read.
juce::Result ToolManager::latestTag (const juce::String& repo, juce::String& tag)
{
    const auto url = "https://github.com/" + repo + "/releases/latest";
    juce::String location;
    int code = 0;

    if (useCurl())
    {
        juce::String out;
        const int rc = runTool ({ "curl", "-sS", "--proto", "=https", "--connect-timeout", "20", "--max-time", "40",
                                  "-A", kUserAgent, "-o", "/dev/null", "-w", "%{http_code} %{redirect_url}", url }, out, 60000);
        if (threadShouldExit())
            return juce::Result::fail (kCancelled);
        if (rc != 0)
            return juce::Result::fail (curlError (rc));
        out = out.trim();
        code = out.upToFirstOccurrenceOf (" ", false, false).getIntValue();
        location = out.fromFirstOccurrenceOf (" ", false, false).trim();
    }
    else
    {
        juce::StringPairArray headers;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withExtraHeaders ("User-Agent: " + juce::String (kUserAgent))
                        .withHttpRequestCmd ("HEAD")
                        .withConnectionTimeoutMs (20000)
                        .withNumRedirectsToFollow (0)
                        .withResponseHeaders (&headers)
                        .withStatusCode (&code);
        auto in = juce::URL (url).createInputStream (opts);
        if (in == nullptr && code == 0)
            return juce::Result::fail (kErrNoNet);
        location = headers["Location"].trim();
    }

    if (code < 300 || code >= 400 || ! location.contains ("/releases/tag/"))
        return juce::Result::fail (httpError (code < 300 ? 0 : code));

    tag = juce::URL::removeEscapeChars (location.fromLastOccurrenceOf ("/releases/tag/", false, false)).trim();
    if (! isSafeName (tag))
        return juce::Result::fail ("UNEXPECTED ANSWER FROM GITHUB");
    return juce::Result::ok();
}

// Size via HEAD (progress bar), -1 = unknown -> indeterminate bar
juce::int64 ToolManager::headSize (const juce::String& url)
{
    if (useCurl())
    {
        juce::String out;
        const int rc = runTool ({ "curl", "-sSIL", "--proto", "=https", "--proto-redir", "=https",
                                  "--connect-timeout", "20", "--max-time", "40", "-A", kUserAgent, url }, out, 60000);
        if (rc != 0)
            return -1;
        juce::int64 size = -1;
        for (auto& line : juce::StringArray::fromLines (out))   // the last response of the redirect chain counts
            if (line.startsWithIgnoreCase ("content-length:"))
                size = line.fromFirstOccurrenceOf (":", false, false).trim().getLargeIntValue();
        return size > 0 ? size : -1;
    }

    int code = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withExtraHeaders ("User-Agent: " + juce::String (kUserAgent))
                    .withHttpRequestCmd ("HEAD")
                    .withConnectionTimeoutMs (20000)
                    .withNumRedirectsToFollow (5)
                    .withStatusCode (&code);
    auto in = juce::URL (url).createInputStream (opts);
    if (in == nullptr || code != 200)
        return -1;
    const auto n = in->getTotalLength();
    return n > 0 ? n : -1;
}

juce::Result ToolManager::makePlan (Tool t, Plan& plan)
{
    plan = {};
    plan.tool = t;

    if (t == Tool::ytdlp || t == Tool::deno)
    {
        const juce::String repo = t == Tool::ytdlp ? "yt-dlp/yt-dlp" : "denoland/deno";
        juce::String tag;
        auto r = latestTag (repo, tag);
        if (r.failed())
            return r;

        // Fixed tag URL (not latest/download): binary and checksum safely from the same release
        const auto base = "https://github.com/" + repo + "/releases/download/" + tag + "/";
        if (t == Tool::ytdlp)
        {
            plan.assetName = kYtDlpAsset;
            plan.sumsUrl = base + "SHA2-256SUMS";
            plan.version = tag;
        }
        else
        {
            plan.assetName = "deno-" + juce::String (kDenoTriple) + ".zip";
            plan.sumsUrl = base + plan.assetName + ".sha256sum";
            plan.version = tag.trimCharactersAtStart ("v");
        }
        plan.assetUrl = base + plan.assetName;
        return juce::Result::ok();
    }

    // ffmpeg (BtbN, rolling tag "latest"): checksums.sha256 lists all files -> highest release branch,
    // lgpl, static: ffmpeg-n<MAJ>.<MIN>-latest-<plat>-lgpl-<MAJ>.<MIN><ext>
    const juce::String base ("https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/");
    const auto tmp = toolsDir().getChildFile (".tmp");
    tmp.createDirectory();
    const auto sumsFile = tmp.getChildFile ("ffmpeg-checksums.sha256");
    auto r = download (base + "checksums.sha256", sumsFile, -1, nullptr);
    const auto text = sumsFile.loadFileAsString();
    sumsFile.deleteFile();
    if (r.failed())
        return r;

    int bestMaj = -1, bestMin = -1;
    for (auto& line : juce::StringArray::fromLines (text))
    {
        auto tokens = juce::StringArray::fromTokens (line.trim(), " \t", {});
        tokens.removeEmptyStrings();   // "hash  name": two spaces would otherwise yield an empty word
        if (tokens.size() != 2 || ! isHex64 (tokens[0]))
            continue;
        const auto name = tokens[1].trimCharactersAtStart ("*");
        if (! name.startsWith ("ffmpeg-n") || ! name.endsWith (kFfmpegExt) || name.contains ("shared") || ! isSafeName (name))
            continue;
        const auto ver = name.fromFirstOccurrenceOf ("ffmpeg-n", false, false).upToFirstOccurrenceOf ("-latest-", false, false);
        if (! ver.containsOnly ("0123456789.") || ! ver.containsChar ('.'))
            continue;
        if (name != "ffmpeg-n" + ver + "-latest-" + kFfmpegPlat + "-lgpl-" + ver + kFfmpegExt)
            continue;
        const int maj = ver.upToFirstOccurrenceOf (".", false, false).getIntValue();
        const int min = ver.fromFirstOccurrenceOf (".", false, false).getIntValue();
        if (maj > bestMaj || (maj == bestMaj && min > bestMin))
        {
            bestMaj = maj;
            bestMin = min;
            plan.assetName = name;
        }
    }

    if (plan.assetName.isEmpty())
        return juce::Result::fail ("NO FFMPEG BUILD FOR THIS SYSTEM");

    plan.assetUrl = base + plan.assetName;
    plan.sumsUrl = base + "checksums.sha256";
    plan.sumsText = text;   // the same list for verification (smaller window for a rebuild in between)
    plan.version = "n" + juce::String (bestMaj) + "." + juce::String (bestMin);
    return juce::Result::ok();
}

//==============================================================================
// Download, SHA-256, unpack, test run in bin/.tmp/<tool>-<ms>/. Replaces nothing. Caller deletes st.tmpDir.
juce::Result ToolManager::prepareOne (const Plan& plan, const juce::String& step, Staged& st)
{
    const auto t = plan.tool;
    const auto upper = toolName (t).toUpperCase() + " " + step;
    const auto dir = toolsDir();
    const auto tmp = dir.getChildFile (".tmp").getChildFile (toolName (t) + "-" + juce::String (juce::Time::currentTimeMillis()));
    tmp.deleteRecursively();
    st.tmpDir = tmp;
    if (! tmp.createDirectory())
        return juce::Result::fail (kErrWrite);

    // --- Download (size via HEAD for the bar; unknown -> moving stripe) ---
    const auto archive = tmp.getChildFile (plan.assetName);
    const auto sumsFile = tmp.getChildFile ("sums.txt");
    setStatus ("DOWNLOADING " + upper, -1.0f);

    juce::Result r = juce::Result::ok();
    if (plan.sumsText.isNotEmpty())
        sumsFile.replaceWithText (plan.sumsText);
    else
        r = download (plan.sumsUrl, sumsFile, -1, nullptr);
    if (r.failed())
        return r;

    const auto size = headSize (plan.assetUrl);
    r = download (plan.assetUrl, archive, size, [this, upper, size] (juce::int64 n)
    {
        setStatus ("DOWNLOADING " + upper, size > 0 ? (float) juce::jlimit (0.0, 1.0, (double) n / (double) size) : -1.0f);
    });
    if (r.failed())
        return r;

    // --- SHA-256 (mandatory) ---
    setStatus ("CHECKING " + upper, -1.0f);
    auto expected = findExpectedHash (sumsFile.loadFileAsString(), plan.assetName);
    if (! isHex64 (expected))
        return juce::Result::fail (kErrCheck);

    // Test helper: SG_TOOLS_FAKE_BAD_SHA=1 or =<tool> falsifies the expected checksum
    const auto fake = juce::SystemStats::getEnvironmentVariable ("SG_TOOLS_FAKE_BAD_SHA", {});
    if (fake == "1" || fake == toolName (t))
        expected = juce::String::repeatedString ("0", 64);

    const auto actual = juce::SHA256 (archive).toHexString().toLowerCase();
    if (threadShouldExit())
        return juce::Result::fail (kCancelled);
    if (actual != expected)
        return juce::Result::fail (kErrCheck);

    // --- Unpack: list (file in tmp, target name in bin folder) ---
    auto& staged = st.files;
    setStatus ("UNPACKING " + upper, -1.0f);

    if (t == Tool::ytdlp)
    {
        staged.add ({ archive, mainExe (t) });
    }
    else if (t == Tool::deno)
    {
        juce::ZipFile zip (archive);
        const int idx = zip.getIndexOfFileName (mainExe (t));
        if (idx < 0)
            return juce::Result::fail ("UNPACKING FAILED");
        const auto out = tmp.getChildFile ("x");
        auto zr = zip.uncompressEntry (idx, out, juce::ZipFile::OverwriteFiles::yes, juce::ZipFile::FollowSymlinks::no);
        if (zr.failed())
            return juce::Result::fail ("UNPACKING FAILED");
        staged.add ({ out.getChildFile (mainExe (t)), mainExe (t) });
    }
    else
    {
        const auto top = plan.assetName.dropLastCharacters (juce::String (kFfmpegExt).length());
        const auto out = tmp.getChildFile ("x");
        out.createDirectory();
        const juce::StringArray names { exeName ("ffmpeg"), exeName ("ffprobe") };

       #if JUCE_WINDOWS
        juce::ZipFile zip (archive);
        for (auto& n : names)
        {
            const int idx = zip.getIndexOfFileName (top + "/bin/" + n);
            if (idx < 0)
                return juce::Result::fail ("UNPACKING FAILED");
            auto zr = zip.uncompressEntry (idx, out, juce::ZipFile::OverwriteFiles::yes, juce::ZipFile::FollowSymlinks::no);
            if (zr.failed())
                return juce::Result::fail ("UNPACKING FAILED");
        }
       #else
        // JUCE cannot do tar.xz -> system tar (needs xz), arguments as a list, no shell
        juce::String out2;
        // --force-local: never read a colon in the path as host:path
        const int rc = runTool ({ "tar", "--force-local", "-xJf", archive.getFullPathName(), "-C", out.getFullPathName(),
                                  top + "/bin/" + names[0], top + "/bin/" + names[1] }, out2, 300000);
        if (threadShouldExit())
            return juce::Result::fail (kCancelled);
        if (rc != 0)
            return juce::Result::fail (rc < 0 || rc == 255 ? juce::String ("TAR/XZ MISSING - INSTALL XZ-UTILS")
                                                           : juce::String ("UNPACKING FAILED"));
       #endif

        for (auto& n : names)
            staged.add ({ out.getChildFile (top).getChildFile ("bin").getChildFile (n), n });
    }

    for (auto& s : staged)
    {
        if (! s.first.existsAsFile())
            return juce::Result::fail ("UNPACKING FAILED");
        if (s.first.isSymbolicLink())
            return juce::Result::fail (kErrCheck);   // link in the archive: never install
        s.first.setExecutePermission (true);
    }

    // --- Test run with the new file (before replacing) ---
    setStatus ("TESTING " + upper, -1.0f);
    juce::String probe;
    for (auto& s : staged)
    {
        juce::String output;
        const auto flag = t == Tool::ffmpeg ? "-version" : "--version";
        const int rc = runTool ({ s.first.getFullPathName(), flag }, output, 60000);
        if (threadShouldExit())
            return juce::Result::fail (kCancelled);
        if (rc != 0 || output.trim().isEmpty())
            return juce::Result::fail ("DOES NOT START ON THIS SYSTEM");
        if (probe.isEmpty())
            probe = juce::StringArray::fromLines (output.trim())[0].trim().substring (0, 60);
    }

    st.sha = actual;
    st.probe = probe;
    return juce::Result::ok();
}

// Replaces a file and creates a backup (backup empty = there was no file before).
// POSIX: hard link as backup, then rename (atomic, target never gone). Windows: a running .exe can be renamed
// but not overwritten -> rename the old one away, move the new one in, on error move back.
static bool replaceWithBackup (const juce::File& src, const juce::File& target, juce::File& backup)
{
    backup = juce::File();
    if (! target.existsAsFile())
        return src.moveFileTo (target);

    const auto bak = target.getSiblingFile (target.getFileName() + ".old-" + juce::String (juce::Time::currentTimeMillis()));
   #if JUCE_WINDOWS
    if (! target.moveFileTo (bak))
        return false;
    if (! src.moveFileTo (target))
    {
        bak.moveFileTo (target);   // back
        return false;
    }
   #else
    if (::link (target.getFullPathName().toRawUTF8(), bak.getFullPathName().toRawUTF8()) != 0
        && ! target.copyFileTo (bak))
        return false;
    if (! src.replaceFileIn (target))
    {
        bak.deleteFile();
        return false;
    }
   #endif
    backup = bak;
    return true;
}

// All or nothing: ffmpeg + ffprobe both or neither; tools.xml only after full success
juce::Result ToolManager::commitOne (const Plan& plan, const Staged& st)
{
    const auto dir = toolsDir();
    struct Done { juce::File target, backup; };
    juce::Array<Done> done;

    // If rename fails, JUCE may copy a NON-existent source as an empty file -> check beforehand
    for (auto& s : st.files)
        if (! s.first.existsAsFile() || s.first.getSize() <= 0)
            return juce::Result::fail (kErrBroken);

    for (auto& s : st.files)
    {
        const auto target = dir.getChildFile (s.second);
        juce::File backup;
        if (! replaceWithBackup (s.first, target, backup))
        {
            // Roll back the files already replaced
            for (int i = done.size(); --i >= 0;)
            {
                if (done[i].backup != juce::File())
                    done.getReference (i).backup.moveFileTo (done[i].target);
                else
                    done.getReference (i).target.deleteFile();
            }
            return juce::Result::fail ("FILE IN USE - CLOSE OTHER SAMPLE GRABBERS");
        }
        target.setExecutePermission (true);
        done.add ({ target, backup });
    }

    for (auto& d : done)
        if (d.backup != juce::File())
            d.backup.deleteFile();   // Windows: fails while the old program is running -> clean up in the next run

    writeToolRecord (plan, st.sha);
    setToolState (plan.tool, State::ok, st.probe);
    return juce::Result::ok();
}

void ToolManager::writeToolRecord (const Plan& plan, const juce::String& sha)
{
    auto xml = loadXml();
    auto* e = xml->getChildByAttribute ("id", toolName (plan.tool));
    if (e == nullptr)
    {
        e = xml->createNewChildElement ("TOOL");
        e->setAttribute ("id", toolName (plan.tool));
    }
    e->setAttribute ("version", plan.version);
    e->setAttribute ("asset", plan.assetName);
    e->setAttribute ("sha256", sha);
    e->setAttribute ("installed", juce::String (juce::Time::currentTimeMillis()));
    e->setAttribute ("latest", plan.version);
    xml->writeTo (toolsDir().getChildFile ("tools.xml"));
}

//==============================================================================
// HTTPS download into a file (follows redirects, e.g. github.com -> release-assets.githubusercontent.com).
// Linux: system curl (https only, also on redirects), otherwise juce::URL (WinINet/NSURLSession).
juce::Result ToolManager::download (const juce::String& url, const juce::File& dest, juce::int64 expectedSize,
                                    std::function<void (juce::int64)> onBytes)
{
    dest.deleteFile();

    if (useCurl())
    {
        // Without -f: the error page ends up in dest (rate-limit text), status code of the last response via -w on stdout
        const juce::StringArray args { "curl", "-sSL", "--proto", "=https", "--proto-redir", "=https",
                                       "--connect-timeout", "20", "--retry", "2",
                                       "-A", kUserAgent, "-H", "Accept: application/octet-stream, application/json",
                                       "-w", "%{http_code}", "-o", dest.getFullPathName(), url };
        bool started = false;
        {
            const juce::ScopedLock sl (procLock);
            proc = std::make_unique<juce::ChildProcess>();
            started = proc->start (args, juce::ChildProcess::wantStdOut);   // only the status code (3 characters); progress via file size
        }
        if (! started)
            return juce::Result::fail (kErrCurl);

        while (proc->isRunning())
        {
            if (threadShouldExit())
            {
                const juce::ScopedLock sl (procLock);
                proc->kill();
                break;
            }
            if (onBytes)
                onBytes (dest.getSize());
            wait (100);
        }
        proc->waitForProcessToFinish (5000);
        const auto codeText = proc->readAllProcessOutput().trim();
        const auto rc = (int) proc->getExitCode();
        {
            const juce::ScopedLock sl (procLock);
            proc.reset();
        }

        if (threadShouldExit())
            return juce::Result::fail (kCancelled);
        if (rc != 0)
            return juce::Result::fail (curlError (rc));
        const int code = codeText.getIntValue();
        if (code >= 400 || code == 0)
        {
            dest.deleteFile();
            return juce::Result::fail (httpError (code));
        }
    }
    else
    {
        int status = 0;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withExtraHeaders ("User-Agent: " + juce::String (kUserAgent))
                        .withConnectionTimeoutMs (20000)
                        .withNumRedirectsToFollow (5)
                        .withStatusCode (&status);
        auto in = juce::URL (url).createInputStream (opts);
        if (in == nullptr)
            return juce::Result::fail (status >= 400 ? httpError (status) : juce::String (kErrNoNet));
        if (status >= 400)
            return juce::Result::fail (httpError (status));

        juce::FileOutputStream out (dest);
        if (! out.openedOk())
            return juce::Result::fail (kErrWrite);

        juce::HeapBlock<char> buf (1 << 16);
        juce::int64 total = 0;
        for (;;)
        {
            if (threadShouldExit())
                return juce::Result::fail (kCancelled);
            const int n = in->read (buf.get(), 1 << 16);
            if (n <= 0)
                break;
            if (! out.write (buf.get(), (size_t) n))
                return juce::Result::fail ("DISK FULL - FREE SOME SPACE");
            total += n;
            if (onBytes)
                onBytes (total);
        }
        out.flush();
        if (! in->isExhausted())
            return juce::Result::fail (kErrBroken);
    }

    if (! dest.existsAsFile())
        return juce::Result::fail (kErrBroken);
    if (expectedSize >= 0 && dest.getSize() != expectedSize)
        return juce::Result::fail (kErrBroken);
    return juce::Result::ok();
}

// Short helper process (tar, --version). Reads everything, kills on threadShouldExit/timeout.
int ToolManager::runTool (const juce::StringArray& args, juce::String& output, int timeoutMs)
{
    bool started = false;
    {
        const juce::ScopedLock sl (procLock);
        proc = std::make_unique<juce::ChildProcess>();
        started = proc->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr);
    }
    if (! started)
        return -1;

    // Wait with timeout/abort; the outputs (tar errors, --version) are small and fit in the pipe buffer,
    // reading happens only after the end (readProcessOutput would block until the process ends).
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (proc->isRunning())
    {
        if (threadShouldExit() || juce::Time::getMillisecondCounter() > deadline)
        {
            const juce::ScopedLock sl (procLock);
            proc->kill();
            break;
        }
        wait (20);
    }

    output = proc->readAllProcessOutput();
    proc->waitForProcessToFinish (5000);
    const int rc = (int) proc->getExitCode();
    {
        const juce::ScopedLock sl (procLock);
        proc.reset();
    }
    return rc;
}

//==============================================================================
int ToolManager::runSelfTest (const juce::String& mode)
{
    auto say = [] (const juce::String& s) { std::cerr << "[SG-TOOLS] " << s.toStdString() << std::endl; };

    juce::SharedResourcePointer<ToolManager> tm;
    say ("dir: " + toolsDir().getFullPathName() + (pathFallbackAllowed() ? "" : " (PATH fallback off)"));

    auto runAndWait = [&] (const juce::String& what, std::function<bool()> start)
    {
        const int before = tm->getRunCounter();
        say (what + ": start");
        const auto t0 = juce::Time::getMillisecondCounter();
        if (! start())
        {
            say (what + ": could not start - " + tm->getLastNote());
            return false;
        }
        juce::String lastShown;
        while (tm->getRunCounter() == before)
        {
            juce::Thread::sleep (250);
            const float p = tm->getProgress();
            const auto s = tm->getStatusText() + (p >= 0.0f ? " " + juce::String ((int) (p * 100.0f)) + " %" : juce::String());
            if (s != lastShown && s.isNotEmpty() && (p < 0.0f || ((int) (p * 100.0f)) % 10 == 0))
            {
                say ("  " + s);
                lastShown = s;
            }
        }
        say (what + ": " + tm->getLastNote() + (tm->lastNoteIsError() ? "  [ERROR]" : "")
             + "  (" + juce::String ((juce::Time::getMillisecondCounter() - t0) / 1000.0, 1) + " s)");
        return ! tm->lastNoteIsError();
    };

    bool ok = true;
    if (mode == "force")
        ok = runAndWait ("updateAll(force)", [&] { return tm->updateAll (true); });
    else if (mode == "update")
        ok = runAndWait ("updateAll", [&] { return tm->updateAll (false); });
    else
        ok = runAndWait ("installMissing", [&] { return tm->installMissing(); });

    const bool checkOk = runAndWait ("checkForUpdates", [&] { return tm->checkForUpdates(); });
    ok = ok && checkOk;

    tm->refresh();
    for (int i = 0; i < numTools; ++i)
    {
        const auto in = tm->getInfo ((Tool) i);
        juce::String line = toolName ((Tool) i) + ": ";
        if (in.path == juce::File())
        {
            line << "MISSING";
            ok = false;
        }
        else
        {
            juce::String probe;
            const auto flag = (Tool) i == Tool::ffmpeg ? "-version" : "--version";
            juce::ChildProcess cp;
            if (cp.start (juce::StringArray { in.path.getFullPathName(), flag }))
            {
                probe = juce::StringArray::fromLines (cp.readAllProcessOutput().trim())[0].trim().substring (0, 70);
                cp.waitForProcessToFinish (10000);
            }
            line << "version=" << in.version << (in.build.isNotEmpty() ? " build=" + in.build : juce::String())
                 << " latest=" << in.latest << (in.updateAvailable ? " UPDATE-AVAILABLE" : "")
                 << " size=" << juce::String ((double) in.path.getSize() / (1024.0 * 1024.0), 1) << "MB"
                 << " sha256=" << (in.sha256.isNotEmpty() ? in.sha256.substring (0, 16) + "... (verified at install)" : juce::String ("-"))
                 << "\n      path=" << in.path.getFullPathName()
                 << "\n      runs: " << probe;
            if (in.message.isNotEmpty())
                line << "\n      last message: " << in.message;
        }
        say (line);
    }

    say (ok ? "RESULT: OK" : "RESULT: FAILED");
    return ok ? 0 : 1;
}
