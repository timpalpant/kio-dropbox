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

# The Dropbox app key this project is registered as.
#
# It sits here in plain sight on purpose. Sign-in uses PKCE, which exists
# precisely because a native application is a "public client" under RFC 8252
# and cannot keep a secret: there is no client secret to pair with this, and the
# key itself is extractable from any binary that ships it. Hiding it would buy
# nothing and cost every builder a setup step.
#
# Override it at configure time with -DDROPBOX_APP_KEY=... or by exporting
# DROPBOX_APP_KEY, which a distribution packager might want in order to get rate
# limits of their own -- Dropbox applies them per app. Users can also enter
# their own key in the setup dialog, with no rebuild.
set(_kio_dropbox_default_app_key "a2yvxn27qepxmi7")
if(NOT "$ENV{DROPBOX_APP_KEY}" STREQUAL "")
    set(_kio_dropbox_default_app_key "$ENV{DROPBOX_APP_KEY}")
endif()
# Note the usual cache semantics: once configured, the cached value wins, so
# changing the environment later needs -DDROPBOX_APP_KEY or a fresh build dir.
set(DROPBOX_APP_KEY "${_kio_dropbox_default_app_key}"
    CACHE STRING "Dropbox app key to build against")

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
