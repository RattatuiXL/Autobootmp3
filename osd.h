#ifndef AUTOMUSIC_OSD_H
#define AUTOMUSIC_OSD_H

void osd_draw(void *topaddr, int bufferwidth, int pixelformat,
              const char *title, unsigned int elapsed_frames,
              unsigned int total_frames, int playing,
              unsigned int animation_phase, unsigned int opacity);

#endif
