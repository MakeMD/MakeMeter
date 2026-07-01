cmake_minimum_required(VERSION 3.22)
project(MakeMeter VERSION 1.0.0)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(MakeMeter
    COMPANY_NAME "MakeMD"
    PLUGIN_MANUFACTURER_CODE Mkmd
    PLUGIN_CODE Mkmt
    FORMATS VST3 Standalone
    PRODUCT_NAME "MakeMeter"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    # Per-user VST3 dir — no admin needed (add this path to your DAW's VST3 scan list).
    VST3_COPY_DIR "$ENV{LOCALAPPDATA}/Programs/Common/VST3")

target_sources(MakeMeter PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_features(MakeMeter PRIVATE cxx_std_17)

# Cyrillic string literals are UTF-8 — tell MSVC so JUCE renders them correctly.
if (MSVC)
    target_compile_options(MakeMeter PRIVATE /utf-8)
endif()

target_compile_definitions(MakeMeter PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MakeMeter
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# --- self-check console app ---
juce_add_console_app(MakeMeterTest PRODUCT_NAME "MakeMeterTest")
target_sources(MakeMeterTest PRIVATE Tests/MeterTest.cpp)
target_compile_features(MakeMeterTest PRIVATE cxx_std_17)
target_include_directories(MakeMeterTest PRIVATE Source)
target_compile_definitions(MakeMeterTest PRIVATE JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(MakeMeterTest PRIVATE
    juce::juce_dsp
    juce::juce_audio_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# --- spectrum smoothness visual check (renders the curve to a PNG) ---
juce_add_console_app(MakeMeterSpec PRODUCT_NAME "MakeMeterSpec")
target_sources(MakeMeterSpec PRIVATE Tests/SpecShot.cpp)
target_compile_features(MakeMeterSpec PRIVATE cxx_std_17)
target_include_directories(MakeMeterSpec PRIVATE Source)
target_compile_definitions(MakeMeterSpec PRIVATE
    JUCE_STANDALONE_APPLICATION=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(MakeMeterSpec PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_audio_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# --- EBU LUFS calibration ---
juce_add_console_app(MakeMeterCalib PRODUCT_NAME "MakeMeterCalib")
target_sources(MakeMeterCalib PRIVATE Tests/CalibEbu.cpp)
target_compile_features(MakeMeterCalib PRIVATE cxx_std_17)
target_include_directories(MakeMeterCalib PRIVATE Source)
target_compile_definitions(MakeMeterCalib PRIVATE JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(MakeMeterCalib PRIVATE
    juce::juce_dsp
    juce::juce_audio_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
