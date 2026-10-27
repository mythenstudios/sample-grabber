#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "ui/SGLookAndFeel.h"
#include "AudioSettingsPanel.h"

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
