# Build a separate, bounded read-prefix object set. The application continues
# to use MHP3RD_GENERATED unchanged; only the EXCLUDE_FROM_ALL read oracle links
# these build-local unit 0023/0024 copies.
option(MHP3RD_TEXTURE_READ_BOUNDARIES "Compile state-8 read observation checkpoints" OFF)
set(MHP3RD_TEXTURE_READ_INSTRUMENTATION_INPUTS)
set(MHP3RD_TEXTURE_READ_INSTRUMENTED_SOURCES)
if(MHP3RD_TEXTURE_READ_BOUNDARIES)
    if(NOT MHP3RD_TEXTURE_TRANSFER_BOUNDARIES OR
       NOT MHP3RD_TEXTURE_TRANSFER_INSTRUMENTED_SOURCES)
        message(FATAL_ERROR "mhp3rd G1b-read: read boundaries require G1a transfer boundaries")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    foreach(number 0023 0024)
        set(matches)
        foreach(unit IN LISTS MHP3RD_GENERATED)
            get_filename_component(unit_name "${unit}" NAME)
            if(unit_name STREQUAL "generated_unit_${number}.cpp")
                list(APPEND matches "${unit}")
            endif()
        endforeach()
        list(LENGTH matches match_count)
        if(NOT match_count EQUAL 1)
            message(FATAL_ERROR "mhp3rd G1b-read: transfer-stage unit ${number} missing or duplicated; re-audit the corpus")
        endif()
        list(GET matches 0 input)
        set(copy "${CMAKE_CURRENT_BINARY_DIR}/texture_read_generated/generated_unit_${number}.cpp")
        set(manifest "${copy}.json")
        add_custom_command(
            OUTPUT "${copy}" "${manifest}"
            COMMAND "${Python3_EXECUTABLE}" "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_read.py"
                --input "${input}" --output "${copy}" --manifest "${manifest}"
            DEPENDS "${input}" "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_read.py"
                "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_transfer.py"
                "${MHP3RD_PROFILE_DIR}/tests/texture_read_oracle_stop.hpp"
            COMMENT "Preparing G1b-read state-8 observations ${number}"
            VERBATIM)
        list(APPEND MHP3RD_TEXTURE_READ_INSTRUMENTATION_INPUTS "${input}")
        list(APPEND MHP3RD_TEXTURE_READ_INSTRUMENTED_SOURCES "${copy}")
        set_source_files_properties("${copy}" PROPERTIES
            COMPILE_OPTIONS "${MHP3RD_GENERATED_OPTIONS}"
            INCLUDE_DIRECTORIES "${MHP3RD_PROFILE_DIR}/host;${MHP3RD_PROFILE_DIR}/tests")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_read.py"
        "${MHP3RD_PROFILE_DIR}/tests/texture_read_oracle_stop.hpp")
endif()
