add_executable(
    uvccapture2
    uvccapture2.cpp
)
# target_link_libraries(
#     uvccapture2
#     ${CMAKE_THREAD_LIBS_INIT}
# )
set_target_properties(uvccapture2 PROPERTIES COMPILE_FLAGS "-std=c++11")

install(
    TARGETS uvccapture2
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
