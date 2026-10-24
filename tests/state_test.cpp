// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
// Checks that the stream quality is saved and restored with the project,
// and that it is applied to the server.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "PluginProcessor.h"
#include <iostream>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();


int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    int fails = 0;
    auto check = [&] (bool c, const char* m) { std::cout << (c ? "OK   " : "FAIL ") << m << "\n"; if (! c) ++fails; };

    std::unique_ptr<StudioWireProcessor> a (static_cast<StudioWireProcessor*> (createPluginFilter()));
    check (a->getMaxQuality() == studiowire::Quality::cd, "new instance: stream quality = CD");
    check (a->getParameters().size() == 1, "a single parameter exposed to the DAW");
    std::cout << "     parameter: " << a->getMaxQualityParam().getName (64) << " = " << a->getMaxQualityParam().getCurrentValueAsText() << "\n";

    a->getMaxQualityParam().setValueNotifyingHost (a->getMaxQualityParam().convertTo0to1 (2.0f)); // Hi-Res
    check (a->getMaxQuality() == studiowire::Quality::hires, "set to Hi-Res");

    juce::MemoryBlock state;
    a->getStateInformation (state);

    std::unique_ptr<StudioWireProcessor> b (static_cast<StudioWireProcessor*> (createPluginFilter()));
    b->setStateInformation (state.getData(), (int) state.getSize());
    check (b->getMaxQuality() == studiowire::Quality::hires, "project reopened: Hi-Res restored");

    // A project saved under the former name (MixShare) still restores its stream quality.
    {
        juce::ValueTree legacy ("MixShare");
        legacy.setProperty ("maxQuality", "eco", nullptr);
        juce::MemoryBlock legacyState;
        juce::MemoryOutputStream out (legacyState, false);
        legacy.writeToStream (out);
        out.flush();
        std::unique_ptr<StudioWireProcessor> c (static_cast<StudioWireProcessor*> (createPluginFilter()));
        c->setStateInformation (legacyState.getData(), (int) legacyState.getSize());
        check (c->getMaxQuality() == studiowire::Quality::eco, "MixShare project: Opus restored");
    }

    // Applied to the server by the plugin's timer (message thread), then changed while live.
    auto* bp = b.get();
    juce::Timer::callAfterDelay (300, [&, bp]
    {
        check (bp->getServer().getMaxQuality() == studiowire::Quality::hires, "server set to Hi-Res");
        bp->getMaxQualityParam().setValueNotifyingHost (bp->getMaxQualityParam().convertTo0to1 (0.0f)); // Opus
        juce::Timer::callAfterDelay (300, [&, bp]
        {
            check (bp->getServer().getMaxQuality() == studiowire::Quality::eco, "changed while live: server switched to Opus");
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();

    a.reset(); b.reset();
    std::cout << (fails ? "Failures\n" : "All tests pass.\n");
    return fails;
}
