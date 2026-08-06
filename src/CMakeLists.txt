# Account handling and the REST client. Deliberately free of any GUI or KIO
# dependency, so the worker, the helper and the KCM can all share it.
add_library(kio_dropbox_common STATIC
    dropboxaccount.cpp
    dropboxapi.cpp
)
target_include_directories(kio_dropbox_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(kio_dropbox_common PUBLIC DROPBOX_BUILTIN_APP_KEY="${DROPBOX_APP_KEY}")
target_link_libraries(kio_dropbox_common PUBLIC
    Qt6::Core
    Qt6::Network
    KF6::I18n
    KF6::Wallet
)
set_property(TARGET kio_dropbox_common PROPERTY POSITION_INDEPENDENT_CODE ON)

# The account-linking UI, shared by the standalone helper and the KCM.
add_library(kio_dropbox_setup STATIC
    gui/setupwidget.cpp
    gui/dropboxplaces.cpp
)
target_include_directories(kio_dropbox_setup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gui)
target_link_libraries(kio_dropbox_setup PUBLIC
    kio_dropbox_common
    Qt6::Widgets
    KF6::KIOFileWidgets
    KF6::WidgetsAddons
    KF6::ConfigWidgets
)
set_property(TARGET kio_dropbox_setup PROPERTY POSITION_INDEPENDENT_CODE ON)

# ---- the KIO worker ----------------------------------------------------------

add_library(dropbox MODULE kio_dropbox.cpp)
target_link_libraries(dropbox
    kio_dropbox_common
    KF6::KIOCore
    KF6::CoreAddons
    KF6::I18n
)
# KIO's loader dlopen()s the module and calls kdemain(); nothing else is exported.
set_target_properties(dropbox PROPERTIES PREFIX "")

install(TARGETS dropbox
        DESTINATION ${KIO_DROPBOX_PLUGIN_DIR}/kf6/kio)

# ---- the standalone authorization helper -------------------------------------

add_executable(kio-dropbox-auth authhelper/main.cpp)
target_link_libraries(kio-dropbox-auth kio_dropbox_setup)

install(TARGETS kio-dropbox-auth DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- the System Settings module ----------------------------------------------

add_library(kcm_dropbox MODULE kcm/dropboxkcm.cpp)
target_link_libraries(kcm_dropbox
    kio_dropbox_setup
    KF6::KCMUtils
)
set_target_properties(kcm_dropbox PROPERTIES PREFIX "")

install(TARGETS kcm_dropbox
        DESTINATION ${KIO_DROPBOX_PLUGIN_DIR}/plasma/kcms/systemsettings_qwidgets)
