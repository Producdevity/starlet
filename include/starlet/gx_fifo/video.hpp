#pragma once

namespace starlet::gx_fifo {

// The video interface (VI SDK calls): a 59.94 Hz NTSC retrace interrupt, the frame buffer the next
// retrace shows, and the retrace callbacks.

// What a retrace shows: the external frame buffer the game flushed, or black.
struct VideoFrame {
    void* frame_buffer;
    bool black;
    unsigned width, height;
    unsigned retrace;
};

// Called on the guest CPU at each retrace, after the frame buffer registers latch; it hands
// presentation to the renderer and must not block. Ports using this module's GPU pass present
// (gpu.hpp). Without a presenter, retraces show nothing.
using VideoPresenter = void (*)(const VideoFrame&);
void set_video_presenter(VideoPresenter presenter);

}
