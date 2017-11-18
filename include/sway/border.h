#ifndef _SWAY_BORDER_H
#define _SWAY_BORDER_H
#include "container.h"

/**
 * Border pixel buffer and corresponding geometry.
 */
struct border {
	unsigned char *buffer;
	// TODO WLR
	//struct wlc_geometry geometry;
};

/**
 * Clear border buffer.
 */
void border_clear(struct border *border);

/**
 * Recursively update all of the borders within a container.
 */
void update_container_border(swayc_t *container);

// TODO WLR
//void render_view_borders(wlc_handle view);
int get_font_text_height(const char *font);
bool should_hide_top_border(swayc_t *con, double y);

#endif
