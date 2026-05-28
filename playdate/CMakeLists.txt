cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})

if (NOT ${ENVSDK} STREQUAL "")
	file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
	execute_process(
		COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
		COMMAND head -n 1
		COMMAND cut -c9-
		OUTPUT_VARIABLE SDK
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
endif()

if (NOT EXISTS ${SDK})
	message(FATAL_ERROR "SDK Path not found; set ENV value PLAYDATE_SDK_PATH")
	return()
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
set(CMAKE_XCODE_GENERATE_SCHEME TRUE)

set(PLAYDATE_GAME_NAME redviper)
set(PLAYDATE_GAME_DEVICE redviper_DEVICE)

project(${PLAYDATE_GAME_NAME} C ASM)

# v0: just the Playdate frontend. Shared V810 core wiring comes later
# (see playdate/NOTES.md §9).
set(REDVIPER_PD_SRC
	src/main.c
)

if (TOOLCHAIN STREQUAL "armgcc")
	add_executable(${PLAYDATE_GAME_DEVICE} ${REDVIPER_PD_SRC})
else()
	add_library(${PLAYDATE_GAME_NAME} SHARED ${REDVIPER_PD_SRC})
endif()

include(${SDK}/C_API/buildsupport/playdate_game.cmake)
