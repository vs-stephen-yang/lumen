if(WIN32)
    add_executable(capture_encode_test
        capture_encode_test.cpp
    )
    target_include_directories(capture_encode_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    )
    target_link_libraries(capture_encode_test PRIVATE
        lumen_platform_windows
    )

    add_executable(encoder_test
        encoder_test.cpp
    )
    target_include_directories(encoder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    )
    target_link_libraries(encoder_test PRIVATE
        lumen_platform_windows
    )
endif()
