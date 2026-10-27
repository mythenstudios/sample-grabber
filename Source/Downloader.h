#pragma once
#include <juce_core/juce_core.h>
#include "ToolManager.h"
#include <functional>
#include <memory>

// External processes (yt-dlp, ffmpeg) on a background thread - never on the audio thread!
// Tool paths come from the ToolManager (own bin folder, else PATH).
//  - grab():       URL -> yt-dlp -> WAV
//  - importFile(): mp3/mp4/m4a/... -> ffmpeg -> WAV
class Downloader : private juce::Thread
{
public:
    struct Result
    {
        bool ok = false;
        juce::File file;
        juce::String message;
    };

    Downloader() : juce::Thread ("external-process") {}
    ~Downloader() override { cancel(); }

    // Both callbacks arrive on the message thread.
    // onProgress: 0..1 and a phase ("DOWNLOADING", "CONVERTING")
    void grab (const juce::String& url,
               std::function<void (float, juce::String)> onProgress,
               std::function<void (Result)> onDone);

    void importFile (const juce::File& file,
                     std::function<void (float, juce::String)> onProgress,
                     std::function<void (Result)> onDone);

    bool isBusy() const { return isThreadRunning(); }
    void cancel();

private:
    enum class Job { download, convert };

    void run() override;
    void runDownload (Result& r);
    void runConvert (Result& r);
    void reportProgress (float p, const juce::String& phase);
    static juce::File dataDir (const juce::String& sub);

    Job job = Job::download;
    juce::String pendingUrl;
    juce::File pendingFile;
    std::function<void (float, juce::String)> progressCb;
    std::function<void (Result)> doneCb;

    juce::CriticalSection procLock;
    std::unique_ptr<juce::ChildProcess> proc;

    juce::SharedResourcePointer<ToolManager> tools;
};
