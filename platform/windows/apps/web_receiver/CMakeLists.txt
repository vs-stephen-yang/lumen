add_executable(web_receiver
    main.cpp
)

target_link_libraries(web_receiver PRIVATE
    lumen_platform_windows
    lumen_signaling
)

# Copy the static test page next to the executable so web_receiver can
# serve it without a Vite build. This is for the connectivity check
# only — once S6 lands, the receiver will instead serve web/dist/.
add_custom_command(TARGET web_receiver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/test_page
            $<TARGET_FILE_DIR:web_receiver>/test_page
    COMMENT "Copying web_receiver test page next to the executable"
)
