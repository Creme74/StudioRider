// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

// The "three bars" menu button, top right.
class MenuButton : public juce::Button
{
public:
    MenuButton() : juce::Button ("Menu") { setTooltip ("Menu"); }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

class StudioRiderEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit StudioRiderEditor (StudioRiderProcessor&);
    ~StudioRiderEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void toggleSharing();
    void refresh();
    juce::String currentLink() const;
    void chooseCloudflared();
    void showMenu();
    void showAbout();
    void checkForPluginUpdate();
    void showUpdateResult (bool ok, const juce::String& tag, const juce::String& page, int status);

    StudioRiderProcessor& proc;

    juce::Label title, versionLabel, linkCaption, status, hint;
    MenuButton menuButton;
    juce::TextButton shareButton, copyButton, locateButton, cfButton;
    juce::Label cfLabel;
    juce::ToggleButton tunnelToggle;
    juce::Label qualityLabel;
    juce::ComboBox qualityBox;
    std::unique_ptr<juce::ComboBoxParameterAttachment> qualityAttachment;
    juce::TextEditor linkField;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Image logo;
    juce::Rectangle<float> logoArea;
    bool revving = false;
    int revFrames = 0;
    juce::Random rng;

    juce::String lastError;
    int separatorY = 0;
    std::uint64_t lastBytes = 0;
    double lastTime = 0, bitrate = 0;
    juce::uint32 copiedAt = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StudioRiderEditor)
};
