#ifndef PD_VIDEO_H
#define PD_VIDEO_H

#include "pd_api.h"

// Split into two functions so the caller can time them independently.
//
// pd_video_vip_step:   runs the Red Viper software VIP renderer
//                      (update_texture_cache_soft + video_soft_render).
//                      Writes a 384x224 2bpp framebuffer into V810_DISPLAY_RAM.
// pd_video_blit:       reads that framebuffer, threshold-converts to 1bpp,
//                      blits centered into the Playdate display.
//
// Both are safe no-ops if vb_state is NULL.
void pd_video_vip_step(PlaydateAPI *pd);
void pd_video_blit(PlaydateAPI *pd);

// Request that the next blit refreshes the WHOLE display, not just the VB
// area. Needed when something else drew the screen (ROM picker, menus): the
// blit normally marks only rows 8..231 as updated, so stale pixels in the
// 8 px margins would otherwise stay on the panel forever.
void pd_video_request_full_redraw(void);

#endif
