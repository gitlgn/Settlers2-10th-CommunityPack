cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(SettlersDNGPatches LANGUAGES CXX)

# static CRT: no dependency on the VC++ redistributable (clean PCs, sandbox)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

#add_definitions(-DUNICODE -D_UNICODE)
add_definitions(-DCMAKE_GENERATOR_PLATFORM=Win32)

add_subdirectory(utilities)

add_library(${PROJECT_NAME} SHARED
	d3d9.cpp
	Config.cpp
	Lobby.cpp
	MainPatch.cpp
	MainPatchASM.cpp
	CrashDiag.cpp
	LAAStress.cpp
	LowHeap.cpp
	GameGuard.cpp
	TimeFix.cpp
	SafeSave.cpp
	NetTrace.cpp
	BinaryFixes.cpp
	ModSync.cpp
	UIScale.cpp
	GfxPatches.cpp
	FixDecisions.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE CppUtils)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(${PROJECT_NAME} PROPERTIES
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/d3d9.def"
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    
    OUTPUT_NAME "d3d9"
	SUFFIX ".dll"
)

target_precompile_headers(${PROJECT_NAME} PRIVATE pch.h)

# Headless unit tests: exercise the real FixDecisions logic (the same .cpp the DLL links) without the
# game, a second LAN player or a live network transfer. Build: cmake --build build --target FixTests
# Run:  build\bin\FixTests.exe
add_executable(FixTests
	FixDecisions.cpp
	tests/FixTestsMain.cpp
)
target_link_libraries(FixTests PRIVATE CppUtils)
target_include_directories(FixTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(FixTests PRIVATE cxx_std_20)
set_target_properties(FixTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
target_precompile_headers(FixTests PRIVATE pch.h)
