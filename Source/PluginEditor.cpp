// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
#include "PluginEditor.h"

namespace
{
    juce::String T (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    const juce::Colour bg        { 0xff16181d };
    const juce::Colour panel     { 0xff1f2228 };
    const juce::Colour line      { 0xff2c3038 };
    const juce::Colour text      { 0xffeceef2 };
    const juce::Colour muted     { 0xff8b909c };
    const juce::Colour accent    { 0xffff7a3d };
    const juce::Colour errColour { 0xffff6b6b };

    void styleButton (juce::TextButton& b, juce::Colour fill, juce::Colour ink)
    {
        b.setColour (juce::TextButton::buttonColourId, fill);
        b.setColour (juce::TextButton::buttonOnColourId, fill);
        b.setColour (juce::TextButton::textColourOffId, ink);
        b.setColour (juce::TextButton::textColourOnId, ink);
        b.setColour (juce::ComboBox::outlineColourId, line);
    }

    juce::String formatKHz (std::uint32_t sr)
    {
        return juce::String (sr / 1000.0, sr % 1000 ? 1 : 0) + " kHz";
    }
}

StudioWireEditor::StudioWireEditor (StudioWireProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    title.setText ("StudioWire", juce::dontSendNotification);
    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, text);
    addAndMakeVisible (title);

    shareButton.onClick = [this] { toggleSharing(); };
    addAndMakeVisible (shareButton);

    tunnelToggle.setButtonText ("Public link (free Cloudflare tunnel)");
    tunnelToggle.setToggleState (proc.isTunnelWanted(), juce::dontSendNotification);
    tunnelToggle.setColour (juce::ToggleButton::textColourId, text);
    tunnelToggle.setColour (juce::ToggleButton::tickColourId, accent);
    tunnelToggle.setColour (juce::ToggleButton::tickDisabledColourId, muted);
    addAndMakeVisible (tunnelToggle);

    qualityLabel.setText ("Stream quality", juce::dontSendNotification);
    qualityLabel.setFont (juce::FontOptions (13.0f));
    qualityLabel.setColour (juce::Label::textColourId, text);
    addAndMakeVisible (qualityLabel);

    qualityBox.setColour (juce::ComboBox::backgroundColourId, panel);
    qualityBox.setColour (juce::ComboBox::textColourId, text);
    qualityBox.setColour (juce::ComboBox::outlineColourId, line);
    qualityBox.setColour (juce::ComboBox::arrowColourId, muted);
    qualityBox.addItemList (proc.getMaxQualityParam().choices, 1); // same order as the parameter
    addAndMakeVisible (qualityBox);
    // Tied to the plugin parameter: the DAW saves it with the project.
    qualityAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (proc.getMaxQualityParam(), qualityBox);

    linkCaption.setText ("Link to send to the listener", juce::dontSendNotification);
    linkCaption.setFont (juce::FontOptions (12.0f));
    linkCaption.setColour (juce::Label::textColourId, muted);
    addAndMakeVisible (linkCaption);

    linkField.setReadOnly (true);
    linkField.setCaretVisible (false);
    linkField.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    linkField.setColour (juce::TextEditor::backgroundColourId, panel);
    linkField.setColour (juce::TextEditor::textColourId, text);
    linkField.setColour (juce::TextEditor::outlineColourId, line);
    linkField.setColour (juce::TextEditor::focusedOutlineColourId, accent);
    addAndMakeVisible (linkField);

    copyButton.setButtonText ("Copy");
    copyButton.onClick = [this]
    {
        const auto link = currentLink();
        if (link.isNotEmpty())
        {
            juce::SystemClipboard::copyTextToClipboard (link);
            copiedAt = juce::Time::getMillisecondCounter();
            refresh();
        }
    };
    addAndMakeVisible (copyButton);

    // cloudflared: version, update / download, and (only when missing) choose by hand.
    cfLabel.setFont (juce::FontOptions (12.0f));
    cfLabel.setColour (juce::Label::textColourId, muted);
    addAndMakeVisible (cfLabel);

    cfButton.onClick = [this]
    {
        auto& cf = proc.getCloudflaredManager();
        if (cf.isUpdateAvailable() || ! proc.getCloudflared().existsAsFile())
            cf.installUpdate();   // checks first if needed
        else
            cf.checkForUpdate();
        refresh();
    };
    addAndMakeVisible (cfButton);

    locateButton.setButtonText (T ("Choose…"));
    locateButton.onClick = [this] { chooseCloudflared(); };
    addChildComponent (locateButton);

    for (auto* b : { &copyButton, &locateButton, &cfButton })
        styleButton (*b, panel, text);

    status.setFont (juce::FontOptions (13.0f));
    status.setColour (juce::Label::textColourId, text);
    status.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (status);

    hint.setFont (juce::FontOptions (12.0f));
    hint.setColour (juce::Label::textColourId, muted);
    hint.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (hint);

    setSize (520, 450);
    lastTime = juce::Time::getMillisecondCounterHiRes();
    refresh();
    startTimerHz (4);
}

StudioWireEditor::~StudioWireEditor()
{
    stopTimer();
}

void StudioWireEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (line);
    g.drawHorizontalLine (separatorY, 20.0f, (float) getWidth() - 20.0f);
}

void StudioWireEditor::resized()
{
    auto r = getLocalBounds().reduced (20, 16);

    title.setBounds (r.removeFromTop (28));
    r.removeFromTop (12);

    shareButton.setBounds (r.removeFromTop (40));
    r.removeFromTop (8);
    tunnelToggle.setBounds (r.removeFromTop (24));
    r.removeFromTop (8);
    auto qualityRow = r.removeFromTop (28);
    qualityLabel.setBounds (qualityRow.removeFromLeft (120));
    qualityBox.setBounds (qualityRow.removeFromLeft (220));
    r.removeFromTop (10);

    linkCaption.setBounds (r.removeFromTop (18));
    auto linkRow = r.removeFromTop (30);
    copyButton.setBounds (linkRow.removeFromRight (80));
    linkRow.removeFromRight (6);
    linkField.setBounds (linkRow);
    r.removeFromTop (14);

    auto cfRow = r.removeFromBottom (28);
    if (locateButton.isVisible())
    {
        locateButton.setBounds (cfRow.removeFromRight (90));
        cfRow.removeFromRight (6);
    }
    cfButton.setBounds (cfRow.removeFromRight (170));
    cfRow.removeFromRight (8);
    cfLabel.setBounds (cfRow);
    r.removeFromBottom (8);

    hint.setBounds (r.removeFromBottom (34));
    r.removeFromBottom (6);
    separatorY = r.getBottom();
    r.removeFromBottom (6);
    status.setBounds (r);
}

void StudioWireEditor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto bytes = proc.getServer().getBytesSent();
    const auto dt = (now - lastTime) / 1000.0;
    if (dt > 0.9)
    {
        bitrate = bytes >= lastBytes ? (double) (bytes - lastBytes) * 8.0 / dt : 0.0;
        lastBytes = bytes;
        lastTime = now;
    }
    refresh();
}

juce::String StudioWireEditor::currentLink() const
{
    if (! proc.isSharing())
        return {};
    if (proc.isTunnelWanted())
        return proc.getPublicLink();
    return proc.getLocalLink();
}

void StudioWireEditor::toggleSharing()
{
    lastError.clear();
    if (proc.isSharing())
    {
        proc.stopSharing();
    }
    else
    {
        juce::String error;
        if (! proc.startSharing (tunnelToggle.getToggleState(), error))
            lastError = error.isNotEmpty() ? error : juce::String ("Could not start sharing.");
        else if (error.isNotEmpty())
            lastError = error;
    }
    refresh();
}

void StudioWireEditor::chooseCloudflared()
{
    chooser = std::make_unique<juce::FileChooser> ("Where is cloudflared?", juce::File ("/opt/homebrew/bin"), "*");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile())
                              {
                                  proc.setCloudflaredOverride (f);
                                  lastError.clear();
                              }
                              refresh();
                          });
}

void StudioWireEditor::refresh()
{
    const bool sharing = proc.isSharing();
    auto& server = proc.getServer();
    auto& tunnel = proc.getTunnel();
    const bool wantTunnel = proc.isTunnelWanted();
    const auto cloudflared = proc.getCloudflared();

    shareButton.setButtonText (sharing ? "Stop sharing" : "Start sharing");
    styleButton (shareButton, sharing ? panel : accent, sharing ? text : juce::Colour (0xff1a0d06));
    tunnelToggle.setEnabled (! sharing);

    const auto link = currentLink();
    if (linkField.getText() != link)
        linkField.setText (link, false);

    copyButton.setEnabled (link.isNotEmpty());

    // --- cloudflared row
    auto& cf = proc.getCloudflaredManager();
    const auto bin = cf.getCurrent();
    const auto activity = cf.getActivity();
    const bool cfBroken = cloudflared.existsAsFile() && bin.file == cloudflared && bin.version.isEmpty()
                          && activity != Cloudflared::Activity::scanning;
    if (! cloudflared.existsAsFile())
        cfLabel.setText ("cloudflared: not found", juce::dontSendNotification);
    else if (cfBroken)
        cfLabel.setText (T ("cloudflared: doesn’t run on this Mac"), juce::dontSendNotification);
    else if (bin.file == cloudflared && bin.version.isNotEmpty())
        cfLabel.setText ("cloudflared " + bin.version + " (" + bin.source + ")"
                             + (cf.isGettingOld() ? T (" · getting old") : juce::String()),
                         juce::dontSendNotification);
    else
        cfLabel.setText ("cloudflared: " + cloudflared.getFullPathName(), juce::dontSendNotification);

    if (activity == Cloudflared::Activity::checking)
        cfButton.setButtonText (T ("Checking…"));
    else if (activity == Cloudflared::Activity::downloading)
        cfButton.setButtonText (T ("Downloading… ") + juce::String (juce::roundToInt (cf.getProgress() * 100.0f)) + "%");
    else if (! cloudflared.existsAsFile())
        cfButton.setButtonText ("Download cloudflared");
    else if (cf.isUpdateAvailable())
        cfButton.setButtonText ("Update to " + cf.getLatestVersion());
    else
        cfButton.setButtonText ("Check for update");
    cfButton.setEnabled (activity != Cloudflared::Activity::checking && activity != Cloudflared::Activity::downloading);

    const bool showLocate = ! cloudflared.existsAsFile() || cfBroken;
    if (locateButton.isVisible() != showLocate)
    {
        locateButton.setVisible (showLocate);
        resized();
        repaint();
    }

    const bool justCopied = copiedAt != 0 && juce::Time::getMillisecondCounter() - copiedAt < 1500;
    copyButton.setButtonText (justCopied ? "Copied!" : "Copy");

    juce::StringArray lines;
    if (! sharing)
    {
        lines.add ("Sharing: off. Nothing can be reached from outside.");
    }
    else
    {
        lines.add ("Sharing: on (local server 127.0.0.1:" + juce::String (server.getPort()) + ")");

        if (! wantTunnel)
            lines.add ("Tunnel: off, the link only works on this Mac");
        else if (! cloudflared.existsAsFile())
            lines.add ("Tunnel: cloudflared not found");
        else
        {
            switch (tunnel.getState())
            {
                case Tunnel::State::starting: lines.add (T ("Tunnel: starting… (5 to 15 s)")); break;
                case Tunnel::State::ready:    lines.add ("Tunnel: ready"); break;
                case Tunnel::State::failed:   lines.add ("Tunnel: error. " + tunnel.getMessage()); break;
                case Tunnel::State::idle:     lines.add ("Tunnel: stopped"); break;
            }
        }

        auto listenersLine = "Listeners: " + juce::String (server.getListenerCount()) + " / "
                           + juce::String (server.getMaxListeners());
        if (server.getListenerCount() > 0)
            listenersLine << " (CD " << server.getListenerCount (studiowire::Quality::cd)
                          << T (" · Hi-Res ") << server.getListenerCount (studiowire::Quality::hires)
                          << T (" · Opus ") << server.getListenerCount (studiowire::Quality::eco) << ")";
        listenersLine << T ("   ·   Sent: ")
                      << (bitrate < 1.0e6 ? juce::String (bitrate / 1.0e3, 0) + " kbps"
                                          : juce::String (bitrate / 1.0e6, 2) + " Mbps");
        lines.add (listenersLine);
        lines.add (server.isReceivingAudio()
                       ? "Signal: receiving, " + formatKHz (server.getSampleRate())
                       : juce::String ("Signal: nothing coming in, press play in your DAW"));

        const auto encErr = server.getEncoderError();
        if (! encErr.empty())
            lines.add ("Encoder: " + juce::String::fromUTF8 (encErr.c_str()));
    }

    if (cf.getMessage().isNotEmpty())
        lines.add (cf.getMessage());

    if (lastError.isNotEmpty())
        lines.add (lastError);

    status.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
    status.setColour (juce::Label::textColourId,
                      lastError.isNotEmpty() || tunnel.getState() == Tunnel::State::failed ? errColour : text);

    // Help line at the bottom.
    if (! cloudflared.existsAsFile() && wantTunnel)
        hint.setText ("The public link needs cloudflared: click Download cloudflared below,\n"
                      "or install it in Terminal with: brew install cloudflared", juce::dontSendNotification);
    else if (cfBroken && wantTunnel)
        hint.setText (T ("This cloudflared doesn’t run on this version of macOS.\n"
                         "Check for update, or Choose… another copy."), juce::dontSendNotification);
    else if (sharing && link.isNotEmpty())
        hint.setText ("Send this link to the listener. It changes every time you start sharing and "
                      "stops working as soon as you stop.", juce::dontSendNotification);
    else
        hint.setText ("Put StudioWire last in the inserts of the master track.", juce::dontSendNotification);
}
