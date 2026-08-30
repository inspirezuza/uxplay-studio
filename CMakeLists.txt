cmake_minimum_required(VERSION 3.20)
project(UxPlayStudio VERSION 1.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Core Widgets Network Test REQUIRED)

# UxPlay is vendored so the embedded-renderer API and app always build from
# one reproducible source tree.
add_subdirectory(libuxplay)

set(UXPLAY_STUDIO_CORE_SOURCES
    src/appstate.cpp
    src/receiverconfig.cpp
    src/videosurface.cpp
    src/streamhealth.cpp
)

add_executable(uxplay-studio WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/airplayworker.cpp
    src/receiverengine.cpp
    src/networkdiagnostics.cpp
    src/mdns_responder.cpp
    src/resources.rc
    resources/resources.qrc
    ${UXPLAY_STUDIO_CORE_SOURCES}
)

target_link_libraries(uxplay-studio PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    uxplay
)
target_include_directories(uxplay-studio PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(uxplay-studio PRIVATE
    UXPLAY_STUDIO_VERSION="${PROJECT_VERSION}"
)
if(WIN32)
    target_link_libraries(uxplay-studio PRIVATE Wtsapi32)
endif()

enable_testing()

add_executable(test-appstate tests/test_appstate.cpp src/appstate.cpp)
target_link_libraries(test-appstate PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-appstate PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME appstate COMMAND test-appstate)

add_executable(test-receiverconfig tests/test_receiverconfig.cpp src/receiverconfig.cpp)
target_link_libraries(test-receiverconfig PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-receiverconfig PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME receiverconfig COMMAND test-receiverconfig)

add_executable(test-videosurface tests/test_videosurface.cpp src/videosurface.cpp)
target_link_libraries(test-videosurface PRIVATE Qt6::Core Qt6::Widgets Qt6::Test)
target_include_directories(test-videosurface PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME videosurface COMMAND test-videosurface)

add_executable(test-streamhealth tests/test_streamhealth.cpp src/streamhealth.cpp)
target_link_libraries(test-streamhealth PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-streamhealth PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME streamhealth COMMAND test-streamhealth)

add_executable(test-mirrorrecovery tests/test_mirrorrecovery.cpp)
target_link_libraries(test-mirrorrecovery PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-mirrorrecovery PRIVATE ${CMAKE_SOURCE_DIR}/libuxplay/lib)
add_test(NAME mirrorrecovery COMMAND test-mirrorrecovery)

add_executable(test-mainwindow
    tests/test_mainwindow.cpp
    src/mainwindow.cpp
    src/airplayworker.cpp
    src/receiverengine.cpp
    src/networkdiagnostics.cpp
    src/mdns_responder.cpp
    resources/resources.qrc
    ${UXPLAY_STUDIO_CORE_SOURCES}
)
target_link_libraries(test-mainwindow PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
    uxplay
)
target_include_directories(test-mainwindow PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test-mainwindow PRIVATE
    UXPLAY_STUDIO_VERSION="${PROJECT_VERSION}"
)
if(WIN32)
    target_link_libraries(test-mainwindow PRIVATE Wtsapi32)
endif()
add_test(NAME mainwindow COMMAND test-mainwindow)
