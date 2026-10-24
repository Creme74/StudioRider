// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Cloudflared.h"
#include "StreamServer.h"
#include "Tunnel.h"

class StudioWireProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    StudioWireProcessor();
    ~StudioWireProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                     { return true; }

    const juce::String getName() const override        { return "StudioWire"; }
    bool acceptsMidi() const override                   { return false; }
    bool producesMidi() const override                  { return false; }
    bool isMidiEffect() const override                  { return false; }
    double getTailLengthSeconds() const override        { return 0.0; }

    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Sharing (called from the UI, on the message thread).
    bool startSharing (bool withTunnel, juce::String& error);
    void stopSharing();
    bool isSharing() const                              { return server.isRunning(); }
    bool isTunnelWanted() const                         { return tunnelWanted; }

    juce::String getPublicLink() const;   // empty until the tunnel is ready
    juce::String getLocalLink() const;

    studiowire::StreamServer& getServer()                 { return server; }
    Tunnel& getTunnel()                                 { return tunnel; }

    juce::File getCloudflared() const                   { return cloudflared.getExecutable(); }
    Cloudflared& getCloudflaredManager()                { return cloudflared; }

    // Stream quality: a plugin parameter, so the DAW saves it with the project.
    // Index 0 = Opus, 1 = CD, 2 = Hi-Res (same order as studiowire::qualityRank).
    juce::AudioParameterChoice& getMaxQualityParam()    { return *maxQualityParam; }
    studiowire::Quality getMaxQuality() const             { return studiowire::qualityFromRank (maxQualityParam->getIndex()); }
    void setCloudflaredOverride (const juce::File& f)   { cloudflared.setOverride (f); }

private:
    void timerCallback() override;      // applies the stream quality (message thread)

    juce::AudioParameterChoice* maxQualityParam = nullptr;
    studiowire::StreamServer server;
    Tunnel tunnel;
    Cloudflared cloudflared;
    bool tunnelWanted = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StudioWireProcessor)
};
