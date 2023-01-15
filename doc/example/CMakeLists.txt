set(example-files
    example
    tutorial-synthetic-ping-pong
    tutorial-synthetic-ping-pong-surrogate
    )

foreach(namefile ${example-files})
    add_executable(${namefile} ${namefile}.c)
    target_link_libraries(${namefile} PUBLIC codes)
endforeach()
