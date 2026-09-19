cmake_minimum_required(VERSION 3.16)
project(his_gui VERSION 1.0 LANGUAGES CXX)

# A Qt window on `his_monitor --stream`. The monitor does the reading, the
# threading and the verdict; this only draws what it says. Qt 5 Widgets is the
# only dependency -- charts are drawn with QPainter, JSON is QtCore's.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(HIS_WERROR "Treat compiler warnings as errors" OFF)

find_package(Qt5 5.12 REQUIRED COMPONENTS Widgets)

add_executable(his_gui
    src/main.cpp
    src/Protocol.cpp
    src/StreamClient.cpp
    src/MainWindow.cpp
    src/FieldView.cpp
    src/WaveView.cpp
    src/TrendView.cpp
)
target_link_libraries(his_gui PRIVATE Qt5::Widgets)
target_compile_options(his_gui PRIVATE -Wall -Wextra -pedantic)
if(HIS_WERROR)
    target_compile_options(his_gui PRIVATE -Werror)
endif()
