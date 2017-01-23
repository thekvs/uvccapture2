include_directories(third_party)

set(SRC
    uvccapture2.cpp
)

add_executable(
    uvccapture2
    ${SRC}
)
target_link_libraries(
    uvccapture2
    ${LIBJPEG_LIBRARIES}
)
set_target_properties(uvccapture2 PROPERTIES COMPILE_FLAGS "-std=c++11")
target_compile_definitions(uvccapture2 PRIVATE -DELPP_DISABLE_DEFAULT_CRASH_HANDLING)
# tell easylogging++ library not to create logfile
target_compile_definitions(uvccapture2 PRIVATE -DELPP_NO_DEFAULT_LOG_FILE)

install(
    TARGETS uvccapture2
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
