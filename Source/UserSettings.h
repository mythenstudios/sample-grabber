// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <juce_data_structures/juce_data_structures.h>
#include <iostream>

// User settings (settings.xml in the user data folder, e.g. theme, yt-dlp auto-update).
// Single place for path and options; both writers (ui::setTheme, ToolManager) go through here.
// Cross-process via InterProcessLock (VST3 and standalone at the same time). Message thread.
namespace sg::settings
{
    inline juce::File file()
    {
        // Same folder as Downloader::dataDir and audio.xml of the standalone app
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("SampleGrabber").getChildFile ("settings.xml");
    }

    inline juce::PropertiesFile::Options options()
    {
        static juce::InterProcessLock lock ("SampleGrabber.settings");
        juce::PropertiesFile::Options o;
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        o.millisecondsBeforeSaving = -1;   // save explicitly only
        o.processLock = &lock;
        return o;
    }

    inline juce::String get (const juce::String& key, const juce::String& fallback)
    {
        const auto f = file();
        if (! f.existsAsFile())
            return fallback;
        juce::PropertiesFile props (f, options());
        if (! props.isValidFile())
            return fallback;   // only set a broken file aside when writing
        return props.getValue (key, fallback);
    }

    // false = not written (unreadable file could not be set aside, or saving failed)
    inline bool set (const juce::String& key, const juce::String& value)
    {
        const auto f = file();
        f.getParentDirectory().createDirectory();

        bool valid = true;
        {
            juce::PropertiesFile probe (f, options());
            valid = ! f.existsAsFile() || probe.isValidFile();
        }

        if (! valid)
        {
            // Unreadable: do not overwrite with a single key, set it aside instead (keep one .bad)
            const auto bad = f.getSiblingFile ("settings.xml.bad");
            bad.deleteFile();
            const bool moved = f.moveFileTo (bad);
            std::cerr << "[SG] settings.xml unreadable -> "
                      << (moved ? bad.getFullPathName() : juce::String ("(rename failed, not written)")).toStdString()
                      << std::endl;
            if (! moved)
                return false;
        }

        juce::PropertiesFile props (f, options());
        props.setValue (key, value);
        return props.save();
    }

    // Removes a key (back to the caller's default). Missing file or key = nothing to do.
    inline bool remove (const juce::String& key)
    {
        const auto f = file();
        if (! f.existsAsFile())
            return true;
        juce::PropertiesFile props (f, options());
        if (! props.isValidFile())
            return false;   // do not touch an unreadable file here; set() sets it aside
        if (! props.containsKey (key))
            return true;
        props.removeValue (key);
        return props.save();
    }
}
