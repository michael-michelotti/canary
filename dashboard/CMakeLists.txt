cmake_minimum_required(VERSION 3.16)

project(canary-dashboard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Mqtt Quick QuickControls2 Graphs)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(canary-dashboard main.cpp)

qt_add_qml_module(canary-dashboard
    URI canarydashboard
    VERSION 1.0
    QML_FILES
        Main.qml
        SensorChart.qml
    SOURCES
        mqttcontroller.h
        mqttcontroller.cpp
)

target_link_libraries(canary-dashboard PRIVATE
    Qt6::Core
    Qt6::Mqtt
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Graphs
)

include(GNUInstallDirs)
install(TARGETS canary-dashboard
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
