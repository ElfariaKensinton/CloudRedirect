cmake_minimum_required(VERSION 3.20)
project(CloudRedirectMacUI VERSION 2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CR_RELEASE_VERSION)
  set(_vp "${CMAKE_CURRENT_SOURCE_DIR}/../Version.props")
  if(EXISTS "${_vp}")
    file(READ "${_vp}" VERSION_PROPS_CONTENT)
    string(REGEX MATCH "<ReleaseVersion>([^<]+)</ReleaseVersion>" _ "${VERSION_PROPS_CONTENT}")
    set(CR_RELEASE_VERSION "${CMAKE_MATCH_1}")
    string(REGEX MATCH "<ReleasePrerelease>([^<]*)</ReleasePrerelease>" _ "${VERSION_PROPS_CONTENT}")
    set(CR_PRERELEASE "${CMAKE_MATCH_1}")
  endif()
endif()
if(NOT CR_RELEASE_VERSION)
  set(CR_RELEASE_VERSION "0.0.0")
endif()
execute_process(COMMAND git rev-parse --short=7 HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GIT_SHA)
  set(GIT_SHA "unknown")
endif()
set(CR_VERSION "${CR_RELEASE_VERSION}${CR_PRERELEASE}+${GIT_SHA}")
find_package(Qt6 REQUIRED COMPONENTS Core Quick QuickControls2 Network)
qt_policy(SET QTP0004 NEW)
qt_add_executable(cloud-redirect-ui MACOSX_BUNDLE
  src/main.cpp
  src/backend.cpp
  src/deployer.cpp
  src/oauthservice.cpp
)
qt_add_qml_module(cloud-redirect-ui
  URI CloudRedirect
  VERSION 1.0
  RESOURCE_PREFIX /
  QML_FILES
    qml/Main.qml
    qml/pages/SetupPage.qml
    qml/pages/DashboardPage.qml
    qml/pages/AppsPage.qml
    qml/pages/BackupsPage.qml
    qml/pages/CloudProviderPage.qml
    qml/pages/StatsPage.qml
    qml/pages/MigrationPage.qml
)
target_include_directories(cloud-redirect-ui PRIVATE src)
target_compile_definitions(cloud-redirect-ui PRIVATE CR_VERSION="${CR_VERSION}")
target_link_libraries(cloud-redirect-ui PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network)
set_target_properties(cloud-redirect-ui PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.cloudredirect.CloudRedirect")
install(TARGETS cloud-redirect-ui BUNDLE DESTINATION .)

add_dependencies(cloud-redirect-ui cloud_redirect cloud_redirect_cli)
add_custom_command(TARGET cloud-redirect-ui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:cloud_redirect>" "$<TARGET_FILE_DIR:cloud-redirect-ui>/cloud_redirect.dylib"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:cloud_redirect_cli>" "$<TARGET_FILE_DIR:cloud-redirect-ui>/cloud_redirect_cli"
)
