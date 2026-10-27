cmake_minimum_required(VERSION 3.22)
project(SampleGrabber VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)

# Signalsmith Stretch (MIT) + Signalsmith Linear (MIT): header-only libraries.
# Their CMakeLists require CMake >= 3.24 (Ubuntu 22.04 ships 3.22) -> SOURCE_SUBDIR points to a
# non-existent directory: the sources are only downloaded, not pulled in via add_subdirectory.
FetchContent_Declare(signalsmith-linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG 0.3.1
    GIT_SHALLOW ON
    SOURCE_SUBDIR _header_only)
FetchContent_Declare(signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG main                               # TODO pin to a fixed commit for releases
    GIT_SHALLOW ON
    SOURCE_SUBDIR _header_only)

FetchContent_MakeAvailable(JUCE signalsmith-linear signalsmith-stretch)

set(SG_STRETCH_INCLUDES
    ${signalsmith-stretch_SOURCE_DIR}
    ${signalsmith-stretch_SOURCE_DIR}/include
    ${signalsmith-linear_SOURCE_DIR}
    ${signalsmith-linear_SOURCE_DIR}/include)

set(SG_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Downloader.cpp
    Source/ToolManager.cpp)

# Brand assets (UI_BRAND.md 1): SVGs and fonts (SIL OFL) as BinaryData, accessed via BinaryData.h.
# SVGs always; fonts only if all four files exist (otherwise default font, SG_HAS_BRAND_FONTS=0).
set(SG_BRAND_SOURCES
    brand/lockup/lockup-horizontal-dark.svg
    brand/lockup/lockup-horizontal-light.svg)

set(SG_BRAND_FONTS
    resources/fonts/Inter-Regular.ttf
    resources/fonts/Inter-Medium.ttf
    resources/fonts/Inter-SemiBold.ttf
    resources/fonts/JetBrainsMono-Medium.ttf)

set(SG_HAS_BRAND_FONTS 1)
foreach(font ${SG_BRAND_FONTS})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${font}")
        message(WARNING "Brand font missing: ${font} -> default font")
        set(SG_HAS_BRAND_FONTS 0)
    endif()
endforeach()

if(SG_HAS_BRAND_FONTS)
    list(APPEND SG_BRAND_SOURCES ${SG_BRAND_FONTS})
endif()

juce_add_binary_data(SGBrandData SOURCES ${SG_BRAND_SOURCES})

set(SG_ICON_ARGS
    ICON_BIG   brand/icon/icon-1024.png
    ICON_SMALL brand/icon/icon-256.png)

# ---------------------------------------------------------------- VST3 plugin
juce_add_plugin(SampleGrabber
    COMPANY_NAME "Mythen Studios"
    BUNDLE_ID com.mythenstudios.samplegrabber
    IS_SYNTH TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE       # required so URLs can be pasted in the DAW
    NEEDS_MIDI_INPUT TRUE
    PLUGIN_MANUFACTURER_CODE Wrst
    PLUGIN_CODE Sgrb
    FORMATS VST3
    ${SG_ICON_ARGS}
    PRODUCT_NAME "Sample Grabber")

target_sources(SampleGrabber PRIVATE ${SG_SOURCES})
target_include_directories(SampleGrabber SYSTEM PRIVATE ${SG_STRETCH_INCLUDES})

target_compile_definitions(SampleGrabber PUBLIC
    SG_HAS_BRAND_FONTS=${SG_HAS_BRAND_FONTS}
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SampleGrabber
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_gui_basics
        juce::juce_dsp
        juce::juce_cryptography
        SGBrandData
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ------------------------------------------- Standalone app (no DAW)
juce_add_gui_app(SampleGrabberApp
    PRODUCT_NAME "Sample Grabber"
    COMPANY_NAME "Mythen Studios"
    BUNDLE_ID com.mythenstudios.samplegrabberapp
    ${SG_ICON_ARGS}
    VERSION 0.1.0)

target_sources(SampleGrabberApp PRIVATE Source/App/Main.cpp ${SG_SOURCES})
target_include_directories(SampleGrabberApp PRIVATE Source)
target_include_directories(SampleGrabberApp SYSTEM PRIVATE ${SG_STRETCH_INCLUDES})

target_compile_definitions(SampleGrabberApp PRIVATE
    SG_HAS_BRAND_FONTS=${SG_HAS_BRAND_FONTS}
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SampleGrabberApp,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SampleGrabberApp,JUCE_VERSION>")

target_link_libraries(SampleGrabberApp
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_dsp
        juce::juce_cryptography
        SGBrandData
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
