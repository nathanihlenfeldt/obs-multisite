# Drives an HEVC round trip through the CMAF muxer and decoder.
#
# HEVC halves the bandwidth for a given quality and is the codec a Raspberry Pi
# 5 decoder accelerates in hardware (its H.264 block was removed), so it is a
# first-class path rather than a future nicety — and therefore worth a test.
#
# Skips cleanly when the ffmpeg build has no HEVC encoder, as some runners do.

find_program(FFMPEG_BIN ffmpeg)
if(NOT FFMPEG_BIN)
    message(STATUS "ffmpeg CLI not found — skipping HEVC test")
    return()
endif()

execute_process(COMMAND ${FFMPEG_BIN} -hide_banner -encoders
                OUTPUT_VARIABLE encoders ERROR_QUIET)
set(VENC "")
foreach(cand libx265 hevc_videotoolbox)
    string(FIND "${encoders}" "${cand}" pos)
    if(NOT pos EQUAL -1)
        set(VENC "${cand}")
        break()
    endif()
endforeach()

if(VENC STREQUAL "")
    message(STATUS "no HEVC encoder in this ffmpeg build — skipping HEVC test")
    return()
endif()
message(STATUS "HEVC test using encoder: ${VENC}")

set(FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/hevc_src.mp4")
set(OUTDIR  "${CMAKE_CURRENT_BINARY_DIR}/hevc_out")
file(REMOVE_RECURSE "${OUTDIR}")

# Keyframes exactly on the segment interval, and scene-cut disabled, which is
# what the encoder settings do in production.
set(X265_ARGS "keyint=180:min-keyint=180:scenecut=0:log-level=error")
# Two audio tracks, matching the H.264 fixture: the muxer's job here is to
# carry a multi-track production feed, so a single-track fixture would not
# exercise it (and the shared test asserts more than one).
execute_process(
    COMMAND ${FFMPEG_BIN} -y -v error
            -f lavfi -i testsrc2=size=640x360:rate=30:duration=8
            -f lavfi -i sine=frequency=440:duration=8
            -f lavfi -i sine=frequency=880:duration=8
            -map 0:v -map 1:a -map 2:a
            -c:v ${VENC} -x265-params ${X265_ARGS}
            -c:a aac -shortest "${FIXTURE}"
    RESULT_VARIABLE gen_rc ERROR_VARIABLE gen_err)
if(NOT gen_rc EQUAL 0)
    message(STATUS "could not build the HEVC fixture — skipping (${gen_err})")
    return()
endif()

execute_process(COMMAND ${TEST_EXE} "${FIXTURE}" "${OUTDIR}"
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "HEVC CMAF test failed (exit ${rc})")
endif()
