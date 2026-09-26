# Instrument build-local copies only. The source corpus remains untouched, and
# unrelated generated units keep their existing compile commands and objects.
option(MHP3RD_CERTIFIED_PROBES "Compile observational boundaries for supported native leaves" ON)
set(MHP3RD_PROBE_INSTRUMENTED_SOURCES)
if(MHP3RD_CERTIFIED_PROBES AND MHP3RD_CAMERA_HELPER_UNIT)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    set(probe_entries 088775AC 08877818 08878B28 08878B4C 08879D08)
    set(found_entries)
    foreach(unit IN LISTS MHP3RD_GENERATED)
        file(STRINGS "${unit}" registrations
            REGEX "register_function\\(0x(088775AC|08877818|08878B28|08878B4C|08879D08)u,")
        if(NOT registrations)
            continue()
        endif()
        set(unit_entries)
        foreach(registration IN LISTS registrations)
            string(REGEX MATCH "0x([0-9A-F]+)u" ignored "${registration}")
            set(entry "${CMAKE_MATCH_1}")
            if(entry IN_LIST found_entries)
                message(FATAL_ERROR "mhp3rd: duplicate probe entry ${entry} in generated corpus")
            endif()
            list(APPEND found_entries "${entry}")
            list(APPEND unit_entries "0x${entry}")
        endforeach()
        list(JOIN unit_entries "," entry_argument)
        get_filename_component(unit_name "${unit}" NAME)
        set(copy "${CMAKE_CURRENT_BINARY_DIR}/probe_generated/${unit_name}")
        set(manifest "${copy}.json")
        add_custom_command(
            OUTPUT "${copy}" "${manifest}"
            COMMAND "${Python3_EXECUTABLE}" "${MHP3RD_PROFILE_DIR}/tools/instrument_probes.py"
                --input "${unit}" --output "${copy}" --entries "${entry_argument}" --manifest "${manifest}"
            DEPENDS "${unit}" "${MHP3RD_PROFILE_DIR}/tools/instrument_probes.py"
            COMMENT "Preparing observational AOT copy ${unit_name}"
            VERBATIM)
        list(REMOVE_ITEM MHP3RD_GENERATED "${unit}")
        list(APPEND MHP3RD_PROBE_INSTRUMENTED_SOURCES "${copy}")
        set_source_files_properties("${copy}" PROPERTIES
            COMPILE_OPTIONS "${MHP3RD_GENERATED_OPTIONS}"
            INCLUDE_DIRECTORIES "${MHP3RD_PROFILE_DIR}/host")
    endforeach()
    foreach(entry IN LISTS probe_entries)
        if(NOT entry IN_LIST found_entries)
            message(FATAL_ERROR "mhp3rd: generated corpus is missing probe entry ${entry}")
        endif()
    endforeach()
    list(APPEND MHP3RD_GENERATED ${MHP3RD_PROBE_INSTRUMENTED_SOURCES})
endif()
