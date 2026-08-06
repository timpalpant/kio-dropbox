# The suite drives the built worker through kioclient against a local stand-in
# for the Dropbox API, so it needs the real plugins on disk rather than a test
# binary linked against the sources. run_tests.sh does that staging itself.
#
# kioclient comes from kde-cli-tools, which is not a build dependency. Without
# it the suite cannot run at all, so register the test only when it is present
# and say so loudly when it is not -- a silently skipped suite is worse than an
# absent one.
find_program(KIOCLIENT_EXECUTABLE NAMES kioclient6 kioclient)
find_package(Python3 COMPONENTS Interpreter)

if(KIOCLIENT_EXECUTABLE AND Python3_Interpreter_FOUND)
    add_test(
        NAME integration
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    # Uploading 20 MiB through a Python reference server is not fast.
    set_tests_properties(integration PROPERTIES TIMEOUT 900)
else()
    # Left as a warning rather than a fatal error so the project still builds
    # for someone who only wants the plugins. It does mean `ctest
    # --no-tests=error` will then fail with "No tests were found", which is the
    # intended outcome for a package build: checkdepends exist to prevent it.
    message(WARNING
        "Integration tests disabled: "
        "kioclient (kde-cli-tools) and Python 3 are both required to run them.")
endif()
