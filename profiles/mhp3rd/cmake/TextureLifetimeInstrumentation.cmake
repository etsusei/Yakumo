# Compose lifetime checkpoints after leaf probes; in particular unit 0029
# already contains shared observational copies when certified probes are ON.
option(MHP3RD_TEXTURE_LIFETIME_BOUNDARIES "Compile selected texture lifetime checkpoints" ON)
set(MHP3RD_TEXTURE_LIFETIME_INSTRUMENTED_SOURCES)
if(MHP3RD_TEXTURE_LIFETIME_BOUNDARIES AND MHP3RD_CAMERA_HELPER_UNIT)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    foreach(number 0029 0040 0043 0046)
        set(matches)
        foreach(unit IN LISTS MHP3RD_GENERATED)
            get_filename_component(unit_name "${unit}" NAME)
            if(unit_name STREQUAL "generated_unit_${number}.cpp")
                list(APPEND matches "${unit}")
            endif()
        endforeach()
        list(LENGTH matches match_count)
        if(NOT match_count EQUAL 1)
            message(FATAL_ERROR "mhp3rd: lifetime unit ${number} missing or duplicated; re-audit the corpus")
        endif()
        list(GET matches 0 input)
        set(copy "${CMAKE_CURRENT_BINARY_DIR}/texture_lifetime_generated/generated_unit_${number}.cpp")
        set(manifest "${copy}.json")
        add_custom_command(
            OUTPUT "${copy}" "${manifest}"
            COMMAND "${Python3_EXECUTABLE}" "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_lifetime.py"
                --input "${input}" --output "${copy}" --manifest "${manifest}"
            DEPENDS "${input}" "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_lifetime.py"
            COMMENT "Preparing texture lifetime AOT checkpoints ${number}"
            VERBATIM)
        list(REMOVE_ITEM MHP3RD_GENERATED "${input}")
        list(APPEND MHP3RD_GENERATED "${copy}")
        list(APPEND MHP3RD_TEXTURE_LIFETIME_INSTRUMENTED_SOURCES "${copy}")
        set_source_files_properties("${copy}" PROPERTIES
            COMPILE_OPTIONS "${MHP3RD_GENERATED_OPTIONS}"
            INCLUDE_DIRECTORIES "${MHP3RD_PROFILE_DIR}/host")
    endforeach()
endif()
