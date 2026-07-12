# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Source list depends on platform. PlatformUnicodeJava.cpp needs fbjni; we
# exclude it when fbjni is unavailable.
if(HERMES_UNICODE_LITE)
  set(source_files
    PlatformUnicodeLite.cpp
    CharacterProperties.cpp)
elseif(HERMES_IS_ANDROID AND NOT TARGET fbjni::fbjni)
  # Android, but we don't have fbjni — drop the Java implementation.
  set(source_files
    PlatformUnicodeLite.cpp
    CharacterProperties.cpp)
else()
  set(source_files
    PlatformUnicodeJava.cpp
    PlatformUnicodeCF.cpp
    PlatformUnicodeICU.cpp
    PlatformUnicodeLite.cpp
    PlatformUnicodeEmscripten.cpp
    CharacterProperties.cpp)
endif()

add_hermes_library(hermesPlatformUnicode ${source_files})

if(HERMES_UNICODE_LITE)
  # nothing
elseif(HERMES_IS_ANDROID AND TARGET fbjni::fbjni)
  target_link_libraries(hermesPlatformUnicode_obj fbjni::fbjni)
  set_source_files_properties(PlatformUnicodeJava.cpp PROPERTIES
    COMPILE_FLAGS "-frtti -fexceptions")
elseif(HERMES_IS_ANDROID)
  message(STATUS "Skipping fbjni link for hermesPlatformUnicode (no fbjni available)")
elseif(APPLE)
  target_link_libraries(hermesPlatformUnicode_obj ${CORE_FOUNDATION})
endif()

hermes_link_icu(hermesPlatformUnicode_obj)
