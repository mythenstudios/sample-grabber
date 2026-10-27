#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "ToolManager.h"
#include "Storage.h"
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// Sample Grabber: loads a sample (via yt-dlp or file), cuts it into 16 slices
// (grid or transients); MIDI notes 36..51 (C1..D#2) trigger the pads.
// Pitch and speed (tempo) are independent: a background thread pre-renders the
// selection with Signalsmith Stretch, playback only reads the finished result.
class SampleGrabberProcessor : public juce::AudioProcessor,
                               private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr int numPads = 16;
    static constexpr int firstPadNote = 36;
    static constexpr int fullRegionPad = numPads;   // trigger code: play the whole selection

    enum class SliceMode { grid = 0, transient = 1, beat = 2 };

    struct PeakColumn { float mn = 0.0f, mx = 0.0f, rms = 0.0f; };

    // Gain parameter: gainMinDb (left stop) means silent (-inf), 0 dB sits at the knob centre
    static constexpr float gainMinDb = -60.0f;
    static constexpr float gainMaxDb = 12.0f;
    static float gainFromDb (float db) { return juce::Decibels::decibelsToGain (db, gainMinDb); }   // <= gainMinDb -> 0

    // Fades: length as fraction of the region (pad or selection), curve shape -100..+100, fixed click guard at every region boundary
    static constexpr float fadeMaxPct = 50.0f;
    static constexpr double clickGuardSeconds = 0.002;

    // Fade curve exponent from the shape: p = 2^(shape/50) (0 = linear, +100 = x^4 soft, -100 = x^0.25 fast).
    // Compute once per block or export, not per sample.
    static double fadeExponent (double shape) { return std::exp2 (shape / 50.0); }

    // Shared envelope for playback, export and display (all threads, no allocation, no state).
    // pos/start/end in samples of the respective source (original or render), end exclusive.
    // fadeIn/OutFrac: fraction of the region length (0..0.5), at most half the region each.
    // Curve: progress x in [0,1] -> x^exponent (see fadeExponent); for fade-out x is the remaining fraction.
    // Plus a linear click guard of clickInSamples at the start and clickOutSamples at the end (at most half the region each,
    // 0 = off; off at the loop wrap so the loop stays seamless). The minimum of all factors applies.
    // Outside the ramps 1.0 without pow.
    static float fadeEnvelope (double pos, double start, double end, double fadeInFrac, double fadeOutFrac,
                               double exponent, double clickInSamples, double clickOutSamples)
    {
        const double len = end - start;
        if (len <= 0.0)
            return 0.0f;

        const double half = 0.5 * len;
        const double fi = juce::jlimit (0.0, half, fadeInFrac * len);
        const double fo = juce::jlimit (0.0, half, fadeOutFrac * len);
        const double ci = juce::jlimit (0.0, half, clickInSamples);
        const double co = juce::jlimit (0.0, half, clickOutSamples);
        const double rel = pos - start;
        const double rem = end - pos;

        if (rel >= juce::jmax (fi, ci) && rem >= juce::jmax (fo, co))
            return 1.0f;   // normal case: outside all ramps

        double env = 1.0;
        if (fi > 1.0 && rel < fi)
            env = juce::jmin (env, std::pow (juce::jmax (0.0, rel / fi), exponent));
        if (fo > 1.0 && rem < fo)
            env = juce::jmin (env, std::pow (juce::jmax (0.0, rem / fo), exponent));
        if (ci > 1.0 && rel < ci)
            env = juce::jmin (env, juce::jmax (0.0, rel / ci));
        if (co > 1.0 && rem < co)
            env = juce::jmin (env, juce::jmax (0.0, rem / co));
        return (float) env;
    }

    SampleGrabberProcessor();
    ~SampleGrabberProcessor() override;

    void prepareToPlay (double sampleRate, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Sample Grabber"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- Message-Thread-API (UI) ----
    // recordHistory = true: the previous state (sample, trim, slice mode, tempo octave) is pushed onto the BACK stack.
    // sourceUrl: link the file was grabbed from (saved in the plugin state for re-grabbing), empty for local files.
    bool loadSample (const juce::File& file, bool recordHistory = true, const juce::String& sourceUrl = {});

    // BACK: return to the state before the last load without re-fetching (message thread).
    // The stack lives in RAM only (not in plugin state): at most maxHistoryDepth levels, oldest are dropped first.
    bool canGoBack() const { return ! history.empty(); }
    bool goBack();
    bool hasSample() const;
    juce::File getLoadedFile() const { return loadedFile; }
    double getFileSampleRate() const { return fileRate.load(); }
    int getFileBitDepth() const { return fileBitDepth.load(); }
    int getNumSamples() const;

    float getTrimStart() const { return trimStart.load(); }
    float getTrimEnd() const { return trimEnd.load(); }
    void setTrim (float startNorm, float endNorm, bool recompute = true);
    void commitSlices() { recomputeSlices(); }

    void setSliceMode (SliceMode m);
    SliceMode getSliceMode() const { return (SliceMode) sliceMode.load(); }
    std::array<float, numPads + 1> getSliceBoundsNorm() const;
    int getSliceVersion() const { return sliceVersion.load(); }

    std::vector<PeakColumn> computePeaks (int columns, float startNorm = 0.0f, float endNorm = 1.0f) const;

    // Transport
    void triggerPad (int pad) { pendingTrigger.store (pad); }
    // Selection changed: if something is playing (not paused), it restarts with the new bounds (pad or whole selection, also in loop).
    void retriggerIfPlaying() { retriggerRequested.store (true); }
    void stop() { stopRequested.store (true); }
    void pause() { pauseRequested.store (true); }
    bool isPaused() const { return pausedFlag.load(); }
    int getActivePad() const { return activePad.load(); }
    float getPlayheadNorm() const { return playheadNorm.load(); }
    bool isRendering() const { return renderBusy.load() || renderDirty.load(); }

    // Parameter display
    float getFadeInPct() const { return fadeInParam->load(); }    // 0..fadeMaxPct, fraction of the region in %
    float getFadeOutPct() const { return fadeOutParam->load(); }
    float getFadeShape() const { return fadeShapeParam->load(); } // -100..+100, 0 = linear
    bool isLoopOn() const { return loopParam->load() > 0.5f; }
    float getSpeedFactor() const { return speedParam->load() / 100.0f; }
    float getGainDb() const { return gainParam->load(); }

    // Peak level (linear, over all channels) of what the selection currently plays or exports:
    // the matching render with pitch/speed, else the original. Without gain and fades. Message thread, scans the
    // whole selection -> call only when getPeakKey() changes, not on every mouse move.
    float computeSelectionPeak() const;
    // Changes whenever the basis of computeSelectionPeak() changes (selection, sample, render, pitch/speed neutral)
    juce::int64 getPeakKey() const;

    // Analysis of the current selection (BPM, key) - message thread, call only when the mouse is released
    void analyse();
    int getSelectionVersion() const { return selectionVersion.load(); }
    float getBpm() const { return bpmValue.load(); }
    int getKey() const { return keyValue.load(); }          // 0..11 = major (C..B), 12..23 = minor, -1 = unknown
    float getKeyConfidence() const { return keyConfidence.load(); }

    // Tempo octave: user correction for half/double tempo (x2 = +1, 1/2 = -1, limited to +-2).
    // Takes effect immediately in getBpm() and survives new analyses; reset when a new file is loaded.
    int getBpmOctave() const { return bpmOctave.load(); }
    void shiftBpmOctave (int delta);   // Message-Thread
    // true if shiftBpmOctave (delta) would change something: tempo known, octave stays within +-2, new tempo in [30, 400] BPM
    // (outside that range BEAT would silently fall back to GRID).
    bool canShiftBpmOctave (int delta) const;

    // Drag-out (message thread): finished 24-bit WAV of a pad (0..15) or the whole selection (fullRegionPad) incl.
    // gain/pitch/speed/fades in the export folder <Music>/Sample Grabber/<title>/. Normally a hard link/copy of the file
    // the background worker pre-rendered; if that is not ready, rendered synchronously (logged). Empty = nothing to export.
    // Names carry a hash of the audio content + parameters: an existing file is reused, never overwritten.
    juce::File prepareDragFile (int pad);

    // Plugin state restore (setStateInformation) -> editor refresh: bumped after every restore (message thread)
    int getStateVersion() const { return stateVersion.load(); }
    juce::String getRestoreNote() const;     // e.g. "SOURCE MISSING - USING SAVED SELECTION", empty = normal
    juce::String getSourceUrl() const;       // link of the loaded sample, empty for local files

    // Self tests (SG_STATE_SELFTEST / SG_EXPORT_SELFTEST): wait until the background worker has nothing left to do
    bool waitForBackgroundIdle (int timeoutMs);
    size_t getEmbeddedAudioBytes() const;
    bool isStagedReady (int pad) const;      // staged file for the current state is ready (fast drag path)
    static void setEmbedCapForTest (size_t bytes);

    // Selection too long/large to embed in the project (> 10 min or > 32 MB FLAC): bumped once per sample (editor note)
    int getEmbedWarnVersion() const { return embedWarnVersion.load(); }

    juce::AudioProcessorValueTreeState apvts;

    // Set by the standalone app only (message thread)
    std::function<void()> onOpenAudioSettings;
    juce::String audioInfo;
    bool audioOk = true;

    // External tools (yt-dlp/deno/ffmpeg), one object per process. Keeps the ToolManager alive as long as a
    // plugin instance exists (installation continues when the editor is closed). Never use on the audio thread.
    juce::SharedResourcePointer<ToolManager> tools;
    juce::SharedResourcePointer<CacheManager> cache;   // cache size/cleaning, one per process

private:
    struct RenderedAudio
    {
        juce::AudioBuffer<float> buffer;
        int regionStart = 0, regionEnd = 0;   // source range (samples in the original) covered by this render
        double speed = 1.0;                   // tempo factor: render length = source length / speed
        float pitch = 0.0f;                   // semitones the render was made with
    };

    class Renderer;
    class BackgroundWorker;

    // Everything an export needs, captured at one moment (any thread except audio). The writer works only on this.
    struct ExportJob
    {
        std::shared_ptr<juce::AudioBuffer<float>> buf;
        std::shared_ptr<RenderedAudio> rnd;   // matching pitch/speed render, nullptr = original or needsRender
        bool stretched = false;               // pitch/speed active (and selection long enough to render)
        bool needsRender = false;             // stretched, but no matching render yet: the writer renders itself
        int pad = 0;
        bool whole = false;
        int selStart = 0, selEnd = 0;         // selection in the original
        int srcStart = 0, srcEnd = 0;         // pad (or selection) in the original
        double rate = 44100.0, speed = 1.0;
        float pitch = 0.0f, gainDb = 0.0f, fadeInPct = 0.0f, fadeOutPct = 0.0f, fadeShape = 0.0f;
        juce::String title;
        double outputSeconds() const { return (double) (srcEnd - srcStart) / (stretched ? speed : 1.0) / rate; }
    };

    bool captureExportJob (int pad, ExportJob& job) const;
    static juce::uint64 hashExportJob (const ExportJob&);
    static juce::String exportFileName (const ExportJob&, juce::uint64 hash);
    static bool writeExport (const ExportJob&, const juce::File& target, const std::function<bool()>& shouldAbort);
    juce::uint64 computeStageKey() const;   // cheap fingerprint of everything that changes the exports (no audio scan)
    juce::uint64 computeEmbedKey() const;   // sample + selection

    // State restore, applied on the message thread
    struct RestoreInfo
    {
        juce::String path, url;
        float trimStart = 0.0f, trimEnd = 1.0f;
        int sliceMode = (int) SliceMode::transient;
        float bpm = 0.0f;
        int bpmOctave = 0;
        bool hasBpm = false;
        std::shared_ptr<const juce::MemoryBlock> flac;   // embedded selection audio, nullptr = none
        float embTrimStart = 0.0f, embTrimEnd = 1.0f;    // selection the embedded audio covers (original coordinates)
    };
    void applyRestore (const RestoreInfo&);
    void applyPendingRestore();   // message thread: applies pendingRestore (newest wins), then clears it
    bool installFromReader (juce::AudioFormatReader& reader, const juce::File& file, bool recordHistory, const juce::String& url,
                            bool embedded = false, float embTrimStart = 0.0f, float embTrimEnd = 1.0f);
    void setMeta (const juce::File& file, const juce::String& url, bool embedded, float embTrimStart, float embTrimEnd);
    void seedEmbedded (std::shared_ptr<const juce::MemoryBlock> flac);   // restored FLAC = whole current selection
    void updateInUse();   // in-use registry for cache cleaning (loaded file + BACK history), message thread

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static std::shared_ptr<RenderedAudio> renderStretched (const juce::AudioBuffer<float>& src, int a, int b, double sr,
                                                           float pitchSemitones, double speed,
                                                           const std::function<bool()>& shouldAbort);

    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void markRenderDirty();
    void doRender();

    std::pair<int, int> getRegion (int len) const;
    std::array<int, numPads + 1> computeBoundsFor (const juce::AudioBuffer<float>& b) const;
    std::shared_ptr<juce::AudioBuffer<float>> getSnapshot() const;
    void recomputeSlices();

    // Shared part of loadSample() and goBack(): compute slices, switch sample live, bump versions, kick off render.
    // Requires fileRate, trim, fluxCache, tempo/key and slice mode to already match the sample.
    void installSample (std::shared_ptr<juce::AudioBuffer<float>> newSample, const juce::File& file, bool forceLog);
    void pushHistory();   // save the current state (only if a sample is loaded)

    struct HistoryEntry
    {
        std::shared_ptr<juce::AudioBuffer<float>> sample;   // shared pointer, no copy of the buffer
        std::vector<float> flux;
        juce::File file;
        juce::String url;
        bool embedded = false;                              // sample = embedded selection, file is the missing original
        float embTrimStart = 0.0f, embTrimEnd = 1.0f;
        double sampleRate = 44100.0;
        int bitDepth = 16;
        float trimStart = 0.0f, trimEnd = 1.0f;
        int sliceMode = (int) SliceMode::transient;
        int bpmOctave = 0;
        float bpm = 0.0f;
        int key = -1;
        float keyConfidence = 0.0f;
    };

    static constexpr int maxHistoryDepth = 5;
    static constexpr size_t maxHistoryBytes = (size_t) 512 * 1024 * 1024;   // total of the sample buffers in the stack
    std::vector<HistoryEntry> history;   // oldest first, message thread only

    juce::AudioFormatManager formats;
    mutable juce::SpinLock lock;
    std::shared_ptr<juce::AudioBuffer<float>> sample;
    std::shared_ptr<RenderedAudio> rendered;
    std::array<int, numPads + 1> sliceBounds {};
    juce::File loadedFile;
    std::vector<float> fluxCache;   // transient curve, message thread only

    std::atomic<double> fileRate { 44100.0 };
    std::atomic<int> fileBitDepth { 16 };
    std::atomic<float> trimStart { 0.0f }, trimEnd { 1.0f };
    std::atomic<int> sliceMode { (int) SliceMode::transient };

    std::atomic<int> pendingTrigger { -1 }, activePad { -1 };
    std::atomic<bool> retriggerRequested { false };
    std::atomic<bool> stopRequested { false }, pauseRequested { false }, pausedFlag { false };
    std::atomic<int> sliceVersion { 0 }, selectionVersion { 0 };
    std::atomic<int> renderVersion { 0 };   // +1 whenever sample/rendered are replaced (for the peak cache in the editor)
    std::atomic<float> playheadNorm { -1.0f };

    std::atomic<float> bpmValue { 0.0f };
    std::atomic<int> bpmOctave { 0 };
    std::atomic<int> keyValue { -1 };
    std::atomic<float> keyConfidence { 0.0f };

    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* pitchParam = nullptr;
    std::atomic<float>* speedParam = nullptr;
    std::atomic<float>* fadeInParam = nullptr;
    std::atomic<float>* fadeOutParam = nullptr;
    std::atomic<float>* fadeShapeParam = nullptr;
    std::atomic<float>* loopParam = nullptr;

    // Metadata for state, export names and the editor (message thread writes; getStateInformation and the worker read).
    // Never touched by the audio thread, so a normal lock is fine.
    mutable juce::CriticalSection metaLock;
    juce::String metaPath;          // path written to the state (original file, also when it is missing)
    juce::String metaTitle;         // export folder/file title
    juce::String metaUrl;           // link the sample was grabbed from
    juce::String restoreNote;
    bool embeddedMode = false;      // sample is the embedded selection of a missing source
    float embOrigTrimStart = 0.0f, embOrigTrimEnd = 1.0f;   // embedded audio = this range of the original (normalised)
    std::shared_ptr<const RestoreInfo> pendingRestore;       // set by setStateInformation until applyRestore ran
    std::shared_ptr<const juce::MemoryBlock> embeddedFlac;   // cached FLAC of the current selection (worker encodes)
    juce::uint64 embeddedFlacKey = 0;                        // computeEmbedKey() it belongs to
    int embeddedFlacSerial = -1;                             // sampleSerial it was encoded from
    float embeddedFlacTrimStart = 0.0f, embeddedFlacTrimEnd = 1.0f;   // selection it covers (normalised, current sample)
    std::atomic<juce::uint64> embeddedDoneKey { 0 };         // key the worker last handled (encoded or skipped)

    std::atomic<int> embedWarnSerial { -1 }, embedWarnVersion { 0 };
    std::atomic<int> sampleSerial { 0 };   // +1 per installed sample (identity for keys, unlike recycled pointers)
    std::atomic<int> stateVersion { 0 };

    std::unique_ptr<Renderer> renderer;
    std::atomic<bool> renderDirty { false }, renderBusy { false };
    std::atomic<juce::uint32> lastChangeMs { 0 };

    // Graveyard: every published sample/render pointer lands here when replaced (outside the SpinLock).
    // This way the last reference is never dropped on the audio thread (voiceRender/xfadeRender/block locals) or under the SpinLock.
    // Writing: message thread and renderer thread. Clearing: message thread only (graveyardTimer). The audio thread never touches it.
    void retire (std::shared_ptr<juce::AudioBuffer<float>> old);
    void retire (std::shared_ptr<RenderedAudio> old);
    void sweepGraveyard();   // message thread: frees entries that are referenced only here
    std::mutex graveyardMutex;
    std::vector<std::shared_ptr<juce::AudioBuffer<float>>> graveSamples;
    std::vector<std::shared_ptr<RenderedAudio>> graveRenders;
    juce::TimedCallback graveyardTimer { [this] { sweepGraveyard(); } };   // declared after the graveyard -> destroyed first

    // audio thread only
    double hostRate = 44100.0;
    double voicePos = 0.0;
    int voiceStart = 0, voiceEnd = 0;
    bool voicePlaying = false, voiceWrapped = false, voiceUsesRender = false, paused = false;
    int voiceSrcStart = 0, voiceSrcEnd = 0;       // slice bounds in the original, regardless of whether a render is running
    int voiceTrigger = -1;                         // pad (0..15) or fullRegionPad of the running voice, for the restart on selection change
    float rampGain = 1.0f;                         // fade in/out for pause/stop/resume
    juce::SmoothedValue<float> gainSmoothed { 1.0f };   // gain, linearly smoothed (~20 ms) so that silent = 0 is reachable
    int rampDir = 0;                               // -1 fading out, +1 fading in, 0 stable
    enum class RampThen { none, stop, pause } rampThen = RampThen::none;
    std::shared_ptr<RenderedAudio> voiceRender;

    // Crossfade when switching original <-> render so pitch/speed changes during playback do not click
    std::shared_ptr<RenderedAudio> xfadeRender;
    bool xfadeFromOriginal = false;
    double xfadePos = 0.0;
    int xfadeEnd = 0, xfadeLeft = 0, xfadeTotal = 1;

    std::unique_ptr<BackgroundWorker> worker;   // staging + FLAC; declared last -> stopped first in the destructor

    JUCE_DECLARE_WEAK_REFERENCEABLE (SampleGrabberProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleGrabberProcessor)
};
