#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace sx
{
// language: "auto", "en" or "zh". Must be called before any UI is created.
void applyLanguage (const juce::String& language);
juce::String currentLanguageSetting (juce::PropertiesFile* props);
void saveLanguageSetting (juce::PropertiesFile* props, const juce::String& language);
}
