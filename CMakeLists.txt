cmake_minimum_required(VERSION 3.0)
cmake_policy(VERSION 3.0)

project(LearnFFmpeg C CXX)

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:wchar_t /D UNICODE")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS")
    add_subdirectory(dxva2_d3d9)
endif()
