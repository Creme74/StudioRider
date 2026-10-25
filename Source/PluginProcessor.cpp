// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
#include "PluginProcessor.h"
#include "PluginEditor.h"

StudioRiderProcessor::StudioRiderProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (maxQualityParam = new juce::AudioParameterChoice (
        juce::ParameterID { "maxQuality", 1 },
        "Stream quality",
        juce::StringArray { "Opus 256 kbps",
                            juce::String::fromUTF8 ("CD · FLAC 16-bit"),
                            juce::String::fromUTF8 ("Hi-Res · FLAC 24-bit") },
        1)); // CD by default

    // The change can come from the host on any thread (automation, project
    // loading): it is applied to the server from the message thread.
    startTimerHz (10);
}

StudioRiderProcessor::~StudioRiderProcessor()
{
    stopTimer();
    stopSharing();
}

void StudioRiderProcessor::timerCallback()
{
    const auto q = getMaxQuality();
    if (server.getMaxQuality() != q)
        server.setMaxQuality (q);
}

void StudioRiderProcessor::prepareToPlay (double sampleRate, int)
{
    server.setSampleRate (sampleRate);
}

bool StudioRiderProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void StudioRiderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Audio passes through the plugin untouched.
    // Nothing is sent during an offline (faster than real time) bounce.
    if (! isNonRealtime())
        server.pushAudio (buffer.getArrayOfReadPointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

//==============================================================================
bool StudioRiderProcessor::startSharing (bool withTunnel, juce::String& error)
{
    tunnelWanted = withTunnel;

    if (! server.isRunning())
    {
        studiorider::StreamServer::Options opts;
        opts.maxQuality = getMaxQuality();
        std::string err;
        if (! server.start (opts, err))
        {
            error = juce::String (err);
            return false;
        }
        server.setSampleRate (getSampleRate() > 0 ? getSampleRate() : 48000.0);
    }

    if (withTunnel)
    {
        const auto exe = getCloudflared();
        if (! exe.existsAsFile())
        {
            error = "cloudflared not found: click Download cloudflared below, "
                    "or install it with Homebrew (brew install cloudflared).";
            return true; // the local server is running anyway
        }
        juce::String tunnelError;
        if (! tunnel.start (exe, server.getPort(), tunnelError))
            error = tunnelError;
    }

    return true;
}

void StudioRiderProcessor::stopSharing()
{
    const bool hadListeners = server.getListenerCount() > 0;
    server.stop();  // tells the listeners first (the tunnel must still be up)
    if (hadListeners && tunnel.getState() == Tunnel::State::ready)
        juce::Thread::sleep (500); // gives the "sharing stopped" message time to get through the tunnel
    tunnel.stop();
}

juce::String StudioRiderProcessor::getPublicLink() const
{
    const auto base = tunnel.getPublicUrl();
    if (base.isEmpty() || ! server.isRunning())
        return {};
    return base + juce::String (server.getPath());
}

juce::String StudioRiderProcessor::getLocalLink() const
{
    return server.isRunning() ? juce::String (server.getLocalUrl()) : juce::String();
}

//==============================================================================
void StudioRiderProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // "Sharing active" is never saved: when a project opens, nothing is
    // exposed until you click Start sharing.
    juce::ValueTree state ("StudioRider");
    state.setProperty ("cloudflared", cloudflared.getOverride().getFullPathName(), nullptr);
    state.setProperty ("tunnel", tunnelWanted, nullptr);
    state.setProperty ("maxQuality", studiorider::qualityId (getMaxQuality()), nullptr);
    juce::MemoryOutputStream out (destData, false);
    state.writeToStream (out);
}

void StudioRiderProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    // StudioWire and MixShare: former names, same plugin codes.
    if (! state.hasType ("StudioRider") && ! state.hasType ("StudioWire") && ! state.hasType ("MixShare"))
        return;
    const auto path = state.getProperty ("cloudflared").toString();
    cloudflared.setOverride (juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File());
    tunnelWanted = (bool) state.getProperty ("tunnel", true);

    studiorider::Quality q;
    if (studiorider::parseQuality (state.getProperty ("maxQuality", "cd").toString().toRawUTF8(), q))
        *maxQualityParam = studiorider::qualityRank (q);
}

//==============================================================================
juce::AudioProcessorEditor* StudioRiderProcessor::createEditor()
{
    return new StudioRiderEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StudioRiderProcessor();
}
