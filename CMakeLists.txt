project(curv)
cmake_minimum_required(VERSION 3.5)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    message(FATAL_ERROR "Do not use 'cmake .'; instead, just type 'make'")
endif ()

execute_process(COMMAND sh -c "${CMAKE_SOURCE_DIR}/cmake/make_version.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

# Configure global include directories, visible in subdirectories.
if (MSYS)
    # Same include directory as in else below BUT without extern/openvdb
    #
    # (On MSYS we use the OpenVDB MinGW package for gettings hands on our
    #  OpenVDB dependency.)
    include_directories(.
        extern/googletest/googletest/include
        extern/double-conversion
        extern/ordered-map/include
        extern/blosc
        extern/stb
        extern/glfw/include
        extern/glm
        extern/glad)
else ()
    include_directories(.
        extern/googletest/googletest/include
        extern/double-conversion
        extern/ordered-map/include
        extern/openvdb
        extern/blosc
        extern/stb
        extern/glfw/include
        extern/glm
        extern/glad)
endif ()

if (APPLE)
    # Using Homebrew on macOS. Search /usr/local/include (bug #28):
    include_directories(SYSTEM /usr/local/include)
    link_directories(/usr/local/lib)
endif (APPLE)

# Libraries
# ===============================================

# REPLxx library
file(GLOB ReplxxSrc "extern/replxx/src/*.cxx" "extern/replxx/src/*.cpp")
add_library(replxx ${ReplxxSrc})
target_include_directories(replxx
   PUBLIC extern/replxx/include
   PRIVATE extern/replxx/src)
set_property(TARGET replxx PROPERTY CXX_STANDARD 14)

# double-conversion library
file(GLOB DoubleConversionSrc "extern/double-conversion/double-conversion/*.cc")
add_library(double-conversion ${DoubleConversionSrc})
set_property(TARGET double-conversion PROPERTY CXX_STANDARD 14)

# glad library, an OpenGL loader
add_library(glad
    extern/glad/glad/glad.c)
set_property(TARGET glad PROPERTY CXX_STANDARD 14)

# Dear IMGUI library
add_library(imgui
    extern/imgui/examples/imgui_impl_glfw.cpp
    extern/imgui/examples/imgui_impl_opengl3.cpp
    extern/imgui/imgui.cpp
    extern/imgui/imgui_demo.cpp
    extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_widgets.cpp)
target_include_directories(imgui
    PUBLIC extern/imgui extern/imgui/examples)
target_link_libraries(imgui PUBLIC glfw glad)
target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_OPENGL_LOADER_GLAD )
set_property(TARGET imgui PROPERTY CXX_STANDARD 14)

# GL library (OpenGL or mesa or ...)
if (APPLE)
    set( sanitize "" )
    FIND_LIBRARY(OPENGL_LIB OpenGL)
    set( LibOpenGL ${OPENGL_LIB} )
elseif (MSYS)
    FIND_LIBRARY(OPENGL_LIB OpenGL NAMES "opengl32.dll.a" HINTS "/mingw64/lib")
    set( LibOpenGL ${OPENGL_LIB} )
else ()
    # The leak sanitizer is only available for 64 bit Linux (gcc and clang).
    # It adds minimal overhead, so we add it to all executables in debug builds.
    set( sanitize "-fsanitize=address" )
    set( LibOpenGL GL )
endif ()

# OpenVDB library
# NB: cannot use find_package(OpenVDB ...) due to https://github.com/AcademySoftwareFoundation/openvdb/issues/412
if (MSYS)
    set( LibOpenVDB "openvdb" )
else ()
    set( LibOpenVDB "openvdb_static" )
endif ()

# Boost library (Windows port required 1.72, 1.65 is latest for Ubuntu 18.04)
find_package(Boost 1.65 REQUIRED COMPONENTS iostreams filesystem system)

# imdemo, broken in v1.65 of ImGui, need to upgrade to headrev
#add_executable(imdemo
#    extern/imgui/examples/example_glfw_opengl3/main.cpp)
#target_include_directories(imdemo
#    PUBLIC extern/imgui/examples/example_glfw_opengl3)
#target_link_libraries(imdemo PUBLIC glfw glad imgui ${LibOpenGL})
#set_property(TARGET imdemo PROPERTY CXX_STANDARD 14)

# Libraries End
# ===============================================


file(GLOB LibCurvSrc "libcurv/*.cc")
add_library(libcurv ${LibCurvSrc})
set_property(TARGET libcurv PROPERTY OUTPUT_NAME curv)

FILE(GLOB LibCurvGeomSrc "libcurv/geom/*.cc" "libcurv/viewer/*.cc")
add_library(libcurv_geom ${LibCurvGeomSrc})
set_property(TARGET libcurv_geom PROPERTY OUTPUT_NAME curv_geom)
target_link_libraries(libcurv_geom PUBLIC imgui)

file(GLOB Src "curv/*.c" "curv/*.cc")
add_executable(curv ${Src})

set(link_libraries_common_to_all_OS libcurv_geom libcurv imgui glfw glad ${LibOpenGL} replxx double-conversion Boost::iostreams Boost::filesystem Boost::system ${LibOpenVDB} Half tbb pthread)

if (MSYS)
    target_link_libraries(curv PUBLIC ${link_libraries_common_to_all_OS})
else ()
    target_link_libraries(curv PUBLIC ${link_libraries_common_to_all_OS} dl)
endif ()

file(GLOB CurvcSrc "curvc/*.cc")
add_executable(curvc EXCLUDE_FROM_ALL ${CurvcSrc})
target_link_libraries(curvc -static libcurv Boost::filesystem Boost::system double-conversion)

file(GLOB TestSrc "tests/*.cc")
add_executable(tester EXCLUDE_FROM_ALL ${TestSrc})
target_link_libraries(tester PUBLIC gtest pthread libcurv libcurv_geom double-conversion Boost::iostreams Boost::filesystem Boost::system)

set_property(TARGET curv curvc libcurv libcurv_geom tester PROPERTY CXX_STANDARD 14)

set(gccflags "-Wall -Wno-unused-result" )
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${gccflags}" )
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${gccflags}" )
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${sanitize} -O1 -Werror" )

add_custom_target(tests tester WORKING_DIRECTORY ../tests)
add_dependencies(tests tester curv)

install(TARGETS curv RUNTIME DESTINATION bin)
install(DIRECTORY lib/curv DESTINATION lib)
install(FILES lib/curv.lang DESTINATION share/gtksourceview-3.0/language-specs)

# Only generate this target if no higher-level project already has
if (NOT TARGET uninstall)
    add_custom_target(uninstall rm -rfv
        "${CMAKE_INSTALL_PREFIX}/bin/curv"
        "${CMAKE_INSTALL_PREFIX}/lib/curv"
        "${CMAKE_INSTALL_PREFIX}/lib/std.curv"
        "${CMAKE_INSTALL_PREFIX}/share/gtksourceview-3.0/language-specs/curv.lang"
    )
endif()

add_subdirectory(extern/googletest/googletest EXCLUDE_FROM_ALL)

if (NOT DEFINED MSYS)
    # Remember that on MSYS we use the OpenVDB MinGW package and *not* obtain it via the Git submodule
    add_subdirectory(extern/openvdb/openvdb EXCLUDE_FROM_ALL)
endif ()

add_subdirectory(extern/glfw EXCLUDE_FROM_ALL)
