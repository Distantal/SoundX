cmake_minimum_required(VERSION 3.22)
project(SoundX VERSION 0.7.0 LANGUAGES C CXX)
set(SOUNDX_VERSION_LABEL "0.07Beta")
# author / vendor string: goes into the exe's version resource, the installer and the About box
set(SOUNDX_AUTHOR "Distantal")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SOUNDX_VST2 "Enable VST2 hosting using the bundled minimal headers" ON)
option(SOUNDX_ASIO "Enable ASIO driver support (requires third_party/asiosdk)" ON)
# ARA (Audio Random Access, Celemony): lets Melodyne-style plug-ins read the audio files and the
# timeline of a project instead of streaming through an insert slot. Needs the ARA SDK in
# third_party/ARA_SDK (see README, "ARA / AAX"); without it the build simply has no ARA support.
option(SOUNDX_ARA "Enable ARA hosting (requires third_party/ARA_SDK)" ON)
# macOS: the hardened runtime is required for notarisation, but it also turns on "library
# validation", so a plug-in host needs installer/SoundX.entitlements (package-mac.sh passes it).
option(SOUNDX_MAC_HARDENED "Enable the macOS hardened runtime (needs the entitlements file)" OFF)
# Debugging aid only: instrument the application with AddressSanitizer (MSVC /fsanitize=address).
# The JUCE modules stay uninstrumented, allocations are still tracked by the ASan runtime, which
# is enough to catch an out-of-bounds write in the app's own buffers. Never used for releases.
option(SOUNDX_ASAN "Build the application with AddressSanitizer (debugging)" OFF)

add_subdirectory(third_party/JUCE)

# ARA: JUCE implements the host side (juce_ARAHosting.h), the ARA library next to the SDK's API
# headers provides the dispatch that talks to the plug-in. Both are only wired up when the SDK
# is actually there, so a checkout without third_party/ARA_SDK still builds.
set(SOUNDX_ARA_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/ARA_SDK")
if(SOUNDX_ARA AND EXISTS "${SOUNDX_ARA_SDK_DIR}/ARA_API/ARAInterface.h")
    set(SOUNDX_ARA_FLAG 1)
    # JUCE compiles the whole host side itself (juce_ARAHosting.cpp includes the SDK's host
    # dispatch), so only the include path has to be handed over here
    juce_set_ara_sdk_path("${SOUNDX_ARA_SDK_DIR}")
    # the SDK's own version numbers, so the UI and the self test can say which ARA is built in
    set(SOUNDX_ARA_VERSION "2.2.0")
    if(EXISTS "${SOUNDX_ARA_SDK_DIR}/ARA_API/ARA_Version.cmake")
        file(STRINGS "${SOUNDX_ARA_SDK_DIR}/ARA_API/ARA_Version.cmake" _ara_version_lines
             REGEX "set\\(ARA_(MAJOR|MINOR|PATCH)_VERSION")
        foreach(_part MAJOR MINOR PATCH)
            foreach(_line IN LISTS _ara_version_lines)
                if(_line MATCHES "set\\(ARA_${_part}_VERSION +([0-9]+)\\)")
                    set(_ara_${_part} "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endforeach()
        if(_ara_MAJOR AND _ara_MINOR AND _ara_PATCH)
            set(SOUNDX_ARA_VERSION "${_ara_MAJOR}.${_ara_MINOR}.${_ara_PATCH}")
        endif()
    endif()
    message(STATUS "SoundX: ARA ${SOUNDX_ARA_VERSION} host support enabled (${SOUNDX_ARA_SDK_DIR})")
else()
    set(SOUNDX_ARA_FLAG 0)
    set(SOUNDX_ARA_VERSION "0")
    if(SOUNDX_ARA)
        message(STATUS "SoundX: ARA SDK not found in third_party/ARA_SDK - building without ARA support")
    endif()
endif()

# ARA test fixture: JUCE's ARA demo plug-in, built as an ARA-enabled VST3 next to the app so the ARA
# host can be exercised on a machine without a commercial ARA plug-in (Melodyne & co). It is a test
# tool only - it is not installed, not packaged and not scanned unless somebody points a scan at it.
option(SOUNDX_ARA_TEST_PLUGIN "Build JUCE's ARA demo plug-in (VST3) as a test fixture for the ARA host" ON)
if(SOUNDX_ARA_FLAG AND SOUNDX_ARA_TEST_PLUGIN)
    set(SOUNDX_ARA_DEMO_HEADER "${CMAKE_SOURCE_DIR}/third_party/JUCE/examples/Plugins/ARAPluginDemo.h")
    if(EXISTS "${SOUNDX_ARA_DEMO_HEADER}")
        juce_add_pip("${SOUNDX_ARA_DEMO_HEADER}")
        message(STATUS "SoundX: building the ARA demo plug-in as a test fixture")
    endif()
endif()

file(GLOB_RECURSE SOUNDX_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE SOUNDX_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.h ${CMAKE_SOURCE_DIR}/src/*.inc)

juce_add_gui_app(SoundX
    PRODUCT_NAME "SoundX"
    COMPANY_NAME "${SOUNDX_AUTHOR}"
    VERSION ${PROJECT_VERSION}
    # macOS: recording an audio track needs the microphone, and macOS kills the process without a
    # usage description (Windows builds ignore these two arguments)
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "SoundX records audio tracks from the selected input device.")

target_sources(SoundX PRIVATE ${SOUNDX_SOURCES})
target_include_directories(SoundX PRIVATE src third_party/vst2)

# The user manual travels inside the exe: Help > User Manual writes it next to the settings and
# opens it, so a single SoundX.exe is still enough to hand out (no extra file to lose).
if(EXISTS "${CMAKE_SOURCE_DIR}/docs/SoundXManual.pdf")
    juce_add_binary_data(SoundXBinaryData SOURCES "${CMAKE_SOURCE_DIR}/docs/SoundXManual.pdf")
    target_link_libraries(SoundX PRIVATE SoundXBinaryData)
    target_compile_definitions(SoundX PRIVATE SOUNDX_HAS_MANUAL=1)
endif()

if(APPLE)
    # A plug-in host must be allowed to load third-party bundles: with the hardened runtime on,
    # macOS "library validation" blocks every bundle that is not signed by the same team, which
    # makes every plug-in fail to load ("Unable to load VST-3 plug-in file" / a scan that finds no
    # types). installer/SoundX.entitlements is passed to codesign by package-mac.sh; the Xcode
    # attribute below does the same for an Xcode build.
    set(SOUNDX_MAC_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/installer/SoundX.entitlements")
    if(EXISTS "${SOUNDX_MAC_ENTITLEMENTS}")
        set_target_properties(SoundX PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${SOUNDX_MAC_ENTITLEMENTS}"
            XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "$<IF:$<BOOL:${SOUNDX_MAC_HARDENED}>,YES,NO>")
    endif()
endif()

# Ninja's /showIncludes parsing does not pick up the localized include prefix on this
# machine, so header changes never triggered rebuilds (stale objects -> memory corruption
# crashes at shutdown when a class layout changed). Make every source depend on every
# header: conservative, but it keeps incremental builds correct.
set_property(SOURCE ${SOUNDX_SOURCES} APPEND PROPERTY OBJECT_DEPENDS ${SOUNDX_HEADERS})

# ASIO hosting: needs the Steinberg ASIO SDK in third_party/asiosdk (common/ folder)
set(SOUNDX_ASIO_DIR "${CMAKE_SOURCE_DIR}/third_party/asiosdk")
if(SOUNDX_ASIO AND EXISTS "${SOUNDX_ASIO_DIR}/common/iasiodrv.h")
    set(SOUNDX_ASIO_FLAG 1)
    target_include_directories(SoundX PRIVATE "${SOUNDX_ASIO_DIR}/common")
else()
    set(SOUNDX_ASIO_FLAG 0)
    if(SOUNDX_ASIO)
        message(WARNING "ASIO SDK not found in third_party/asiosdk - building without ASIO device support")
    endif()
endif()

if(SOUNDX_VST2)
    set(SOUNDX_VST2_FLAG 1)
else()
    set(SOUNDX_VST2_FLAG 0)
endif()

target_compile_definitions(SoundX PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=${SOUNDX_VST2_FLAG}
    JUCE_PLUGINHOST_ARA=${SOUNDX_ARA_FLAG}
    SOUNDX_ARA_SDK_VERSION="${SOUNDX_ARA_VERSION}"
    JUCE_ASIO=${SOUNDX_ASIO_FLAG}
    JUCE_WASAPI=1
    JUCE_WASAPI_EXCLUSIVE=1
    JUCE_DIRECTSOUND=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_APPLICATION_NAME_STRING="SoundX"
    JUCE_APPLICATION_VERSION_STRING="${SOUNDX_VERSION_LABEL}")

target_link_libraries(SoundX PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(SoundX PRIVATE /utf-8 /MP /wd4100 /wd4505 /EHa)
    # The default 1 MB main-thread stack is easy to exhaust: the window layout code, the self test
    # (one very long function) and a couple of plug-in editors all recurse or carry large frames,
    # and MSVC reports that as an instant 0xC00000FD crash. 8 MB matches what other hosts reserve.
    target_link_options(SoundX PRIVATE "/STACK:8388608,1048576")
    if(SOUNDX_ASAN)
        # application code only: the JUCE module libraries stay uninstrumented so this build is
        # cheap to produce; the ASan runtime still intercepts every allocation they make
        target_compile_options(SoundX PRIVATE /fsanitize=address /Z7)
        target_link_options(SoundX PRIVATE /fsanitize=address /DEBUG)
        add_custom_command(TARGET SoundX POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$ENV{VCToolsInstallDir}/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll"
                    "$<TARGET_FILE_DIR:SoundX>")
        message(STATUS "SoundX: AddressSanitizer build (application code only)")
    else()
        # Developer diagnostics: keep a PDB and the linker map next to the build directory. They are
        # NOT shipped (package.ps1 only copies the exe) but the crash handler in Main.cpp uses them to
        # name the faulting function / source line, which is the only way to debug a user's crash log.
        # It costs link time only; the exe keeps the same code and optimisation.
        target_compile_options(SoundX PRIVATE /Z7)
        target_link_options(SoundX PRIVATE /DEBUG "/PDB:${CMAKE_BINARY_DIR}/SoundX.pdb"
                                              "/MAP:${CMAKE_BINARY_DIR}/SoundX.map")
    endif()
endif()


#==============================================================================
# Installer: embeds the built application and writes it to a folder the user picks.
# Built only when the payload exists (i.e. after the app itself has been built once).
set(SOUNDX_PAYLOAD "${CMAKE_SOURCE_DIR}/dist/SoundX.exe")

# SoundX.exe links the Microsoft C++ runtime dynamically, so the redistributable DLLs have to
# travel with it: a freshly installed Windows 10/11 machine does not ship the VC++ 2015-2022
# runtime, and without it the program cannot start at all. package.ps1 fills dist/runtime with
# them; they are embedded here and written next to SoundX.exe (app-local deployment, no admin
# rights and no separate redistributable install).
file(GLOB SOUNDX_RUNTIME_DLLS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/dist/runtime/*.dll")
# the manual is shipped as a real file next to the installed exe as well (it is also embedded in
# SoundX.exe itself for the Help menu), so a user can find / print / send it without the program
set(SOUNDX_PAYLOAD_MANUAL "")
if(EXISTS "${CMAKE_SOURCE_DIR}/docs/SoundXManual.pdf")
    set(SOUNDX_PAYLOAD_MANUAL "${CMAKE_SOURCE_DIR}/docs/SoundXManual.pdf")
endif()

if(EXISTS "${SOUNDX_PAYLOAD}")
    juce_add_binary_data(SoundXPayload SOURCES "${SOUNDX_PAYLOAD}" ${SOUNDX_RUNTIME_DLLS} ${SOUNDX_PAYLOAD_MANUAL})

    # the setup object embeds BinaryData::SoundX_exeSize: keep it in sync with the payload,
# otherwise a stale constant writes a truncated exe ("not a valid Win32 application")
set_source_files_properties("${CMAKE_SOURCE_DIR}/installer/SetupMain.cpp"
    PROPERTIES OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/juce_binarydata_SoundXPayload/JuceLibraryCode/BinaryData.h")

juce_add_gui_app(SoundXSetup
        PRODUCT_NAME "SoundX Setup"
        COMPANY_NAME "${SOUNDX_AUTHOR}"
        VERSION ${PROJECT_VERSION})

    target_sources(SoundXSetup PRIVATE installer/SetupMain.cpp)
    target_compile_definitions(SoundXSetup PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_APPLICATION_NAME_STRING="SoundX Setup"
        JUCE_APPLICATION_VERSION_STRING="${SOUNDX_VERSION_LABEL}")
    target_link_libraries(SoundXSetup PRIVATE
        SoundXPayload
        juce::juce_core
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    if(MSVC)
        target_compile_options(SoundXSetup PRIVATE /utf-8 /MP /wd4100 /wd4505)
    endif()
endif()

# The Windows resource file carries FILEVERSION / PRODUCTVERSION, and JUCE 8.0.4 generates it with
# a custom command that only depends on the icon: bumping project(VERSION ...) alone would keep
# shipping the old version numbers inside the exe and inside the installer. Drop the generated file
# when its input (Info.txt, which holds the version) is newer, so the next build rewrites it.
foreach(_soundx_rc_target SoundX SoundXSetup)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND TARGET ${_soundx_rc_target})
        get_target_property(_soundx_info ${_soundx_rc_target} JUCE_INFO_FILE)
        get_target_property(_soundx_code ${_soundx_rc_target} JUCE_GENERATED_SOURCES_DIRECTORY)
        if(_soundx_info AND _soundx_code)
            set(_soundx_rc "${_soundx_code}/${_soundx_rc_target}_resources.rc")
            if(EXISTS "${_soundx_rc}" AND EXISTS "${_soundx_info}" AND "${_soundx_info}" IS_NEWER_THAN "${_soundx_rc}")
                message(STATUS "SoundX: version info changed, regenerating ${_soundx_rc_target}_resources.rc")
                file(REMOVE "${_soundx_rc}")
            endif()
        endif()
    endif()
endforeach()