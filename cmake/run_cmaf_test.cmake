# Drives the CMAF muxer test: builds a multi-track fixture with the ffmpeg CLI,
# then runs the test binary against it.
#
# Prints diagnostics up front so a CI failure is self-explanatory (FFmpeg
# version, available H.264 encoder) rather than an opaque exit code.

find_program(FFMPEG_BIN ffmpeg)
find_program(FFPROBE_BIN ffprobe)

if(NOT FFMPEG_BIN OR NOT FFPROBE_BIN)
    message(STATUS "ffmpeg/ffprobe CLI not found — skipping CMAF test")
    return()
endif()

execute_process(COMMAND ${FFMPEG_BIN} -version
                OUTPUT_VARIABLE ver ERROR_QUIET)
string(REGEX MATCH "^[^\n]*" ver_line "${ver}")
message(STATUS "CMAF test using: ${ver_line}")

# Pick an available H.264 encoder — runners don't all ship the same build.
execute_process(COMMAND ${FFMPEG_BIN} -hide_banner -encoders
                OUTPUT_VARIABLE encoders ERROR_QUIET)
set(VENC "")
foreach(cand libx264 libopenh264 h264_videotoolbox)
    string(FIND "${encoders}" "${cand}" pos)
    if(NOT pos EQUAL -1)
        set(VENC "${cand}")
        break()
    endif()
endforeach()

if(VENC STREQUAL "")
    message(STATUS "no H.264 encoder available in this ffmpeg build "
                   "— skipping CMAF test")
    return()
endif()
message(STATUS "CMAF test H.264 encoder: ${VENC}")

set(SRC "${CMAKE_CURRENT_BINARY_DIR}/cmaf_src.mp4")
set(OUT "${CMAKE_CURRENT_BINARY_DIR}/cmaf_out")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

# 4s: 1 video (keyframe every 2s) + 2 audio tracks.
execute_process(COMMAND ${FFMPEG_BIN} -y -hide_banner
    -f lavfi -i testsrc2=size=320x240:rate=30:duration=4
    -f lavfi -i sine=frequency=440:duration=4
    -f lavfi -i sine=frequency=880:duration=4
    -map 0:v -map 1:a -map 2:a
    -c:v ${VENC} -g 60 -keyint_min 60 -pix_fmt yuv420p
    -c:a aac -shortest ${SRC}
    RESULT_VARIABLE gen_rc
    OUTPUT_VARIABLE gen_out ERROR_VARIABLE gen_err)

if(NOT gen_rc EQUAL 0)
    message(FATAL_ERROR
        "failed to generate CMAF fixture (rc=${gen_rc}):\n${gen_err}")
endif()

execute_process(COMMAND ${FFPROBE_BIN} -v error
                        -show_entries stream=index,codec_type,codec_name
                        -of csv ${SRC}
                OUTPUT_VARIABLE probe ERROR_QUIET)
message(STATUS "fixture streams:\n${probe}")

execute_process(COMMAND ${TEST_EXE} ${SRC} ${OUT}
                RESULT_VARIABLE rc
                OUTPUT_VARIABLE test_out ERROR_VARIABLE test_err)
message(STATUS "${test_out}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "CMAF muxer test failed (rc=${rc}):\n${test_err}${test_out}")
endif()
