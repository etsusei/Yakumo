# Build a separate completion object set. The application keeps using the
# uninstrumented generated corpus; only the completion oracle links these
# copies and runs past the read-result edge to the retirement boundary.
option(MHP3RD_TEXTURE_COMPLETION_BOUNDARIES
       "Compile selected texture completion checkpoints" OFF)
set(MHP3RD_TEXTURE_COMPLETION_STOP_HEADER
    "${MHP3RD_PROFILE_DIR}/tests/texture_completion_oracle_stop.hpp" CACHE FILEPATH
    "Completion-oracle terminal-stop header")
set(MHP3RD_TEXTURE_COMPLETION_INSTRUMENTED_SOURCES)
if(MHP3RD_TEXTURE_COMPLETION_BOUNDARIES)
    if(NOT MHP3RD_TEXTURE_TRANSFER_BOUNDARIES OR
       NOT MHP3RD_TEXTURE_READ_BOUNDARIES OR
       NOT MHP3RD_TEXTURE_LIFETIME_BOUNDARIES OR
       NOT MHP3RD_TEXTURE_TRANSFER_INSTRUMENTED_SOURCES OR
       NOT MHP3RD_TEXTURE_READ_INSTRUMENTED_SOURCES OR
       NOT MHP3RD_TEXTURE_LIFETIME_INSTRUMENTED_SOURCES)
        message(FATAL_ERROR "mhp3rd G1c: completion boundaries require lifetime, transfer and read boundaries")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(MHP3RD_COMPLETION_GENERATED ${MHP3RD_GENERATED})
    foreach(number 0023 0024)
        set(matches)
        foreach(unit IN LISTS MHP3RD_COMPLETION_GENERATED)
            get_filename_component(unit_name "${unit}" NAME)
            if(unit_name STREQUAL "generated_unit_${number}.cpp")
                list(APPEND matches "${unit}")
            endif()
        endforeach()
        list(LENGTH matches match_count)
        if(NOT match_count EQUAL 1)
            message(FATAL_ERROR "mhp3rd G1c: completion unit ${number} missing or duplicated")
        endif()
        list(GET matches 0 input)
        set(copy "${CMAKE_CURRENT_BINARY_DIR}/texture_completion_generated/generated_unit_${number}.cpp")
        set(manifest "${copy}.json")
        if(number STREQUAL "0024")
            set(tool "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_completion.py")
            set(extra_deps "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_read.py")
        else()
            set(tool "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_read.py")
            set(extra_deps "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_transfer.py")
        endif()
        add_custom_command(
            OUTPUT "${copy}" "${manifest}"
            COMMAND "${Python3_EXECUTABLE}" "${tool}"
                --input "${input}" --output "${copy}" --manifest "${manifest}"
            DEPENDS "${input}" "${tool}" ${extra_deps}
                "${MHP3RD_TEXTURE_COMPLETION_STOP_HEADER}"
            COMMENT "Preparing G1c completion checkpoints ${number}"
            VERBATIM)
        list(REMOVE_ITEM MHP3RD_COMPLETION_GENERATED "${input}")
        list(APPEND MHP3RD_TEXTURE_COMPLETION_INSTRUMENTED_SOURCES "${copy}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_completion.py"
        "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_read.py")
endif()
