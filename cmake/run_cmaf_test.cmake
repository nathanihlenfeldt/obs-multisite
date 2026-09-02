# Generates a 4s multi-track fixture with ffmpeg, then runs the CMAF test on it.
find_program(FFMPEG_BIN ffmpeg)
if(NOT FFMPEG_BIN)
    message(STATUS "ffmpeg CLI not available — skipping CMAF test")
    return()
endif()

set(SRC "${CMAKE_CURRENT_BINARY_DIR}/cmaf_src.mp4")
set(OUT "${CMAKE_CURRENT_BINARY_DIR}/cmaf_out")

execute_process(COMMAND ${FFMPEG_BIN} -y
    -f lavfi -i testsrc2=size=320x240:rate=30:duration=4
    -f lavfi -i sine=frequency=440:duration=4
    -f lavfi -i sine=frequency=880:duration=4
    -map 0:v -map 1:a -map 2:a
    -c:v libx264 -g 60 -keyint_min 60 -sc_threshold 0 -profile:v baseline
    -c:a aac -shortest ${SRC}
    RESULT_VARIABLE gen_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT gen_rc EQUAL 0)
    message(FATAL_ERROR "failed to generate CMAF test fixture")
endif()

execute_process(COMMAND ${TEST_EXE} ${SRC} ${OUT} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "CMAF muxer test failed")
endif()
