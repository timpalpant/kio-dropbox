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
find_package(KF6KCMUtils 6.0 REQUIRED)
find_package(KF6ConfigWidgets 6.0 REQUIRED)
find_package(KF6WidgetsAddons 6.0 REQUIRED)

# A Dropbox app key baked into the build, so users don't have to register an app
# of their own. The key is not a secret -- PKCE exists precisely because native
# apps are public clients that cannot keep one (RFC 8252) -- but Dropbox caps an
# unapproved app at 50 linked users and rate-limits per app, so a packaged build
# wants a key that has been through Dropbox's production approval.
# Leave empty to make every user supply their own.
set(DROPBOX_APP_KEY "" CACHE STRING "Dropbox app key to bake into the build (optional)")

# ECM's KDEInstallDirs would normally provide these.
set(KIO_DROPBOX_PLUGIN_DIR "${CMAKE_INSTALL_LIBDIR}/qt6/plugins"
    CACHE PATH "Where Qt plugins are installed (KIO workers land in kf6/kio below this)")

add_compile_definitions(TRANSLATION_DOMAIN="kio6_dropbox")

option(BUILD_TESTING "Build and register the integration test suite" ON)

add_subdirectory(src)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

install(FILES data/dropbox-network.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/remoteview)
install(FILES data/io.github.timpalpant.kio-dropbox.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.timpalpant.kio-dropbox.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/io.github.timpalpant.kio-dropbox.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# --- packaging ----------------------------------------------------------------

set(CPACK_PACKAGE_NAME "kio-dropbox")
set(CPACK_PACKAGE_VENDOR "Timothy Palpant")
set(CPACK_PACKAGE_CONTACT "Timothy Palpant <tim@palpant.us>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Browse Dropbox as dropbox:/ in Dolphin")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_REQUIRES "kf6-kio, kf6-kwallet, kf6-kcmutils")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
include(CPack)

message(STATUS "")
message(STATUS "kio-dropbox ${PROJECT_VERSION}")
message(STATUS "  worker  -> ${CMAKE_INSTALL_PREFIX}/${KIO_DROPBOX_PLUGIN_DIR}/kf6/kio/dropbox.so")
message(STATUS "  helper  -> ${CMAKE_INSTALL_FULL_BINDIR}/kio-dropbox-auth")
message(STATUS "  kcm     -> ${CMAKE_INSTALL_PREFIX}/${KIO_DROPBOX_PLUGIN_DIR}/plasma/kcms/systemsettings_qwidgets/kcm_dropbox.so")
message(STATUS "")
