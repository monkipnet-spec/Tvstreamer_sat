option(TVSTREAMMERSAT5_BUILD_OSCAM_MINI "Build minimal OSCam helper" ON)
set(OSCAM_MINI_GIT_REPOSITORY "https://github.com/gfto/oscam.git" CACHE STRING "OSCam repository")
set(OSCAM_MINI_GIT_REVISION "2780c48789c8e1427df4078ea9b06e0b51594bbc" CACHE STRING "Pinned OSCam revision")

if(TVSTREAMMERSAT5_BUILD_OSCAM_MINI)
    find_program(GIT_EXECUTABLE git REQUIRED)

    set(OSCAM_MINI_OUTPUT_DIR "${CMAKE_BINARY_DIR}/oscam-mini")
    set(OSCAM_MINI_BINARY "${OSCAM_MINI_OUTPUT_DIR}/oscam-mini")

    add_custom_command(
        OUTPUT "${OSCAM_MINI_BINARY}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${OSCAM_MINI_OUTPUT_DIR}"
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/build_oscam_mini.sh"
                "${OSCAM_MINI_OUTPUT_DIR}"
                "${OSCAM_MINI_GIT_REPOSITORY}"
                "${OSCAM_MINI_GIT_REVISION}"
        DEPENDS "${CMAKE_SOURCE_DIR}/scripts/build_oscam_mini.sh"
        COMMENT "Building OSCam-mini (Newcamd + Phoenix + Irdeto + Viaccess)"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_target(oscam-mini ALL DEPENDS "${OSCAM_MINI_BINARY}")

    install(PROGRAMS "${OSCAM_MINI_BINARY}" DESTINATION /opt/TVStreammerSAT5/oscam-mini)
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/scripts/install_oscam_mini.sh" DESTINATION /opt/TVStreammerSAT5/oscam-mini)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/oscam-mini/oscam-mini.service" DESTINATION /opt/TVStreammerSAT5/oscam-mini)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/packaging/oscam-mini/config/" DESTINATION /opt/TVStreammerSAT5/oscam-mini/config)
endif()
