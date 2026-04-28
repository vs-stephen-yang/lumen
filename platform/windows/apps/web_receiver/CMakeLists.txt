add_executable(web_receiver
    main.cpp
)

target_link_libraries(web_receiver PRIVATE
    lumen_platform_windows
    lumen_signaling
)
