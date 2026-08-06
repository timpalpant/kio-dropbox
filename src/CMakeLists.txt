add_library(kio_dropbox_common STATIC
    dropboxaccount.cpp
    dropboxapi.cpp
)
target_include_directories(kio_dropbox_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kio_dropbox_common PUBLIC
    Qt6::Core
    Qt6::Network
    KF6::I18n
    KF6::Wallet
)
set_property(TARGET kio_dropbox_common PROPERTY POSITION_INDEPENDENT_CODE ON)

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

# ---- the authorization helper ------------------------------------------------

add_executable(kio-dropbox-auth
    authhelper/main.cpp
    authhelper/setupdialog.cpp
)
target_link_libraries(kio-dropbox-auth
    kio_dropbox_common
    Qt6::Widgets
    KF6::I18n
    KF6::WidgetsAddons
)

install(TARGETS kio-dropbox-auth DESTINATION ${CMAKE_INSTALL_BINDIR})
