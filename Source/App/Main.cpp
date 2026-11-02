// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "ui/SGLookAndFeel.h"
#include "AudioSettingsPanel.h"
#include "Downloader.h"
#include "TimeRange.h"
#include <cmath>
#include <cstdlib>
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

        // Taskbar/dock icon (X11 _NET_WM_ICON). DocumentWindow::setIcon would only draw into the custom title bar.
        if (auto* peer = getPeer())
            peer->setIcon (juce::ImageCache::getFromMemory (BinaryData::icon256_png, BinaryData::icon256_pngSize));
    }

    ~MainWindow() override
    {
        // Snapshot runs (developer aid) never overwrite the user's audio setup
        if (auto xml = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT", {}).isEmpty() ? deviceManager.createStateXml() : nullptr)
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
        const auto shownTitle = juce::String::fromUTF8 ("Selftest - Ausl\xc3\xa4nder");   // non-ASCII display title round trip
        {
            SampleGrabberProcessor a;
            expect (a.loadSample (copy, true, "https://example.com/watch?v=selftest", {}, shownTitle), "load source");
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
            expect (b.getDisplayTitle() == shownTitle, "B display title");
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
            expect (c.getDisplayTitle() == shownTitle, "C display title");
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

    // Part of SG_SETTINGS_SELFTEST (not Windows: the registry is left alone there): folder priority env > stored > default
    // for the export folder, plus checkChosenFolder. Writes settings.xml (exportDir) and restores the old value; test
    // folders go to the temp folder (TMPDIR). Exit code 0 = ok.
    static int runFolders()
    {
       #if JUCE_WINDOWS
        std::fprintf (stderr, "[SG-SETTINGS] folders: skipped on Windows (registry)\n");
        return 0;
       #else
        using sg::storage::Folder;
        int failures = 0;
        auto expect = [&] (bool ok, const char* what)
        {
            std::fprintf (stderr, "[SG-SETTINGS] folders: %s %s\n", ok ? "ok  " : "FAIL", what);
            if (! ok)
                ++failures;
        };

        const auto oldStored = sg::storage::storedFolder (Folder::exports);
        const auto oldEnv = juce::SystemStats::getEnvironmentVariable ("SG_EXPORT_DIR", {});
        const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("sg-folders-" + juce::String (juce::Time::currentTimeMillis()));
        const auto stored = scratch.getChildFile ("stored exports");
        const auto envDir = scratch.getChildFile ("env exports");

        ::unsetenv ("SG_EXPORT_DIR");
        expect (sg::storage::setStoredFolder (Folder::exports, {}), "reset stored value");
        expect (sg::storage::exportRoot() == sg::storage::defaultFolder (Folder::exports), "nothing set -> default");
        expect (sg::storage::setStoredFolder (Folder::exports, stored.getFullPathName()), "store a folder");
        expect (sg::storage::storedFolder (Folder::exports) == stored.getFullPathName(), "stored value reads back");
        expect (sg::storage::exportRoot() == stored && stored.isDirectory(), "stored -> used (and created)");
        ::setenv ("SG_EXPORT_DIR", envDir.getFullPathName().toRawUTF8(), 1);
        expect (sg::storage::folderFromEnv (Folder::exports) && sg::storage::exportRoot() == envDir, "env wins over stored");
        ::unsetenv ("SG_EXPORT_DIR");
        sg::settings::set ("exportDir", "relative/path");
        expect (sg::storage::exportRoot() == sg::storage::defaultFolder (Folder::exports), "relative stored -> default");
        sg::settings::set ("exportDir", "   ");
        expect (sg::storage::exportRoot() == sg::storage::defaultFolder (Folder::exports), "blank stored -> default");
        sg::settings::set ("exportDir", "/proc/sg-cannot-create/x");
        expect (sg::storage::exportRoot() == sg::storage::defaultFolder (Folder::exports), "uncreatable stored -> default");

        juce::File chosen;
        expect (sg::storage::checkChosenFolder (Folder::exports, "rel/dir", chosen).failed(), "check: relative refused");
        expect (sg::storage::checkChosenFolder (Folder::exports, "/", chosen).failed(), "check: root refused");
        expect (sg::storage::checkChosenFolder (Folder::cache, juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                                   .getFullPathName(), chosen).failed(), "check: home refused as cache");
        expect (sg::storage::checkChosenFolder (Folder::exports, sg::storage::cacheRoot().getChildFile ("x").getFullPathName(),
                                                chosen).failed(), "check: exports inside the cache refused");
        const auto foreign = scratch.getChildFile ("foreign");
        foreign.getChildFile ("song.wav").create();
        expect (sg::storage::checkChosenFolder (Folder::cache, "\"" + foreign.getFullPathName() + "\"", chosen).wasOk()
                    && chosen == foreign.getChildFile ("Sample Grabber Cache") && chosen.isDirectory(),
                "check: foreign folder as cache -> own subfolder");
        const auto onlyDl = scratch.getChildFile ("only downloads");
        onlyDl.getChildFile ("downloads").getChildFile ("myfile").create();
        expect (sg::storage::checkChosenFolder (Folder::cache, onlyDl.getFullPathName(), chosen).wasOk()
                    && chosen == onlyDl.getChildFile ("Sample Grabber Cache"),
                "check: folder with only downloads/myfile as cache -> own subfolder");
        const auto fresh = scratch.getChildFile ("new cache");
        expect (sg::storage::checkChosenFolder (Folder::cache, fresh.getFullPathName(), chosen).wasOk() && chosen == fresh
                    && fresh.isDirectory(), "check: new folder as cache -> used as is");

        // Stored cache folder (installer writes it unchecked): same rules as CHANGE
        {
            const auto oldCacheStored = sg::storage::storedFolder (Folder::cache);
            const auto oldCacheEnv = juce::SystemStats::getEnvironmentVariable ("SG_CACHE_DIR", {});
            ::unsetenv ("SG_CACHE_DIR");
            sg::settings::set ("cacheDir", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName());
            expect (sg::storage::resolveFolder (Folder::cache) == sg::storage::defaultFolder (Folder::cache), "stored cache = home -> default");
            sg::settings::set ("cacheDir", onlyDl.getFullPathName());
            expect (sg::storage::resolveFolder (Folder::cache) == onlyDl.getChildFile ("Sample Grabber Cache")
                        && ! sg::storage::hasCacheMarker (onlyDl), "stored cache = foreign folder -> own subfolder");
            const auto storedFresh = scratch.getChildFile ("stored new cache");
            sg::settings::set ("cacheDir", storedFresh.getFullPathName());
            expect (sg::storage::resolveFolder (Folder::cache) == storedFresh, "stored cache = new folder -> used as is");
            sg::storage::setStoredFolder (Folder::cache, oldCacheStored);
            if (oldCacheEnv.isNotEmpty())
                ::setenv ("SG_CACHE_DIR", oldCacheEnv.toRawUTF8(), 1);
        }

        // Restore
        sg::storage::setStoredFolder (Folder::exports, oldStored);
        if (oldEnv.isNotEmpty())
            ::setenv ("SG_EXPORT_DIR", oldEnv.toRawUTF8(), 1);
        scratch.deleteRecursively();
        std::fprintf (stderr, "[SG-SETTINGS] folders: %s\n", failures == 0 ? "ALL OK" : "FAILURES");
        return failures == 0 ? 0 : 1;
       #endif
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

namespace selftest
{
    // SG_RANGE_SELFTEST=1: range parsing/formatting/guard, link start times, range tags in cache names (paths only)
    int runRange()
    {
        int failures = sg::range::runSelfTest();
        auto expect = [&] (bool ok, const juce::String& what)
        {
            std::fprintf (stderr, "[SG-RANGE] %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
            if (! ok)
                ++failures;
        };

        const auto dl = sg::storage::cacheRoot().getChildFile ("downloads");
        const auto im = sg::storage::cacheRoot().getChildFile ("imports");
        struct N { juce::File f; const char* title; const char* range; };
        const N names[] = { { dl.getChildFile ("Big_Buck_Bunny_aqz-KE-bpKQ_r60-80.wav"), "Big_Buck_Bunny", "1:00-1:20" },
                            { dl.getChildFile ("Big_Buck_Bunny_aqz-KE-bpKQ.wav"), "Big_Buck_Bunny", "" },
                            { dl.getChildFile ("Talk_abcdefghijk_r3720p5-end.wav"), "Talk", "1:02:00.5-" },
                            { im.getChildFile ("justice_mid_r10-25.wav"), "justice_mid", "0:10-0:25" },
                            { juce::File ("/home/x/My_Song_r10-25.wav"), "My_Song_r10-25", "" } };   // user file: name stays
        for (const auto& n : names)
        {
            sg::range::Range r;
            sg::storage::stripRangeTag (n.f, &r);
            const auto title = sg::storage::titleFromFile (n.f);
            expect (title == n.title && sg::range::toText (r) == n.range,
                    n.f.getFileName() + " -> title '" + title + "' range '" + sg::range::toText (r) + "'");
        }

        std::fprintf (stderr, "[SG-RANGE] %s (%d failures total)\n", failures == 0 ? "ALL OK" : "FAILURES", failures);
        return failures == 0 ? 0 : 1;
    }

    // SG_GRAB_SELFTEST=<url|file> [SG_GRAB_RANGE=<from-to>] [SG_GRAB_PROBE_ONLY=1], needs SG_CACHE_DIR (scratch).
    // probe -> guard verdict -> (range) grab/import -> length check -> load + trim + state -> delete the cache file
    // (as KEEP DOWNLOADS OFF does) -> restore (source missing) -> grab again -> saved trim restored? -> delete again.
    class GrabTest
    {
    public:
        explicit GrabTest (std::function<void (int)> finishedIn) : finished (std::move (finishedIn)) {}

        void start (const juce::String& srcText)
        {
            if (! requireScratch())
                return done (2);

            isUrl = srcText.startsWithIgnoreCase ("http://") || srcText.startsWithIgnoreCase ("https://");
            src = isUrl ? srcText : juce::File::getCurrentWorkingDirectory().getChildFile (srcText).getFullPathName();
            const auto p = sg::range::parse (juce::SystemStats::getEnvironmentVariable ("SG_GRAB_RANGE", {}));
            if (! p.ok)
            {
                log ("range invalid: " + p.error);
                return done (2);
            }
            range = p.range;
            rangeText = sg::range::toText (range);
            probeOnly = juce::SystemStats::getEnvironmentVariable ("SG_GRAB_PROBE_ONLY", {}).isNotEmpty();

            t0 = now();
            auto cb = [this] (Downloader::Result r) { onProbe (r); };
            if (isUrl)
                dl.probe (src, cb);
            else
                dl.probeFile (juce::File (src), cb);
        }

    private:
        static double now() { return juce::Time::getMillisecondCounterHiRes(); }
        static void log (const juce::String& s) { std::fprintf (stderr, "[SG-GRAB] %s\n", s.toRawUTF8()); }
        void expect (bool ok, const juce::String& what) { log ((ok ? "ok   " : "FAIL ") + what); if (! ok) ++failures; }
        void done (int code) { if (finished) finished (code); }

        void onProbe (const Downloader::Result& r)
        {
            log ("probe " + juce::String (now() - t0, 0) + " ms: ok=" + juce::String ((int) r.ok) + " duration="
                 + (r.duration >= 0.0 ? juce::String (r.duration, 3) + " s (" + sg::range::formatTime (r.duration) + ")" : juce::String ("unknown"))
                 + " title='" + r.title + "' msg='" + r.message + "'");
            if (! r.ok)
                return done (1);
            duration = r.duration;

            const auto v = sg::range::guard (range, duration, isUrl);
            const char* kinds[] = { "GRAB", "CONFIRM (GRAB ALL)", "BLOCKED" };
            log ("guard for range '" + rangeText + "': " + kinds[(int) v.kind] + (v.status.isNotEmpty() ? " - " + v.status : juce::String()));
            if (isUrl && range.set)
                log (juce::String ("strategy: ") + (Downloader::cutsLocally (range, duration) ? "whole audio + local cut" : "--download-sections"));
            if (probeOnly)
                return done (failures == 0 ? 0 : 1);
            if (v.kind != sg::range::Verdict::Kind::grab)
            {
                log ("not downloading (guard)");
                return done (failures == 0 ? 0 : 1);
            }
            fetch ([this] (Downloader::Result res) { onFirst (res); });

            // SG_GRAB_CANCEL_AFTER=<ms>: cancel the running fetch, then no process of the job and no partial file may remain
            const int cancelMs = juce::SystemStats::getEnvironmentVariable ("SG_GRAB_CANCEL_AFTER", {}).getIntValue();
            if (cancelMs > 0)
                juce::Timer::callAfterDelay (cancelMs, [this] { cancelCheck(); });
        }

        void cancelCheck()
        {
            const auto t = now();
            dl.cancel();
            log ("cancel took " + juce::String (now() - t, 0) + " ms, busy=" + juce::String ((int) dl.isBusy()));
            juce::Thread::sleep (500);
            int survivors = 0;
           #if JUCE_LINUX
            const auto marker = sg::storage::cacheRoot().getFullPathName();
            for (const auto& d : juce::File ("/proc").findChildFiles (juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::no))
                if (d.getFileName().containsOnly ("0123456789"))
                {
                    juce::MemoryBlock mb;
                    juce::FileInputStream in (d.getChildFile ("cmdline"));
                    if (in.openedOk())
                        in.readIntoMemoryBlock (mb);
                    auto* bytes = static_cast<char*> (mb.getData());
                    for (size_t i = 0; i < mb.getSize(); ++i)
                        if (bytes[i] == 0)
                            bytes[i] = ' ';   // argv separators
                    const auto cmd = juce::String::fromUTF8 (bytes, (int) mb.getSize());
                    if (cmd.contains (marker))
                    {
                        ++survivors;
                        log ("still running: " + d.getFileName() + " " + cmd.substring (0, 100));
                    }
                }
           #elif JUCE_WINDOWS
            // No other process' command line without reading its PEB: match by image instead (yt-dlp.exe, its
            // PyInstaller child of the same name, ffmpeg/ffprobe, deno) in our tools folder or wherever the tools resolve
            juce::Array<juce::File> dirs { ToolManager::toolsDir() };
            {
                juce::SharedResourcePointer<ToolManager> tm;
                for (const auto t : { ToolManager::Tool::ytdlp, ToolManager::Tool::ffmpeg, ToolManager::Tool::deno })
                {
                    const auto exe = tm->getExecutable (t);
                    if (exe != juce::File())
                        dirs.addIfNotAlreadyThere (exe.getParentDirectory());
                }
            }
            for (const auto& p : sg::winproc::findProcessesIn (dirs, { "yt-dlp", "ffmpeg", "ffprobe", "deno" }))
            {
                ++survivors;
                log ("still running: " + p);
            }
           #endif
            const auto left = sg::storage::cacheRoot().getChildFile ("downloads").findChildFiles (juce::File::findFiles, false);
            for (const auto& f : left)
                log ("left over: " + f.getFileName());
            expect (survivors == 0, "no process of the job survives");
            expect (left.isEmpty(), "no partial files");
            log (failures == 0 ? "ALL OK" : "FAILURES");
            done (failures == 0 ? 0 : 1);
        }

        void fetch (std::function<void (Downloader::Result)> then)
        {
            t0 = now();
            auto prog = [] (float, juce::String) {};
            if (isUrl)
            {
                dl.grab (src, range, duration, false, prog, std::move (then));
                return;
            }
            int bits = 16;
            {
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                if (std::unique_ptr<juce::AudioFormatReader> rd { fm.createReaderFor (juce::File (src)) })
                    bits = (int) rd->bitsPerSample;
            }
            dl.importFile (juce::File (src), range, bits, prog, std::move (then));
        }

        int channels = 0;
        double fileSeconds (const juce::File& f, double& rate, int& bits)
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
            if (rd == nullptr)
                return -1.0;
            rate = rd->sampleRate;
            bits = (int) rd->bitsPerSample;

            // Channels and RMS per channel (downmix check: a 5.1 centre must end up in both channels)
            const int n = (int) juce::jmin<juce::int64> (rd->lengthInSamples, (juce::int64) (rd->sampleRate * 600.0));
            juce::AudioBuffer<float> buf ((int) rd->numChannels, n);
            rd->read (&buf, 0, n, 0, true, true);
            juce::String rms;
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                rms << " ch" << ch << " " << juce::String (juce::Decibels::gainToDecibels (buf.getRMSLevel (ch, 0, n), -120.0f), 1) << " dB";
            log ("channels " + juce::String ((int) rd->numChannels) + ", RMS" + rms);
            channels = (int) rd->numChannels;
            return (double) rd->lengthInSamples / rd->sampleRate;
        }

        void onFirst (const Downloader::Result& r)
        {
            log ("fetch " + juce::String ((now() - t0) / 1000.0, 1) + " s: ok=" + juce::String ((int) r.ok) + " file='"
                 + r.file.getFullPathName() + "' msg='" + r.message + "'");
            expect (r.ok && r.file.existsAsFile(), "result file exists");
            if (! r.ok)
                return done (1);

            double rate = 0.0;
            int bits = 0;
            const double got = fileSeconds (r.file, rate, bits);
            const double want = sg::range::coveredSeconds (range, duration);
            log ("length: expected " + juce::String (want, 3) + " s, actual " + juce::String (got, 3) + " s, diff "
                 + juce::String ((got - want) * 1000.0, 1) + " ms (" + juce::String (rate, 0) + " Hz, " + juce::String (bits) + " bit, "
                 + sg::storage::formatBytes (r.file.getSize()) + ")");
            // Whole source: site metadata rounds the duration (YouTube: 635 s for 634.6 s), ranges must be exact
            const double tol = range.set ? 0.25 : 1.5;
            expect (want < 0.0 || std::abs (got - want) < tol, "length within " + juce::String (tol, 2) + " s");
            expect (channels >= 1 && channels <= 2, "at most stereo");

            sg::range::Range tag;
            sg::storage::stripRangeTag (r.file, &tag);
            log ("title '" + sg::storage::titleFromFile (r.file) + "', range from name '" + sg::range::toText (tag) + "'");
            expect (tag == range, "range tag in the file name");

            const auto url = isUrl ? src : src;   // local: the original path is the source (as the editor does)
            juce::MemoryBlock st;
            {
                SampleGrabberProcessor a;
                expect (a.loadSample (r.file, true, url, rangeText), "load result");
                a.setTrim (0.2f, 0.6f);
                expect (a.waitForBackgroundIdle (60000), "worker idle (A)");
                a.getStateInformation (st);
                printState ("A loaded", a);
                expect (a.getSourceRange() == rangeText, "A range kept");
            }

            // KEEP DOWNLOADS OFF: the cache file goes away after loading
            // (the editor does this once the FLAC is settled; here A has waited for the worker already)
            expect (sg::storage::deleteCacheSource (r.file) && ! r.file.existsAsFile(), "cache file deleted (keep off)");
            {
                SampleGrabberProcessor a2;   // the saved state must carry sourceKept=false: mark and save again
                a2.setStateInformation (st.getData(), (int) st.getSize());
                a2.noteSourceNotKept (r.file);
                expect (a2.waitForBackgroundIdle (60000), "worker idle (A2)");
                st.reset();
                a2.getStateInformation (st);
            }

            b = std::make_unique<SampleGrabberProcessor>();
            b->setStateInformation (st.getData(), (int) st.getSize());
            printState ("B source missing", *b);
            expect (b->getRestoreNote().startsWith ("SAVED SELECTION LOADED"), "B note (not kept on purpose)");
            expect (b->getSourceUrl() == url && b->getSourceRange() == rangeText, "B link + range restored");

            fetch ([this] (Downloader::Result res) { onRegrab (res); });
        }

        void onRegrab (const Downloader::Result& r)
        {
            log ("re-grab " + juce::String ((now() - t0) / 1000.0, 1) + " s: ok=" + juce::String ((int) r.ok) + " file='" + r.file.getFullPathName() + "'");
            expect (r.ok, "re-grab ok");
            if (r.ok)
            {
                expect (b->loadSample (r.file, true, src, rangeText), "B loads re-grab");
                printState ("B after re-grab", *b);
                expect (std::abs (b->getTrimStart() - 0.2f) < 1.0e-4f && std::abs (b->getTrimEnd() - 0.6f) < 1.0e-4f, "saved trim restored");
                r.file.deleteFile();
            }
            b.reset();
            log (failures == 0 ? "ALL OK" : "FAILURES");
            done (failures == 0 ? 0 : 1);
        }

        Downloader dl;
        std::unique_ptr<SampleGrabberProcessor> b;
        std::function<void (int)> finished;
        juce::String src, rangeText;
        sg::range::Range range;
        bool isUrl = false, probeOnly = false;
        double duration = -1.0, t0 = 0.0;
        int failures = 0;
    };
}

namespace selftest
{
    // SG_PROC_SELFTEST=1 (Windows only, needs SG_CACHE_DIR): argv quoting round trip through CommandLineToArgvW, and
    // that kill() and destroying a running WinProcess leave no process of its tree behind. The tree is
    // sgproc-cmd.exe -> sgproc-cmd2.exe -> ping.exe (copies of cmd.exe with unique names in a scratch folder, so
    // findProcessesIn sees exactly our processes).
    int runProc()
    {
       #if ! JUCE_WINDOWS
        std::fprintf (stderr, "[SG-PROC] skipped (Windows only)\n");
        return 0;
       #else
        if (! requireScratch())
            return 1;

        int failures = 0;
        auto log = [] (const juce::String& s) { std::fprintf (stderr, "[SG-PROC] %s\n", s.toRawUTF8()); };
        auto expect = [&] (bool ok, const juce::String& what)
        {
            log ((ok ? "ok   " : "FAIL ") + what);
            if (! ok)
                ++failures;
        };
        auto now = [] { return juce::Time::getMillisecondCounterHiRes(); };

        // a) Quoting: every case alone and all together, argv[0] = an exe path (own parsing rules)
        {
            const juce::StringArray cases { "a b", "a\"b", "C:\\dir with space\\", "", "\t", "\\\\\"", "trailing\\\\",
                                            "x", "a\\b", "\\\"", "a\\\\b c", "\"", "tab\tin", "new\nline" };
            auto visible = [] (const juce::String& s)
            {
                return "[" + s.replace ("\t", "<TAB>").replace ("\n", "<LF>") + "]";
            };
            auto roundTrip = [&] (const juce::StringArray& args, const juce::String& what)
            {
                const auto cmd = sg::WinProcess::quoteCommandLine (args);
                const auto back = sg::winproc::parseCommandLine (cmd);
                const bool ok = back == args;
                expect (ok, "quote " + what + " -> " + visible (cmd));
                if (! ok)
                    for (int i = 0; i < juce::jmax (args.size(), back.size()); ++i)
                        log ("      arg " + juce::String (i) + ": in " + visible (args[i]) + " out " + visible (back[i]));
            };
            for (const auto* exe : { "C:\\sg\\tool.exe", "C:\\Program Files\\sg tool\\tool.exe" })
            {
                for (const auto& c : cases)
                    roundTrip (juce::StringArray { juce::String (exe), c }, visible (c));
                juce::StringArray all { juce::String (exe) };
                all.addArray (cases);
                roundTrip (all, "all cases, exe " + visible (exe));
            }
        }

        // Scratch copies of cmd.exe (+ their language resources, best effort)
        const auto sys = juce::File::getSpecialLocation (juce::File::windowsSystemDirectory);
        const auto scratch = sg::storage::cacheRoot().getChildFile ("proc-selftest");
        scratch.deleteRecursively();
        scratch.createDirectory();
        const juce::StringArray names { "sgproc-cmd", "sgproc-cmd2" };
        for (const auto& n : names)
        {
            const auto copy = scratch.getChildFile (n + ".exe");
            expect (sys.getChildFile ("cmd.exe").copyFileTo (copy), "copy cmd.exe -> " + copy.getFileName());
        }
        // Messages of cmd.exe live in <lang>\cmd.exe.mui, looked up by the image name
        for (const auto& lang : sys.findChildFiles (juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::no))
        {
            const auto mui = lang.getChildFile ("cmd.exe.mui");
            if (! mui.existsAsFile())
                continue;
            const auto dest = scratch.getChildFile (lang.getFileName());
            dest.createDirectory();
            for (const auto& n : names)
                mui.copyFileTo (dest.getChildFile (n + ".exe.mui"));
        }
        juce::StringArray treeArgs;
        treeArgs.add (scratch.getChildFile ("sgproc-cmd.exe").getFullPathName());
        treeArgs.add ("/c");
        treeArgs.add (scratch.getChildFile ("sgproc-cmd2.exe").getFullPathName());
        treeArgs.add ("/c");
        treeArgs.add (sys.getChildFile ("ping.exe").getFullPathName());
        treeArgs.addArray (juce::StringArray { "-n", "60", "127.0.0.1" });
        const juce::Array<juce::File> dirs { scratch };
        auto running = [&] { return sg::winproc::findProcessesIn (dirs, names); };
        auto waitUntil = [&] (std::function<bool()> cond, double ms)
        {
            const auto end = now() + ms;
            for (;;)
            {
                if (cond())
                    return true;
                if (now() >= end)
                    return false;
                juce::Thread::sleep (20);
            }
        };
        auto startTree = [&] (sg::WinProcess& p, const char* tag)
        {
            const bool started = p.start (treeArgs, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr);
            expect (started, juce::String (tag) + ": tree started");
            const bool up = started && waitUntil ([&] { return running().size() == names.size(); }, 5000);
            for (const auto& r : running())
                log ("      running: " + r);
            expect (up, juce::String (tag) + ": both cmd copies run");
            return up;
        };

        // b) + d) kill() ends the whole tree before it returns, reading to EOF afterwards does not hang
        {
            auto p = std::make_unique<sg::WinProcess>();
            if (startTree (*p, "kill"))
            {
                const auto t = now();
                p->kill();
                const auto killMs = now() - t;
                const auto left = running();
                for (const auto& r : left)
                    log ("      still running: " + r);
                expect (left.isEmpty(), "kill: nothing left right after kill() (" + juce::String (killMs, 0) + " ms)");
                expect (p->getExitCode() == 1, "kill: exit code 1 (got " + juce::String ((juce::int64) p->getExitCode()) + ")");

                const auto t2 = now();
                char buf[512];
                int total = 0;
                for (;;)
                {
                    const int n = p->readProcessOutput (buf, (int) sizeof (buf));
                    if (n <= 0)
                        break;
                    total += n;
                }
                const auto readMs = now() - t2;
                expect (readMs < 1000.0, "kill: read to EOF after kill took " + juce::String (readMs, 0) + " ms ("
                                             + juce::String (total) + " bytes)");
            }
        }

        // c) Destroying the object without kill() (plugin closed mid-download): KILL_ON_JOB_CLOSE ends the tree
        {
            auto p = std::make_unique<sg::WinProcess>();
            if (startTree (*p, "destroy"))
            {
                const auto t = now();
                p.reset();
                const bool gone = waitUntil ([&] { return running().isEmpty(); }, 2000);
                for (const auto& r : running())
                    log ("      still running: " + r);
                expect (gone, "destroy: nothing left after " + juce::String (now() - t, 0) + " ms");
            }
        }

        scratch.deleteRecursively();
        log (failures == 0 ? "ALL OK" : "FAILURES");
        return failures == 0 ? 0 : 1;
       #endif
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
        // Self-test of the tool installation without a window: SG_TOOLS_SELFTEST=1|update|force|remove [SG_TOOLS_DIR=<folder>]
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
            setApplicationReturnValue (selftest::runFolders());
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

        if (juce::SystemStats::getEnvironmentVariable ("SG_RANGE_SELFTEST", {}).isNotEmpty())
        {
            setApplicationReturnValue (selftest::runRange());
            quit();
            return;
        }

        if (juce::SystemStats::getEnvironmentVariable ("SG_PROC_SELFTEST", {}).isNotEmpty())
        {
            setApplicationReturnValue (selftest::runProc());
            quit();
            return;
        }

        const auto grabTest = juce::SystemStats::getEnvironmentVariable ("SG_GRAB_SELFTEST", {});
        if (grabTest.isNotEmpty())
        {
            grabTester = std::make_unique<selftest::GrabTest> ([this] (int code)
            {
                setApplicationReturnValue (code);
                juce::MessageManager::callAsync ([this] { grabTester.reset(); quit(); });
            });
            grabTester->start (grabTest);
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
    std::unique_ptr<selftest::GrabTest> grabTester;
};

START_JUCE_APPLICATION (SampleGrabberApp)
