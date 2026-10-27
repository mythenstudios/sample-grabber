#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "ui/SGLookAndFeel.h"
#include "AudioSettingsPanel.h"
#include <cmath>
#include <functional>
#include <limits>
#include <thread>

// Window content: the plugin editor plus the audio panel as an overlay on top.
class AppContent : public juce::Component
{
public:
    AppContent (juce::AudioProcessorEditor* ed, juce::AudioDeviceManager& dm)
        : editor (ed), panel (dm, [this] { showPanel (false); })
    {
        addAndMakeVisible (*editor);
        addChildComponent (panel);
        setSize (editor->getWidth(), editor->getHeight());
    }

    void resized() override
    {
        editor->setBounds (getLocalBounds());
        panel.setBounds (getLocalBounds());
    }

    juce::AudioProcessorEditor* getEditor() const { return editor.get(); }

    void showPanel (bool show)
    {
        panel.setVisible (show);
        if (show)
            panel.toFront (true);
    }

private:
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    AudioSettingsPanel panel;
};

// Standalone Sample Grabber app (no DAW): own audio device management,
// flat Swiss window frame instead of the JUCE default bar with "Options" menu.
class MainWindow : public juce::DocumentWindow,
                   private juce::ChangeListener
{
public:
    MainWindow()
        : juce::DocumentWindow ("Sample Grabber", ui::c().canvas,
                                juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton)
    {
        setLookAndFeel (&laf);
        setUsingNativeTitleBar (false);
        setTitleBarHeight (34);

        settingsFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                           .getChildFile ("SampleGrabber").getChildFile ("audio.xml");
        auto saved = juce::parseXML (settingsFile);

        processor = std::make_unique<SampleGrabberProcessor>();
        processor->onOpenAudioSettings = [this] { if (content != nullptr) content->showPanel (true); };

        deviceManager.initialise (0, 2, saved.get(), true);

        for (const auto& d : juce::MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (d.identifier, true);

        player.setProcessor (processor.get());
        deviceManager.addAudioCallback (&player);
        deviceManager.addMidiInputDeviceCallback ({}, &player);
        deviceManager.addChangeListener (this);
        updateAudioInfo();

        content = new AppContent (processor->createEditorIfNeeded(), deviceManager);
        setContentOwned (content, true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~MainWindow() override
    {
        if (auto xml = deviceManager.createStateXml())
        {
            settingsFile.getParentDirectory().createDirectory();
            xml->writeTo (settingsFile);
        }

        deviceManager.removeChangeListener (this);
        content = nullptr;
        clearContentComponent();   // destroy the editor before the processor
        deviceManager.removeMidiInputDeviceCallback ({}, &player);
        deviceManager.removeAudioCallback (&player);
        player.setProcessor (nullptr);
        processor.reset();
        setLookAndFeel (nullptr);
    }

    // Developer helper (SG_SNAPSHOT): optionally load a file, then write the whole window as PNG (1x and 2x)
    // to <dir>/<theme>.png and <theme>@2x.png and quit the app.
    void runSnapshot (const juce::File& dir, const juce::File& audio)
    {
        if (audio.existsAsFile() && content != nullptr)
            if (auto* target = dynamic_cast<juce::FileDragAndDropTarget*> (content->getEditor()))
                target->filesDropped ({ audio.getFullPathName() }, 0, 0);

        // SG_SNAPSHOT_GAIN=<dB>: set the gain for the image (e.g. overload indicator); plugin state is not saved in the app
        const auto snapGain = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_GAIN", {});
        if (snapGain.isNotEmpty() && processor != nullptr)
            if (auto* gp = processor->apvts.getParameter ("gain"))
                gp->setValueNotifyingHost (gp->convertTo0to1 (snapGain.getFloatValue()));

        // SG_SNAPSHOT_FADES=<in>,<out>,<shape>: set fades for the image (e.g. 15,25,40), percent resp. -100..+100
        const auto snapFades = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_FADES", {}), ",", {});
        if (snapFades.size() == 3 && processor != nullptr)
        {
            const char* ids[] = { "fadeInPct", "fadeOutPct", "fadeShape" };
            for (int i = 0; i < 3; ++i)
                if (auto* fp = processor->apvts.getParameter (ids[i]))
                    fp->setValueNotifyingHost (fp->convertTo0to1 (snapFades[i].trim().getFloatValue()));
        }

        juce::Timer::callAfterDelay (audio.existsAsFile() ? 4000 : 1000, [this, dir]
        {
            dir.createDirectory();
            juce::String name = ui::theme() == ui::ThemeId::classic ? "classic" : "dark";

            // SG_SNAPSHOT_DRAG=1: show the drop state (file is dragged over the window)
            if (juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_DRAG", {}).isNotEmpty())
                if (auto* target = dynamic_cast<juce::FileDragAndDropTarget*> (content->getEditor()))
                {
                    target->fileDragEnter ({ "drop.wav" }, 0, 0);
                    name += "_drag";
                }

            for (const float scale : { 1.0f, 2.0f })
            {
                const auto img = createComponentSnapshot (getLocalBounds(), true, scale);
                const auto f = dir.getChildFile (name + (scale > 1.5f ? "@2x" : "") + ".png");
                f.deleteFile();
                juce::FileOutputStream out (f);
                if (out.openedOk())
                    juce::PNGImageFormat().writeImageToStream (img, out);
            }

            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        });
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateAudioInfo(); }

    void updateAudioInfo()
    {
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            processor->audioOk = true;
            processor->audioInfo = dev->getName().toUpperCase() + " " + juce::String (dev->getCurrentSampleRate() / 1000.0, 1) + "K";
        }
        else
        {
            processor->audioOk = false;
            processor->audioInfo = "NO AUDIO DEVICE";
        }
    }

    SGLookAndFeel laf;
    juce::AudioDeviceManager deviceManager;
    juce::AudioProcessorPlayer player;
    std::unique_ptr<SampleGrabberProcessor> processor;
    AppContent* content = nullptr;
    juce::File settingsFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
// Self tests without a window (output on stderr, exit code 0 = ok). Always run with SG_CACHE_DIR (and SG_EXPORT_DIR)
// pointing to scratch folders so the user's real cache/exports are never touched.
namespace selftest
{
    void printState (const char* tag, SampleGrabberProcessor& p)
    {
        auto param = [&] (const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        std::fprintf (stderr, "[SG-STATE] %s: hasSample=%d len=%d (%.3f s) rate=%.0f trim=%.4f..%.4f sliceMode=%d bpm=%.2f octave=%d "
                              "gain=%.1f pitch=%.2f speed=%.1f fadeIn=%.1f fadeOut=%.1f shape=%.0f url='%s' note='%s' file='%s' embedded=%d bytes\n",
                      tag, p.hasSample() ? 1 : 0, p.getNumSamples(), p.getFileSampleRate() > 0 ? p.getNumSamples() / p.getFileSampleRate() : 0.0,
                      p.getFileSampleRate(), (double) p.getTrimStart(), (double) p.getTrimEnd(), (int) p.getSliceMode(), (double) p.getBpm(),
                      p.getBpmOctave(), (double) param ("gain"), (double) param ("pitch"), (double) param ("speed"), (double) param ("fadeInPct"),
                      (double) param ("fadeOutPct"), (double) param ("fadeShape"), p.getSourceUrl().toRawUTF8(), p.getRestoreNote().toRawUTF8(),
                      p.getLoadedFile().getFullPathName().toRawUTF8(), (int) p.getEmbeddedAudioBytes());
    }

    void setParam (SampleGrabberProcessor& p, const char* id, float v)
    {
        if (auto* prm = p.apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    }

    bool requireScratch()
    {
        if (juce::SystemStats::getEnvironmentVariable ("SG_CACHE_DIR", {}).trim().isEmpty())
        {
            std::fprintf (stderr, "[SG-TEST] set SG_CACHE_DIR (and SG_EXPORT_DIR) to scratch folders first\n");
            return false;
        }
        return true;
    }

    // SG_STATE_SELFTEST=<wav> [SG_SELFTEST_TMP=<scratch dir>]: copy the wav to a temp file, load it, set trim/params,
    // save state, restore with the source present, delete the temp copy, restore again (source missing -> embedded FLAC).
    int runState (const juce::File& src)
    {
        if (! requireScratch() || ! src.existsAsFile())
            return 2;

        const auto tmpEnv = juce::SystemStats::getEnvironmentVariable ("SG_SELFTEST_TMP", {}).trim();
        const auto tmpDir = tmpEnv.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (tmpEnv)
                                                : juce::File::getSpecialLocation (juce::File::tempDirectory);
        tmpDir.createDirectory();
        const auto copy = tmpDir.getNonexistentChildFile ("sg_state_selftest", ".wav");
        if (! src.copyFileTo (copy))
            return 2;

        int failures = 0;
        auto expect = [&] (bool ok, const char* what) { if (! ok) { ++failures; std::fprintf (stderr, "[SG-STATE] FAIL: %s\n", what); } };

        juce::MemoryBlock st;
        int selLen = 0;
        {
            SampleGrabberProcessor a;
            expect (a.loadSample (copy, true, "https://example.com/watch?v=selftest"), "load source");
            a.setTrim (0.25f, 0.5f);
            a.setSliceMode (SampleGrabberProcessor::SliceMode::beat);
            a.analyse();
            if (a.canShiftBpmOctave (+1))
                a.shiftBpmOctave (+1);
            setParam (a, "gain", -3.0f);
            setParam (a, "fadeInPct", 5.0f);
            setParam (a, "fadeShape", 40.0f);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            expect (a.waitForBackgroundIdle (30000), "worker idle (A)");
            std::fprintf (stderr, "[SG-STATE] worker idle after %.0f ms\n", juce::Time::getMillisecondCounterHiRes() - t0);
            printState ("A original", a);
            const int n = a.getNumSamples();
            selLen = (int) std::lround (0.5 * n) - (int) std::lround (0.25 * n);
            a.getStateInformation (st);
            std::fprintf (stderr, "[SG-STATE] state size %d bytes, embedded %d bytes, selection %d samples\n",
                          (int) st.getSize(), (int) a.getEmbeddedAudioBytes(), selLen);
            expect (a.getEmbeddedAudioBytes() > 0, "FLAC cached before save");
        }

        {
            SampleGrabberProcessor b;
            b.setStateInformation (st.getData(), (int) st.getSize());
            printState ("B source present", b);
            expect (b.getLoadedFile() == copy && std::abs (b.getTrimStart() - 0.25f) < 1.0e-4f, "B loads source + trim");
            expect (b.getRestoreNote().isEmpty(), "B no note");
            expect (b.getEmbeddedAudioBytes() > 0, "B seeded FLAC from state");
        }

        copy.deleteFile();
        std::fprintf (stderr, "[SG-STATE] deleted temp source %s\n", copy.getFullPathName().toRawUTF8());

        juce::MemoryBlock st2;
        {
            SampleGrabberProcessor c;
            c.setStateInformation (st.getData(), (int) st.getSize());
            printState ("C source missing", c);
            expect (c.hasSample() && c.getNumSamples() == selLen, "C embedded length = selection length");
            expect (c.getTrimStart() < 1.0e-6f && c.getTrimEnd() > 1.0f - 1.0e-6f, "C trim = whole");
            expect (c.getRestoreNote() == "SOURCE MISSING - USING SAVED SELECTION", "C note");
            expect (c.getSourceUrl() == "https://example.com/watch?v=selftest", "C url kept");
            expect (c.getSliceMode() == SampleGrabberProcessor::SliceMode::beat, "C slice mode");
            // Narrow the selection inside the embedded audio, save again: trims must come out in original coordinates
            c.setTrim (0.5f, 1.0f);
            expect (c.waitForBackgroundIdle (30000), "worker idle (C)");
            c.getStateInformation (st2);
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (st2.getData(), (int) st2.getSize()))
                std::fprintf (stderr, "[SG-STATE] C re-saved: samplePath='%s' trim=%s..%s emb=%s..%s state %d bytes\n",
                              xml->getStringAttribute ("samplePath").toRawUTF8(), xml->getStringAttribute ("trimStart").toRawUTF8(),
                              xml->getStringAttribute ("trimEnd").toRawUTF8(), xml->getStringAttribute ("embTrimStart").toRawUTF8(),
                              xml->getStringAttribute ("embTrimEnd").toRawUTF8(), (int) st2.getSize());
        }

        {
            SampleGrabberProcessor d;
            d.setStateInformation (st2.getData(), (int) st2.getSize());
            printState ("D from C's state", d);
            expect (std::abs (d.getNumSamples() - selLen / 2) <= 2, "D = second half of the selection");
        }

        {
            // XML part only (no FLAC chunk): as before, nothing loaded, but the reference survives
            const auto strLen = (int) juce::ByteOrder::littleEndianInt (juce::addBytesToPointer (st.getData(), 4));
            SampleGrabberProcessor e;
            e.setStateInformation (st.getData(), 8 + strLen + 1);
            printState ("E no embedded audio", e);
            expect (! e.hasSample() && e.getRestoreNote() == "SOURCE MISSING - NO SAVED AUDIO", "E note");
            juce::MemoryBlock st3;
            e.getStateInformation (st3);
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (st3.getData(), (int) st3.getSize()))
                expect (xml->getStringAttribute ("samplePath") == copy.getFullPathName(), "E keeps samplePath on re-save");
        }

        // ---- damaged / hostile states: must never crash, never load garbage lengths ----
        const auto xmlLen = (size_t) juce::ByteOrder::littleEndianInt (juce::addBytesToPointer (st.getData(), 4));
        const size_t chunkPos = 8 + xmlLen + 1;   // "SGA1", int64 size, FLAC bytes
        auto restore = [&] (const char* tag, const juce::MemoryBlock& blk) -> std::unique_ptr<SampleGrabberProcessor>
        {
            auto p = std::make_unique<SampleGrabberProcessor>();
            p->setStateInformation (blk.getData(), (int) blk.getSize());
            printState (tag, *p);
            return p;
        };
        // Same chunk, XML attributes replaced
        auto withXml = [&] (const std::function<void (juce::XmlElement&)>& edit)
        {
            auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
            edit (*xml);
            juce::MemoryBlock out;
            juce::AudioProcessor::copyXmlToBinary (*xml, out);
            out.append (juce::addBytesToPointer (st.getData(), chunkPos), st.getSize() - chunkPos);
            return out;
        };

        {
            juce::MemoryBlock t (st.getData(), st.getSize() - 1000);   // trailer cut off
            auto p = restore ("F truncated trailer", t);
            expect (! p->hasSample() && p->getRestoreNote() == "SOURCE MISSING - NO SAVED AUDIO", "F truncated chunk ignored");
        }
        {
            juce::MemoryBlock t (st);
            const juce::int64 huge = std::numeric_limits<juce::int64>::max();
            t.copyFrom (&huge, chunkPos + 4, sizeof (huge));
            auto p = restore ("G huge length", t);
            expect (! p->hasSample(), "G huge length ignored");
            const juce::int64 neg = -5;
            t.copyFrom (&neg, chunkPos + 4, sizeof (neg));
            auto q = restore ("G negative length", t);
            expect (! q->hasSample(), "G negative length ignored");
        }
        {
            juce::MemoryBlock t (st);
            auto* b = static_cast<juce::uint8*> (t.getData());
            for (size_t i = chunkPos + 12 + 4000; i < chunkPos + 12 + 60000 && i < t.getSize(); ++i)
                b[i] ^= 0x5a;   // frames damaged, header intact
            auto p = restore ("H corrupt FLAC frames", t);
            expect (p->getNumSamples() == 0 || p->getNumSamples() == selLen, "H no garbage length");
            juce::MemoryBlock u (st);
            u.copyFrom ("XXXX", chunkPos + 12, 4);   // "fLaC" magic destroyed
            auto q = restore ("H corrupt FLAC header", u);
            expect (! q->hasSample() && q->getRestoreNote() == "SOURCE MISSING - NO SAVED AUDIO", "H bad header rejected");
        }
        {
            auto t = withXml ([] (juce::XmlElement& x)
            {
                x.setAttribute ("trimStart", "nan");
                x.setAttribute ("trimEnd", "inf");
                x.setAttribute ("bpm", "nan");
                x.setAttribute ("embTrimStart", "-inf");
                for (auto* c : x.getChildIterator())
                    if (c->getStringAttribute ("id") == "gain")
                        c->setAttribute ("value", "nan");
            });
            auto p = restore ("I NaN values", t);
            expect (std::isfinite (p->getTrimStart()) && std::isfinite (p->getTrimEnd()) && std::isfinite (p->getBpm())
                    && std::isfinite (p->getGainDb()), "I all values finite");
        }
        {
            // Saved trims narrower than the embedded range -> mapped into the FLAC (0.3..0.45 of 0.25..0.5 = 0.2..0.8)
            auto t = withXml ([] (juce::XmlElement& x) { x.setAttribute ("trimStart", 0.3); x.setAttribute ("trimEnd", 0.45); });
            auto p = restore ("L narrower trims", t);
            expect (std::abs (p->getTrimStart() - 0.2f) < 1.0e-3f && std::abs (p->getTrimEnd() - 0.8f) < 1.0e-3f, "L trims mapped into FLAC range");
        }
        {
            // FLAC cap: restore, then change the selection so the worker re-encodes with a 100 KB limit
            SampleGrabberProcessor::setEmbedCapForTest (100 * 1024);
            auto p = restore ("J cap: restored", st);
            const int warnBefore = p->getEmbedWarnVersion();
            p->setTrim (0.0f, 0.9f);
            expect (p->waitForBackgroundIdle (30000), "J idle");
            juce::MemoryBlock out;
            p->getStateInformation (out);
            auto xml = juce::AudioProcessor::getXmlFromBinary (out.getData(), (int) out.getSize());
            const auto xl = (size_t) juce::ByteOrder::littleEndianInt (juce::addBytesToPointer (out.getData(), 4));
            std::fprintf (stderr, "[SG-STATE] J cap: embedded=%d bytes, state %d bytes (xml %d), warn %d -> %d, path='%s' url='%s'\n",
                          (int) p->getEmbeddedAudioBytes(), (int) out.getSize(), (int) (8 + xl + 1), warnBefore, p->getEmbedWarnVersion(),
                          xml->getStringAttribute ("samplePath").toRawUTF8(), xml->getStringAttribute ("sourceUrl").toRawUTF8());
            expect (p->getEmbeddedAudioBytes() == 0 && out.getSize() == 8 + xl + 1, "J nothing embedded over the cap");
            expect (p->getEmbedWarnVersion() == warnBefore + 1, "J warning raised once");
            expect (xml->getStringAttribute ("sourceUrl").isNotEmpty() && xml->getStringAttribute ("samplePath").isNotEmpty(), "J path + url kept");
            SampleGrabberProcessor::setEmbedCapForTest ((size_t) 32 * 1024 * 1024);
        }
        {
            // setStateInformation from a host worker thread: until the message thread applies it, getStateInformation
            // must return the new state, not the old sample
            SampleGrabberProcessor p;
            juce::MemoryBlock before;
            p.getStateInformation (before);
            std::thread host ([&] { p.setStateInformation (st.getData(), (int) st.getSize()); });
            host.join();
            juce::MemoryBlock pendingOut;
            p.getStateInformation (pendingOut);
            auto xml = juce::AudioProcessor::getXmlFromBinary (pendingOut.getData(), (int) pendingOut.getSize());
            std::fprintf (stderr, "[SG-STATE] K pending: hasSample=%d state %d bytes samplePath='%s' trim=%s..%s\n", p.hasSample() ? 1 : 0,
                          (int) pendingOut.getSize(), xml->getStringAttribute ("samplePath").toRawUTF8(),
                          xml->getStringAttribute ("trimStart").toRawUTF8(), xml->getStringAttribute ("trimEnd").toRawUTF8());
            const auto xl = (size_t) juce::ByteOrder::littleEndianInt (juce::addBytesToPointer (pendingOut.getData(), 4));
            expect (! p.hasSample() && xml->getStringAttribute ("samplePath") == copy.getFullPathName()
                    && pendingOut.getSize() - (8 + xl + 1) == st.getSize() - chunkPos, "K pending restore (incl. FLAC) is what gets saved");
        }

        std::fprintf (stderr, "[SG-STATE] %s\n", failures == 0 ? "ALL OK" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }

    // SG_EXPORT_SELFTEST=<wav>: staging + drag fallback + reuse, files go to SG_EXPORT_DIR
    int runExport (const juce::File& src)
    {
        if (! requireScratch() || juce::SystemStats::getEnvironmentVariable ("SG_EXPORT_DIR", {}).trim().isEmpty() || ! src.existsAsFile())
            return 2;

        int failures = 0;
        auto expect = [&] (bool ok, const char* what) { if (! ok) { ++failures; std::fprintf (stderr, "[SG-EXPORT] FAIL: %s\n", what); } };
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        auto describe = [&] (const char* tag, const juce::File& f, double ms)
        {
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            std::fprintf (stderr, "[SG-EXPORT] %s: %.0f ms, %s, %lld frames, %d bit\n", tag, ms, f.getFileName().toRawUTF8(),
                          r != nullptr ? (long long) r->lengthInSamples : -1LL, r != nullptr ? (int) r->bitsPerSample : 0);
        };
        auto timed = [&] (SampleGrabberProcessor& p, int pad, const char* tag)
        {
            const bool ready = p.isStagedReady (pad);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto f = p.prepareDragFile (pad);
            const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            std::fprintf (stderr, "[SG-EXPORT] %s staged-ready=%d\n", tag, ready ? 1 : 0);
            describe (tag, f, ms);
            return f;
        };

        SampleGrabberProcessor p;
        expect (p.loadSample (src), "load");
        p.setTrim (0.1f, 0.6f);
        expect (p.waitForBackgroundIdle (60000), "idle 1");

        const auto f0 = timed (p, 0, "pad 1 (staged)");
        const auto fs = timed (p, SampleGrabberProcessor::fullRegionPad, "selection (staged)");
        const auto f0b = timed (p, 0, "pad 1 again");
        expect (f0 == f0b && f0.existsAsFile(), "same state -> same file reused");

        setParam (p, "gain", -6.0f);
        const auto f0c = timed (p, 0, "pad 1 after gain change (immediate, fallback)");
        expect (f0c != f0 && f0.existsAsFile(), "gain change -> new name, old file kept");

        expect (p.waitForBackgroundIdle (60000), "idle 2");
        const auto f0d = timed (p, 0, "pad 1 after gain change (staged)");
        expect (f0d == f0c, "fallback and staged produce the same name");

        setParam (p, "pitch", 3.0f);
        expect (p.waitForBackgroundIdle (60000), "idle 3 (pitch render)");
        timed (p, SampleGrabberProcessor::fullRegionPad, "selection pitch +3 (staged)");
        timed (p, 2, "pad 3 pitch +3 (staged)");

        setParam (p, "gain", 0.0f);
        setParam (p, "pitch", 0.0f);
        expect (p.waitForBackgroundIdle (60000), "idle 4");
        const auto f0e = timed (p, 0, "pad 1 back to original settings");
        expect (f0e == f0, "back to the first settings -> first file reused");

        for (const auto& f : sg::storage::exportRoot().findChildFiles (juce::File::findFiles, true))
            std::fprintf (stderr, "[SG-EXPORT] export dir: %s (%lld bytes)\n", f.getFullPathName().toRawUTF8(), (long long) f.getSize());
        for (const auto& f : sg::storage::cacheRoot().getChildFile ("staging").findChildFiles (juce::File::findFiles, true))
            std::fprintf (stderr, "[SG-EXPORT] staging: %s\n", f.getFullPathName().toRawUTF8());

        std::fprintf (stderr, "[SG-EXPORT] %s\n", failures == 0 ? "ALL OK" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }
}

class SampleGrabberApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Sample Grabber"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String&) override
    {
        // Self-test of the tool installation without a window: SG_TOOLS_SELFTEST=1|update|force [SG_TOOLS_DIR=<folder>]
        // (test helpers: SG_TOOLS_ONLY=yt-dlp,deno,ffmpeg, SG_TOOLS_FAKE_BAD_SHA=1|<tool>, SG_TOOLS_HTTP=juce). Exit code 0 = ok.
        // SG_SETTINGS_SELFTEST=1: read settings.xml and write one value back unchanged (tests .bad handling)
        if (juce::SystemStats::getEnvironmentVariable ("SG_SETTINGS_SELFTEST", {}).isNotEmpty())
        {
            const bool autoYt = ToolManager::isAutoUpdateYtDlp();
            std::cerr << "[SG-SETTINGS] file=" << sg::settings::file().getFullPathName().toStdString()
                      << " theme=" << sg::settings::get ("theme", "(default)").toStdString()
                      << " autoUpdateYtDlp=" << (autoYt ? "on" : "off") << std::endl;
            ToolManager::setAutoUpdateYtDlp (autoYt);
            std::cerr << "[SG-SETTINGS] written, theme now=" << sg::settings::get ("theme", "(default)").toStdString() << std::endl;
            quit();
            return;
        }

        if (juce::SystemStats::getEnvironmentVariable ("SG_CACHE_SELFTEST", {}).isNotEmpty())
        {
            setApplicationReturnValue (CacheManager::runSelfTest());
            quit();
            return;
        }

        const auto stateTest = juce::SystemStats::getEnvironmentVariable ("SG_STATE_SELFTEST", {});
        if (stateTest.isNotEmpty())
        {
            setApplicationReturnValue (selftest::runState (juce::File::getCurrentWorkingDirectory().getChildFile (stateTest)));
            quit();
            return;
        }

        const auto exportTest = juce::SystemStats::getEnvironmentVariable ("SG_EXPORT_SELFTEST", {});
        if (exportTest.isNotEmpty())
        {
            setApplicationReturnValue (selftest::runExport (juce::File::getCurrentWorkingDirectory().getChildFile (exportTest)));
            quit();
            return;
        }

        const auto toolsTest = juce::SystemStats::getEnvironmentVariable ("SG_TOOLS_SELFTEST", {});
        if (toolsTest.isNotEmpty())
        {
            setApplicationReturnValue (ToolManager::runSelfTest (toolsTest));
            quit();
            return;
        }

        ui::loadThemeSetting();   // before the window, so frame and LookAndFeel start in the right theme right away

        // Snapshot mode for developers: SG_SNAPSHOT=<ordner> [SG_THEME=dark|classic] [SG_SNAPSHOT_FILE=<audio>]
        const auto snapDir = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT", {});
        const auto snapTheme = juce::SystemStats::getEnvironmentVariable ("SG_THEME", {});
        if (snapDir.isNotEmpty() && snapTheme.isNotEmpty())
            ui::setThemeTransient (snapTheme == "classic" ? ui::ThemeId::classic : ui::ThemeId::dark);

        window = std::make_unique<MainWindow>();

        if (snapDir.isNotEmpty())
            window->runSnapshot (juce::File (snapDir),
                                 juce::File (juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_FILE", {})));
    }
    void shutdown() override                            { window.reset(); }
    void systemRequestedQuit() override                 { quit(); }

private:
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (SampleGrabberApp)
