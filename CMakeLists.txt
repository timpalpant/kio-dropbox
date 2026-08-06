cmake_minimum_required(VERSION 3.20)

project(kio-dropbox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Gui Widgets)

# Deliberately no dependency on extra-cmake-modules: the individual KF6 config
# files are self-contained, and skipping ECM keeps the build dependencies down
# to what a KDE desktop already has installed.
find_package(KF6KIO 6.0 REQUIRED)
find_package(KF6CoreAddons 6.0 REQUIRED)
find_package(KF6I18n 6.0 REQUIRED)
find_package(KF6Wallet 6.0 REQUIRED)
find_package(KF6WidgetsAddons 6.0 REQUIRED)

# ECM's KDEInstallDirs would normally provide these.
set(KIO_DROPBOX_PLUGIN_DIR "${CMAKE_INSTALL_LIBDIR}/qt6/plugins"
    CACHE PATH "Where Qt plugins are installed (KIO workers land in kf6/kio below this)")

add_compile_definitions(TRANSLATION_DOMAIN="kio6_dropbox")

add_subdirectory(src)

install(FILES data/dropbox-network.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/remoteview)
install(FILES data/org.kde.kio-dropbox-auth.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

message(STATUS "")
message(STATUS "kio-dropbox ${PROJECT_VERSION}")
message(STATUS "  worker  -> ${CMAKE_INSTALL_PREFIX}/${KIO_DROPBOX_PLUGIN_DIR}/kf6/kio/dropbox.so")
message(STATUS "  helper  -> ${CMAKE_INSTALL_FULL_BINDIR}/kio-dropbox-auth")
message(STATUS "")
