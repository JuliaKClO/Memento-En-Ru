cmake_minimum_required(VERSION 3.16)

project(
    Memento
    VERSION 2.0.2
    DESCRIPTION "An mpv-based video player for studying Japanese"
	HOMEPAGE_URL "https://ripose-jp.github.io/Memento"
    LANGUAGES C CXX
)

if(APPLE)
    enable_language(OBJCXX)
endif()

# Set the default build type
set(default_build_type "RelWithDebInfo")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
	set(
		CMAKE_BUILD_TYPE "${default_build_type}" CACHE
    	STRING "Choose the type of build." FORCE
	)
  	# Set the possible values of build type for cmake-gui
  	set_property(
		CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    	"Debug" "Release" "MinSizeRel" "RelWithDebInfo"
	)
endif()

# Require out-of-source builds
file(TO_CMAKE_PATH "${PROJECT_BINARY_DIR}/CMakeLists.txt" LOC_PATH)
if(EXISTS "${LOC_PATH}")
    message(FATAL_ERROR "You cannot build in a source directory (or any directory with a CMakeLists.txt file). Please make a build subdirectory. Feel free to remove CMakeCache.txt and CMakeFiles.")
endif()

add_subdirectory(option)

# Check that dangerous options have no been toggled
if(NOT APPLE AND MEMENTO_BUNDLE)
	message(FATAL_ERROR "You cannot set the MEMENTO_BUNDLE option to ON on non-Apple platforms.")
endif()

# Set the module path
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

# Make IDEs understand the code better
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Set Compiler Flags
list(
	APPEND
	MEMENTO_COMPILER_FLAGS
	"-Wall"
	"-Wextra"
	"-Wpedantic"
	"$<$<BOOL:${MEMENTO_BUNDLE}>:-DMEMENTO_BUNDLE=1>"
	"$<$<BOOL:${MEMENTO_ASAN}>:-fsanitize=address>"
	"$<$<BOOL:${MEMENTO_MECAB_SUPPORT}>:-DMEMENTO_MECAB_SUPPORT=1>"
	"$<$<BOOL:${MEMENTO_OCR_SUPPORT}>:-DMEMENTO_OCR_SUPPORT=1>"
	"$<$<BOOL:${MEMENTO_QAPPLICATION}>:-DMEMENTO_QAPPLICATION=1>"
	"$<$<BOOL:${MEMENTO_SYSTEM_QCORO}>:-DMEMENTO_SYSTEM_QCORO=1>"
	"$<$<BOOL:${MEMENTO_WERROR}>:-Werror>"
)

# Avoid generating copy relocations for protected Qt data symbols on Linux.
# Some system Qt builds require indirect external access and the dynamic loader
# rejects an executable that references their protected data via R_X86_64_COPY.
if(
	CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
	CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
)
	include(CheckCCompilerFlag)
	include(CheckCXXCompilerFlag)
	if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
		CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		set(MEMENTO_NO_DIRECT_EXTERN_ACCESS_FLAG "-mno-direct-extern-access")
	elseif(CMAKE_C_COMPILER_ID MATCHES "Clang" AND
		CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		set(MEMENTO_NO_DIRECT_EXTERN_ACCESS_FLAG
			"-fno-direct-access-external-data")
	endif()
	if(MEMENTO_NO_DIRECT_EXTERN_ACCESS_FLAG)
		check_c_compiler_flag(
			"${MEMENTO_NO_DIRECT_EXTERN_ACCESS_FLAG}"
			MEMENTO_C_SUPPORTS_NO_DIRECT_EXTERN_ACCESS
		)
		check_cxx_compiler_flag(
			"${MEMENTO_NO_DIRECT_EXTERN_ACCESS_FLAG}"
			MEMENTO_CXX_SUPPORTS_NO_DIRECT_EXTERN_ACCESS
		)
	endif()
	if(MEMENTO_C_SUPPORTS_NO_DIRECT_EXTERN_ACCESS AND
		MEMENTO_CXX_SUPPORTS_NO_DIRECT_EXTERN_ACCESS)
		list(APPEND MEMENTO_COMPILER_FLAGS
			"${MEMENTO_NO_DIRECT_EXTERN_ACCESS_FLAG}")
	else()
		message(WARNING "The C/C++ toolchain cannot enable indirect external "
			"data access; the Linux executable may not start with Qt builds "
			"that require it.")
	endif()
endif()

if(MEMENTO_ASAN)
	add_link_options(-fsanitize=address)
endif()

# Get the Git version hash
if(NOT MEMENTO_RELEASE_BUILD)
	find_package(Git QUIET)

	if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
		execute_process(
			COMMAND ${GIT_EXECUTABLE} log --pretty=format:%h -n 1
			WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
			OUTPUT_VARIABLE MEMENTO_VERSION_HASH
		)
	endif()
endif()

# Set the proper variables for macOS cross compilation
if(MEMENTO_MAC_CROSSCOMPILE_X86)
	set(CMAKE_OSX_ARCHITECTURES "x86_64")
endif()

# Generate include directories
if(APPLE AND NOT MEMENTO_MAC_CROSSCOMPILE_X86)
	# This makes sure Memento builds on whatever arch is native
	# The order of directories MATTERS because M1 users may cross compile but
	# x86_64 users will not
	set(
		MEMENTO_INCLUDE_DIRS

		# arm64 locations
		"/opt/homebrew/opt/qt6/include"
		"/opt/homebrew/include"

		# x86_64 locations
		"/usr/local/opt/qt6/include/"
		"/usr/local/include"
	)
elseif(APPLE AND MEMENTO_MAC_CROSSCOMPILE_X86)
	# This is a cross compile target, x86_64 users shouldn't need to use this
	set(
		MEMENTO_INCLUDE_DIRS
		"/usr/local/opt/qt6/include/"
		"/usr/local/include"
	)
endif()
list(PREPEND MEMENTO_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src")

# Set prefix path for macOS
if(APPLE AND NOT MEMENTO_MAC_CROSSCOMPILE_X86)
	# x86_64 locations
	list(PREPEND CMAKE_PREFIX_PATH "/usr/local/opt")
	list(PREPEND CMAKE_PREFIX_PATH "/usr/local/")

	# arm64 locations
	list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt")
	list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew")
elseif(APPLE AND MEMENTO_MAC_CROSSCOMPILE_X86)
	# x86_64 locations
	list(PREPEND CMAKE_PREFIX_PATH "/usr/local/opt")
	list(PREPEND CMAKE_PREFIX_PATH "/usr/local/")
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(JsonC REQUIRED)
find_package(libzip REQUIRED)
find_package(mpv REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite3::SQLite3 AND TARGET SQLite::SQLite3)
	add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
if(MEMENTO_SYSTEM_QCORO)
	find_package(QCoro6 REQUIRED COMPONENTS Core Network Qml)
endif()
if(MEMENTO_MECAB_SUPPORT)
	find_package(MeCab REQUIRED)
endif()
if(MEMENTO_OCR_SUPPORT AND MEMENTO_SYSTEM_MOCR)
	find_package(mocr REQUIRED)
endif()

set(
	MEMENTO_QT_COMPONENTS
	Concurrent
	Core
	Gui
	LinguistTools
	Network
	OpenGL
	Qml
	Quick
	QuickControls2
	Svg
)
if(APPLE)
	list(APPEND MEMENTO_QT_COMPONENTS QuickPrivate)
endif()
if(UNIX AND NOT APPLE)
	list(APPEND MEMENTO_QT_COMPONENTS DBus)
endif()
if(MEMENTO_QAPPLICATION)
	list(APPEND MEMENTO_QT_COMPONENTS Widgets)
endif()
set(QT_NO_PRIVATE_MODULE_WARNING ON CACHE BOOL "Suppress Qt private module warning" FORCE)
find_package(Qt6 REQUIRED COMPONENTS ${MEMENTO_QT_COMPONENTS})
unset(MEMENTO_QT_COMPONENTS)

qt_standard_project_setup(REQUIRES 6.9)

if(WIN32)
	add_subdirectory(windows)
endif()

add_subdirectory(extern)
add_subdirectory(src)
add_subdirectory(res)

option(
	MEMENTO_BUILD_ENGLISH_TESTS
	"Build standalone English search and sentence assistant tests"
	OFF
)
option(
	MEMENTO_TEST_PROFILE
	"Use persistent isolated Memento-En-Ru-Test paths for local testing"
	OFF
)
if(MEMENTO_TEST_PROFILE)
	target_compile_definitions(memento PRIVATE MEMENTO_TEST_PROFILE=1)
endif()
if(MEMENTO_BUILD_ENGLISH_TESTS)
	enable_testing()
    add_executable(memento_database_tests tests/database_tests.cpp)
    target_include_directories(memento_database_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(memento_database_tests PRIVATE dictionary yomidbbuilder libzip::libzip SQLite3::SQLite3)
    add_test(NAME memento_database_tests COMMAND memento_database_tests)
    add_executable(memento_assets_tests tests/assets_tests.cpp)
    target_link_libraries(memento_assets_tests PRIVATE Qt6::Gui Qt6::Svg)
    add_dependencies(memento_assets_tests Memento_lrelease)
    add_test(NAME memento_assets_tests COMMAND memento_assets_tests "${PROJECT_SOURCE_DIR}/res/images" "${PROJECT_BINARY_DIR}/src/memento_ru.qm")
    set_tests_properties(memento_assets_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
	add_executable(
		memento_englishquerygenerator_tests
		tests/englishquerygenerator_tests.cpp
		src/dict/englishquerygenerator.cpp
		src/dict/data/irregularverbs.cpp
		src/dict/exactquerygenerator.cpp
	)
	set_target_properties(
		memento_englishquerygenerator_tests
		PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
	)
	target_compile_features(memento_englishquerygenerator_tests PRIVATE cxx_std_20)
	target_include_directories(
		memento_englishquerygenerator_tests
		PRIVATE "${PROJECT_SOURCE_DIR}/src"
	)
	target_link_libraries(
		memento_englishquerygenerator_tests
		PRIVATE Qt6::Core querygenerator
	)
	add_test(
		NAME memento_englishquerygenerator_tests
		COMMAND memento_englishquerygenerator_tests
	)
	add_executable(
		memento_sentenceassistant_tests
		tests/sentenceassistant_tests.cpp
		src/subtitle/sentenceassistant.cpp
		src/subtitle/sentenceassistant.h
	)
	set_target_properties(
		memento_sentenceassistant_tests
		PROPERTIES AUTOMOC ON AUTOUIC OFF AUTORCC OFF
	)
	target_compile_features(memento_sentenceassistant_tests PRIVATE cxx_std_20)
	target_include_directories(
		memento_sentenceassistant_tests
		PRIVATE "${PROJECT_SOURCE_DIR}/src"
	)
	target_link_libraries(
		memento_sentenceassistant_tests
		PRIVATE Qt6::Core Qt6::Gui
	)
	add_test(
		NAME memento_sentenceassistant_tests
		COMMAND memento_sentenceassistant_tests
	)
	add_executable(
		memento_lingvo_renderer_tests
		tests/lingvo_renderer_tests.cpp
	)
	set_target_properties(
		memento_lingvo_renderer_tests
		PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
	)
	target_compile_features(memento_lingvo_renderer_tests PRIVATE cxx_std_20)
	target_include_directories(
		memento_lingvo_renderer_tests
		PRIVATE "${PROJECT_SOURCE_DIR}/src"
	)
	target_link_libraries(
		memento_lingvo_renderer_tests
		PRIVATE definition
	)
	add_test(
		NAME memento_lingvo_renderer_tests
		COMMAND memento_lingvo_renderer_tests
	)
	add_executable(memento_lingvo_audio_tests
		tests/lingvo_audio_tests.cpp
		src/audio/lingvolsaaudio.cpp
	)
	target_compile_features(memento_lingvo_audio_tests PRIVATE cxx_std_20)
	target_include_directories(memento_lingvo_audio_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
	target_link_libraries(memento_lingvo_audio_tests PRIVATE Qt6::Core PkgConfig::VORBISFILE)
	qt_add_resources(memento_lingvo_audio_tests "lingvo_audio_test_resource"
		PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/res"
		FILES "${PROJECT_SOURCE_DIR}/res/audio/lingvo_audio_manifest.tsv")
	add_test(NAME memento_lingvo_audio_tests COMMAND memento_lingvo_audio_tests)
endif()
