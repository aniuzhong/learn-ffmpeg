cmake_minimum_required(VERSION 3.0)
cmake_policy(VERSION 3.0)

project(learn-ffmpeg C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED yes)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

find_package(FFmpeg COMPONENTS AVCODEC AVFORMAT AVUTIL AVDEVICE AVFILTER SWSCALE SWRESAMPLE REQUIRED)
include_directories(${FFMPEG_INCLUDE_DIRS})

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:wchar_t /D UNICODE")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS")
    add_executable(hw_d3d11va hw_d3d11va.cpp)
    target_link_libraries(hw_d3d11va PRIVATE d3d11.lib ${FFMPEG_LIBRARIES})
endif()
