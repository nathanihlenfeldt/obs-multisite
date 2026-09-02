# Optional local fixtures

`test_cmaf_decode` normally runs against the muxer's own output, generated into
the build directory by the `cmaf` test — so encode → decode is exercised on
real bytes without committing large media here.

To additionally check a **captured production segment**, drop a pair of files
from your bucket into this folder and run the test against them directly:

    real_init.mp4    the event's init segment
    real_seg.m4s     any media fragment from the same event

    ./build/test_cmaf_decode test-data/real_init.mp4 test-data/real_seg.m4s

These are intentionally not committed (a fragment is several MB).
