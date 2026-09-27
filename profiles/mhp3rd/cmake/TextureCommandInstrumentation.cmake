# Same-unit transfers bypass the runtime's per-PC registration. Instrument a
# build-local copy at the certified builder labels; the game corpus is read-only.
# No mode or callback owner is enabled by this observational seam.
option(MHP3RD_TEXTURE_COMMAND_BOUNDARIES "Compile texture command entry/return boundaries" ON)
set(MHP3RD_TEXTURE_COMMAND_INSTRUMENTED_SOURCE "")
if(MHP3RD_TEXTURE_COMMAND_BOUNDARIES AND MHP3RD_CAMERA_HELPER_UNIT)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(texture_command_units)
    foreach(unit IN LISTS MHP3RD_GENERATED)
        # Probe-generated copies may not exist during a fresh configure and
        # cover other units. A future overlap requires a composed transformer.
        if(unit IN_LIST MHP3RD_PROBE_INSTRUMENTED_SOURCES)
            continue()
        endif()
        file(STRINGS "${unit}" registrations REGEX "register_function\\(0x0889E5C0u,")
        foreach(registration IN LISTS registrations)
            list(APPEND texture_command_units "${unit}")
        endforeach()
    endforeach()
    list(LENGTH texture_command_units unit_count)
    if(NOT unit_count EQUAL 1)
        message(FATAL_ERROR "mhp3rd: texture builder requires exactly one generated registration")
    endif()
    list(GET texture_command_units 0 unit)
    get_filename_component(unit_name "${unit}" NAME)
    set(copy "${CMAKE_CURRENT_BINARY_DIR}/texture_command_generated/${unit_name}")
    set(manifest "${copy}.json")
    add_custom_command(
        OUTPUT "${copy}" "${manifest}"
        COMMAND "${Python3_EXECUTABLE}" "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_commands.py"
            --input "${unit}" --output "${copy}" --manifest "${manifest}"
        DEPENDS "${unit}" "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_commands.py"
        COMMENT "Preparing texture command AOT boundaries ${unit_name}"
        VERBATIM)
    list(REMOVE_ITEM MHP3RD_GENERATED "${unit}")
    list(APPEND MHP3RD_GENERATED "${copy}")
    set_source_files_properties("${copy}" PROPERTIES
        COMPILE_OPTIONS "${MHP3RD_GENERATED_OPTIONS}"
        INCLUDE_DIRECTORIES "${MHP3RD_PROFILE_DIR}/host")
    set(MHP3RD_TEXTURE_COMMAND_INSTRUMENTED_SOURCE "${copy}")
endif()
