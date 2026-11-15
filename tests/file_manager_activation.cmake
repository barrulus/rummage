# Exercise the daemon's real keyfile parser and Exec argv parser, with no user session.
if(INNER)
    foreach(method ShowItems ShowFolders ShowItemProperties)
        execute_process(COMMAND "${DBUS_SEND}" --session --print-reply --reply-timeout=5000
            --dest=org.freedesktop.FileManager1 /org/freedesktop/FileManager1
            "org.freedesktop.FileManager1.${method}"
            "array:string:file:///nonexistent-rummage-activation-fixture" "string:"
            RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
        if(NOT "${status}" STREQUAL "0" OR NOT output MATCHES "method return")
            message(FATAL_ERROR "${method} activation failed: ${status}: ${output}${error}")
        endif()
    endforeach()
    return()
endif()

file(MAKE_DIRECTORY "${TEST_DIRECTORY}")
foreach(directory config data state cache runtime)
    file(MAKE_DIRECTORY "${TEST_DIRECTORY}/${directory}")
endforeach()
file(CHMOD "${TEST_DIRECTORY}/runtime" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(ENV{XDG_CONFIG_HOME} "${TEST_DIRECTORY}/config")
set(ENV{XDG_DATA_HOME} "${TEST_DIRECTORY}/data")
set(ENV{XDG_STATE_HOME} "${TEST_DIRECTORY}/state")
set(ENV{XDG_CACHE_HOME} "${TEST_DIRECTORY}/cache")
set(ENV{XDG_RUNTIME_DIR} "${TEST_DIRECTORY}/runtime")
set(ENV{DBUS_SESSION_BUS_ADDRESS} "unix:path=${TEST_DIRECTORY}/no-user-bus")
set(ENV{WAYLAND_DISPLAY} "rummage-no-display")
set(special_directory [=[space "quote" back\slash $dollar `tick`]=])
set(service "${TEST_DIRECTORY}/data/dbus-1/services/org.freedesktop.FileManager1.service")
# CMake's file() path helpers normalize literal backslashes into separators. Use
# argv-only POSIX operations for this deliberately unusual Linux filename.
find_program(MAKE_DIRECTORY_PROGRAM mkdir REQUIRED)
find_program(COPY_PROGRAM cp REQUIRED)
foreach(directory plain "${special_directory}")
    set(program_directory "${TEST_DIRECTORY}/${directory}")
    get_filename_component(program_name "${RUMMAGE_EXECUTABLE}" NAME)
    set(program "${program_directory}/${program_name}")
    execute_process(COMMAND "${MAKE_DIRECTORY_PROGRAM}" -p -- "${program_directory}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${COPY_PROGRAM}" -- "${RUMMAGE_EXECUTABLE}" "${program}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${program}" --set-file-manager
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    if(NOT "${status}" STREQUAL "0" OR NOT EXISTS "${service}")
        message(FATAL_ERROR "Temporary setup failed: ${status}: ${output}${error}")
    endif()
    execute_process(COMMAND "${DBUS_RUN_SESSION}" -- "${CMAKE_COMMAND}"
        -DINNER=ON "-DDBUS_SEND=${DBUS_SEND}" -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    if(NOT "${status}" STREQUAL "0")
        file(READ "${service}" contents)
        message(FATAL_ERROR "Automatic activation failed: ${status}: ${output}${error}\n${contents}")
    endif()
    execute_process(COMMAND "${program}" --unset-file-manager
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    if(NOT "${status}" STREQUAL "0" OR EXISTS "${service}")
        message(FATAL_ERROR "Temporary removal failed: ${status}: ${output}${error}")
    endif()
endforeach()
# dbus-run-session stops the private daemon when the inner command exits; the service
# observes connection loss and quits immediately rather than waiting for its idle timer.
file(REMOVE_RECURSE "${TEST_DIRECTORY}")
