add_executable(web_receiver
    main.cpp
)

target_link_libraries(web_receiver PRIVATE
    lumen_platform_windows
    lumen_signaling
)

# Deploy the sender web root next to the executable. Prefers the
# productionized Vite build (web/dist) when it exists, else falls back to the
# legacy inline test page. Served at /sender/ by the signaling server.
add_custom_command(TARGET web_receiver POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DWEB_DIST=${CMAKE_SOURCE_DIR}/web/examples/screen-share/dist
            -DTEST_PAGE=${CMAKE_CURRENT_SOURCE_DIR}/test_page
            -DOUT_DIR=$<TARGET_FILE_DIR:web_receiver>/test_page
            -P ${CMAKE_CURRENT_SOURCE_DIR}/deploy_web_root.cmake
    COMMENT "Deploying web_receiver sender root (web SDK example build if present, else test_page)"
)
