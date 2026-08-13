cmake_minimum_required(VERSION 3.20)
project(UxPlayStudio VERSION 2.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Core Widgets Network Test REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)

# UxPlay is vendored so the embedded-renderer API and app always build from
# one reproducible source tree.
add_subdirectory(libuxplay)

set(UXPLAY_STUDIO_CORE_SOURCES
    src/appstate.cpp
    src/receiverconfig.cpp
    src/videosurface.cpp
    src/studio/scenedocument.cpp
    src/studio/scenecanvas.cpp
    src/studio/cameraselfview.cpp
    src/ui/studiovisuals.cpp
    src/projects/projectstore.cpp
    src/recording/sessionclock.cpp
    src/recording/recordingsession.cpp
    src/recording/gstpipelinerunner.cpp
    src/recording/camerapreviewengine.cpp
    src/export/exportpipeline.cpp
    src/export/exportjob.cpp
)

function(require_uxplay_static_runtime target)
    # Clean rebuilds on Windows can start a dependent link step before the
    # vendored static archives settle unless the graph is explicit.
    add_dependencies(${target} uxplay renderers airplay)
endfunction()

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
    PkgConfig::CAIRO
)
require_uxplay_static_runtime(uxplay-studio)
target_include_directories(uxplay-studio PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(uxplay-studio PRIVATE
    UXPLAY_STUDIO_VERSION="${PROJECT_VERSION}"
)

enable_testing()

add_executable(test-appstate tests/test_appstate.cpp src/appstate.cpp)
target_link_libraries(test-appstate PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-appstate PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME appstate COMMAND test-appstate)

add_executable(test-receiverconfig tests/test_receiverconfig.cpp src/receiverconfig.cpp)
target_link_libraries(test-receiverconfig PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-receiverconfig PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME receiverconfig COMMAND test-receiverconfig)

add_executable(test-receiverengine
    tests/test_receiverengine.cpp
    src/airplayworker.cpp
    src/receiverengine.cpp
    src/appstate.cpp
    src/receiverconfig.cpp
)
target_link_libraries(test-receiverengine PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_include_directories(test-receiverengine PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME receiverengine COMMAND test-receiverengine)

add_executable(test-camerapreview
    tests/test_camerapreview.cpp
    src/recording/camerapreviewengine.cpp
    src/studio/cameraselfview.cpp
)
target_link_libraries(test-camerapreview PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test uxplay)
require_uxplay_static_runtime(test-camerapreview)
target_include_directories(test-camerapreview PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME camerapreview COMMAND test-camerapreview)

add_executable(test-videosurface tests/test_videosurface.cpp src/videosurface.cpp)
target_link_libraries(test-videosurface PRIVATE Qt6::Core Qt6::Widgets Qt6::Test uxplay)
require_uxplay_static_runtime(test-videosurface)
target_include_directories(test-videosurface PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME videosurface COMMAND test-videosurface)

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
    PkgConfig::CAIRO
)
require_uxplay_static_runtime(test-mainwindow)
target_include_directories(test-mainwindow PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test-mainwindow PRIVATE
    UXPLAY_STUDIO_VERSION="${PROJECT_VERSION}"
)
add_test(NAME mainwindow COMMAND test-mainwindow)

add_executable(test-scenedocument
    tests/test_scenedocument.cpp
    src/studio/scenedocument.cpp
)
target_link_libraries(test-scenedocument PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-scenedocument PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME scenedocument COMMAND test-scenedocument)

add_executable(test-projectstore
    tests/test_projectstore.cpp
    src/projects/projectstore.cpp
    src/studio/scenedocument.cpp
)
target_link_libraries(test-projectstore PRIVATE Qt6::Core Qt6::Test uxplay)
require_uxplay_static_runtime(test-projectstore)
target_include_directories(test-projectstore PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME projectstore COMMAND test-projectstore)

add_executable(test-scenecanvas
    tests/test_scenecanvas.cpp
    src/studio/scenecanvas.cpp
    src/studio/scenedocument.cpp
)
target_link_libraries(test-scenecanvas PRIVATE Qt6::Core Qt6::Widgets Qt6::Test)
target_include_directories(test-scenecanvas PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME scenecanvas COMMAND test-scenecanvas)

add_executable(test-sessionclock tests/test_sessionclock.cpp src/recording/sessionclock.cpp)
target_link_libraries(test-sessionclock PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-sessionclock PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME sessionclock COMMAND test-sessionclock)

add_executable(test-recordingsession
    tests/test_recordingsession.cpp
    src/recording/recordingsession.cpp
    src/recording/gstpipelinerunner.cpp
    src/projects/projectstore.cpp
    src/studio/scenedocument.cpp
)
target_link_libraries(test-recordingsession PRIVATE Qt6::Core Qt6::Gui Qt6::Test uxplay)
require_uxplay_static_runtime(test-recordingsession)
target_include_directories(test-recordingsession PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME recordingsession COMMAND test-recordingsession)

add_executable(test-mux-handoff tests/test_mux_handoff.cpp)
target_link_libraries(test-mux-handoff PRIVATE Qt6::Core Qt6::Test uxplay)
require_uxplay_static_runtime(test-mux-handoff)
target_include_directories(test-mux-handoff PRIVATE ${CMAKE_SOURCE_DIR}/libuxplay)
add_test(NAME mux-handoff COMMAND test-mux-handoff)

add_executable(test-exportpipeline
    tests/test_exportpipeline.cpp
    src/export/exportpipeline.cpp
    src/projects/projectstore.cpp
    src/studio/scenedocument.cpp
)
target_link_libraries(test-exportpipeline PRIVATE Qt6::Core Qt6::Gui Qt6::Test uxplay)
require_uxplay_static_runtime(test-exportpipeline)
target_include_directories(test-exportpipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME exportpipeline COMMAND test-exportpipeline)

add_executable(test-exportjob
    tests/test_exportjob.cpp
    src/export/exportjob.cpp
    src/export/exportpipeline.cpp
    src/projects/projectstore.cpp
    src/studio/scenedocument.cpp
)
target_link_libraries(test-exportjob PRIVATE Qt6::Core Qt6::Gui Qt6::Test uxplay PkgConfig::CAIRO)
require_uxplay_static_runtime(test-exportjob)
target_include_directories(test-exportjob PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME exportjob COMMAND test-exportjob)
