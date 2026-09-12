// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// tile_crop.h — one tile of a composited feed, as a view.
//
// Phase 10 has the encoder declare how it composited a feed and the decoder
// expose each region as its own source. The OBS plugin gets that almost for
// free: obs_source_frame carries a pointer per plane, so a tile is offset
// pointers and a smaller size. The appliance decodes the whole picture and
// hands one frame to an HDMI output, so the same trick has to be spelled out
// once here.
//
// The returned frame is a VIEW, in the sense fix_planes() in video_output.h
// describes: its plane pointers alias the original buffer and its `data` is
// empty. It is valid only while the original frame is, and must only be handed
// to something that reads plane/stride/width/height — an output's present().
// Nothing is copied and nothing is allocated, however many tiles there are.
//
#include "model.h"
#include "cmaf_decoder.h"

namespace multisite {

// The rectangle tile_rect() picks out of `frame`, as a frame that aliases the
// same planes. An absent or out-of-range tile is the whole picture — tile_rect
// guarantees that, and it is the safer answer: something recoverable by hand.
inline DecodedVideoFrame tile_view(const DecodedVideoFrame& frame,
                                   const TileLayout& layout, int index) {
    const TileLayout::Rect r = layout.tile_rect(index, frame.width, frame.height);
    DecodedVideoFrame v;
    v.width      = r.w;
    v.height     = r.h;
    v.pts_ns     = frame.pts_ns;
    v.full_range = frame.full_range;
    v.stride[0]  = frame.stride[0];
    v.stride[1]  = frame.stride[1];
    v.stride[2]  = frame.stride[2];
    // Chroma is half resolution in both directions, which is why tile_rect
    // guarantees even edges: an odd offset has no chroma sample to start from,
    // and the colour would shear away from the luma.
    v.plane[0] = frame.plane[0] + (size_t)r.y * frame.stride[0] + r.x;
    v.plane[1] = frame.plane[1] + (size_t)(r.y / 2) * frame.stride[1] + (r.x / 2);
    v.plane[2] = frame.plane[2] + (size_t)(r.y / 2) * frame.stride[2] + (r.x / 2);
    return v;
}

} // namespace multisite
