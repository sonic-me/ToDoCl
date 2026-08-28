cmake_minimum_required(VERSION 3.20)
project(TaskTicket LANGUAGES CXX RC)

if (NOT WIN32)
    message(FATAL_ERROR "TaskTicket is a native Win32 application and can only be built for Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(TaskTicket WIN32
    WinMain.cpp
    TaskTicketApp.cpp
    TaskTicketInput.cpp
    Persistence.cpp
    TaskTicket.h
    Resource.h
    TaskTicket.rc
)

target_compile_definitions(TaskTicket PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(TaskTicket PRIVATE
    dwmapi
    shell32
    ole32
    user32
    gdi32
)

# Prefer static CRT linking so the resulting EXE has no extra runtime
# dependencies to install - keeps the "single standalone EXE" goal simple.
if (MSVC)
    set_property(TARGET TaskTicket PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(TaskTicket PROPERTIES
    OUTPUT_NAME "TaskTicket"
)
if (MSVC)
    target_link_options(TaskTicket PRIVATE "/MANIFEST:NO")
endif()
