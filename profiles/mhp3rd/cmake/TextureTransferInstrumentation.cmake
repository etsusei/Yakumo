# Add build-local transfer observations after the existing lifetime
# instrumentation. Enable this only for the bounded G1a oracle, not normal
# application delivery.
option(MHP3RD_TEXTURE_TRANSFER_BOUNDARIES "Compile selected texture transfer checkpoints" OFF)
set(MHP3RD_TEXTURE_TRANSFER_INSTRUMENTED_SOURCES)
if(MHP3RD_TEXTURE_TRANSFER_BOUNDARIES)
    if(NOT MHP3RD_TEXTURE_LIFETIME_BOUNDARIES OR
       NOT MHP3RD_TEXTURE_LIFETIME_INSTRUMENTED_SOURCES)
        message(FATAL_ERROR "mhp3rd G1a: transfer boundaries require lifetime boundaries")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    foreach(number 0023 0040)
        set(matches)
        foreach(unit IN LISTS MHP3RD_GENERATED)
            get_filename_component(unit_name "${unit}" NAME)
            if(unit_name STREQUAL "generated_unit_${number}.cpp")
                list(APPEND matches "${unit}")
            endif()
        endforeach()
        list(LENGTH matches match_count)
        if(NOT match_count EQUAL 1)
            message(FATAL_ERROR "mhp3rd G1a: transfer unit ${number} missing or duplicated; re-audit the corpus")
        endif()
        list(GET matches 0 input)
        set(copy "${CMAKE_CURRENT_BINARY_DIR}/texture_transfer_generated/generated_unit_${number}.cpp")
        set(manifest "${copy}.json")
        add_custom_command(
            OUTPUT "${copy}" "${manifest}"
            COMMAND "${Python3_EXECUTABLE}" "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_transfer.py"
                --input "${input}" --output "${copy}" --manifest "${manifest}"
            DEPENDS "${input}" "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_transfer.py"
            COMMENT "Preparing G1a load/enqueue transfer checkpoints ${number}"
            VERBATIM)
        list(REMOVE_ITEM MHP3RD_GENERATED "${input}")
        list(APPEND MHP3RD_GENERATED "${copy}")
        list(APPEND MHP3RD_TEXTURE_TRANSFER_INSTRUMENTED_SOURCES "${copy}")
        set_source_files_properties("${copy}" PROPERTIES
            COMPILE_OPTIONS "${MHP3RD_GENERATED_OPTIONS}"
            INCLUDE_DIRECTORIES "${MHP3RD_PROFILE_DIR}/host")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${MHP3RD_PROFILE_DIR}/tools/instrument_texture_transfer.py")
endif()
