#ifndef PD_VIDEO_H
#define PD_VIDEO_H

#include "pd_api.h"

// Composite + blit one VB frame into the Playdate framebuffer.
// Safe to call when no ROM is loaded (no-op).
void pd_video_render_frame(PlaydateAPI *pd);

#endif
