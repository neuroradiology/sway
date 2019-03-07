#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include "list.h"
#include "log.h"
#include "swaybar/bar.h"
#include "swaybar/config.h"
#include "swaybar/input.h"
#include "swaybar/ipc.h"

void free_hotspots(struct wl_list *list) {
	struct swaybar_hotspot *hotspot, *tmp;
	wl_list_for_each_safe(hotspot, tmp, list, link) {
		wl_list_remove(&hotspot->link);
		if (hotspot->destroy) {
			hotspot->destroy(hotspot->data);
		}
		free(hotspot);
	}
}

uint32_t event_to_x11_button(uint32_t event) {
	switch (event) {
	case BTN_LEFT:
		return 1;
	case BTN_MIDDLE:
		return 2;
	case BTN_RIGHT:
		return 3;
	case SWAY_SCROLL_UP:
		return 4;
	case SWAY_SCROLL_DOWN:
		return 5;
	case SWAY_SCROLL_LEFT:
		return 6;
	case SWAY_SCROLL_RIGHT:
		return 7;
	case BTN_SIDE:
		return 8;
	case BTN_EXTRA:
		return 9;
	default:
		return 0;
	}
}

static uint32_t wl_axis_to_button(uint32_t axis, wl_fixed_t value) {
	bool negative = wl_fixed_to_double(value) < 0;
	switch (axis) {
	case WL_POINTER_AXIS_VERTICAL_SCROLL:
		return negative ? SWAY_SCROLL_UP : SWAY_SCROLL_DOWN;
	case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
		return negative ? SWAY_SCROLL_LEFT : SWAY_SCROLL_RIGHT;
	default:
		sway_log(SWAY_DEBUG, "Unexpected axis value on mouse scroll");
		return 0;
	}
}

void update_cursor(struct swaybar *bar) {
	struct swaybar_pointer *pointer = &bar->pointer;
	if (pointer->cursor_theme) {
		wl_cursor_theme_destroy(pointer->cursor_theme);
	}
	int scale = pointer->current ? pointer->current->scale : 1;
	pointer->cursor_theme = wl_cursor_theme_load(NULL, 24 * scale, bar->shm);
	struct wl_cursor *cursor;
	cursor = wl_cursor_theme_get_cursor(pointer->cursor_theme, "left_ptr");
	pointer->cursor_image = cursor->images[0];
	wl_surface_set_buffer_scale(pointer->cursor_surface, scale);
	wl_surface_attach(pointer->cursor_surface,
			wl_cursor_image_get_buffer(pointer->cursor_image), 0, 0);
	wl_pointer_set_cursor(pointer->pointer, pointer->serial,
			pointer->cursor_surface,
			pointer->cursor_image->hotspot_x / scale,
			pointer->cursor_image->hotspot_y / scale);
	wl_surface_damage_buffer(pointer->cursor_surface, 0, 0,
			INT32_MAX, INT32_MAX);
	wl_surface_commit(pointer->cursor_surface);
}

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct swaybar *bar = data;
	struct swaybar_pointer *pointer = &bar->pointer;
	pointer->serial = serial;
	struct swaybar_output *output;
	wl_list_for_each(output, &bar->outputs, link) {
		if (output->surface == surface) {
			pointer->current = output;
			break;
		}
	}
	update_cursor(bar);
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct swaybar *bar = data;
	bar->pointer.current = NULL;
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct swaybar *bar = data;
	bar->pointer.x = wl_fixed_to_int(surface_x);
	bar->pointer.y = wl_fixed_to_int(surface_y);
}

static bool check_bindings(struct swaybar *bar, uint32_t button,
		uint32_t state) {
	bool released = state == WL_POINTER_BUTTON_STATE_RELEASED;
	for (int i = 0; i < bar->config->bindings->length; i++) {
		struct swaybar_binding *binding = bar->config->bindings->items[i];
		if (binding->button == button && binding->release == released) {
			ipc_execute_binding(bar, binding);
			return true;
		}
	}
	return false;
}

static void process_hotspots(struct swaybar_output *output,
		double x, double y, uint32_t button) {
	x *= output->scale;
	y *= output->scale;
	struct swaybar_hotspot *hotspot;
	wl_list_for_each(hotspot, &output->hotspots, link) {
		if (x >= hotspot->x && y >= hotspot->y
				&& x < hotspot->x + hotspot->width
				&& y < hotspot->y + hotspot->height) {
			if (HOTSPOT_IGNORE == hotspot->callback(output, hotspot,
					x / output->scale, y / output->scale, button, hotspot->data)) {
				return;
			}
		}
	}
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct swaybar *bar = data;
	struct swaybar_pointer *pointer = &bar->pointer;
	struct swaybar_output *output = pointer->current;
	if (!sway_assert(output, "button with no active output")) {
		return;
	}

	if (check_bindings(bar, button, state)) {
		return;
	}

	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}
	process_hotspots(output, pointer->x, pointer->y, button);
}

static void workspace_next(struct swaybar *bar, struct swaybar_output *output,
		bool left) {
	struct swaybar_config *config = bar->config;
	struct swaybar_workspace *first =
		wl_container_of(output->workspaces.next, first, link);
	struct swaybar_workspace *last =
		wl_container_of(output->workspaces.prev, last, link);

	struct swaybar_workspace *active;
	wl_list_for_each(active, &output->workspaces, link) {
		if (active->visible) {
			break;
		}
	}
	if (!sway_assert(active->visible, "axis with null workspace")) {
		return;
	}

	struct swaybar_workspace *new;
	if (left) {
		if (active == first) {
			new = config->wrap_scroll ? last : NULL;
		} else {
			new = wl_container_of(active->link.prev, new, link);
		}
	} else {
		if (active == last) {
			new = config->wrap_scroll ? first : NULL;
		} else {
			new = wl_container_of(active->link.next, new, link);
		}
	}

	if (new) {
		ipc_send_workspace_command(bar, new->name);
	}
}

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	struct swaybar *bar = data;
	struct swaybar_pointer *pointer = &bar->pointer;
	struct swaybar_output *output = pointer->current;
	if (!sway_assert(output, "axis with no active output")) {
		return;
	}

	// If there is a button press binding, execute it, skip default behavior,
	// and check button release bindings
	uint32_t button = wl_axis_to_button(axis, value);
	if (check_bindings(bar, button, WL_POINTER_BUTTON_STATE_PRESSED)) {
		check_bindings(bar, button, WL_POINTER_BUTTON_STATE_RELEASED);
		return;
	}

	struct swaybar_hotspot *hotspot;
	wl_list_for_each(hotspot, &output->hotspots, link) {
		double x = pointer->x * output->scale;
		double y = pointer->y * output->scale;
		if (x >= hotspot->x
				&& y >= hotspot->y
				&& x < hotspot->x + hotspot->width
				&& y < hotspot->y + hotspot->height) {
			if (HOTSPOT_IGNORE == hotspot->callback(output, hotspot,
					pointer->x, pointer->y, button, hotspot->data)) {
				return;
			}
		}
	}

	struct swaybar_config *config = bar->config;
	double amt = wl_fixed_to_double(value);
	if (amt == 0.0 || !config->workspace_buttons) {
		check_bindings(bar, button, WL_POINTER_BUTTON_STATE_RELEASED);
		return;
	}

	if (!sway_assert(!wl_list_empty(&output->workspaces), "axis with no workspaces")) {
		return;
	}

	workspace_next(bar, output, amt < 0.0);

	// Check button release bindings
	check_bindings(bar, button, WL_POINTER_BUTTON_STATE_RELEASED);
}

static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
	// Who cares
}

static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis_source) {
	// Who cares
}

static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis) {
	// Who cares
}

static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis, int32_t discrete) {
	// Who cares
}

static struct wl_pointer_listener pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete,
};

static struct touch_slot *get_touch_slot(struct swaybar_touch *touch, int32_t id) {
	ssize_t next = -1;
	for (size_t i = 0; i < sizeof(touch->slots) / sizeof(*touch->slots); ++i) {
		if (touch->slots[i].id == id) {
			return &touch->slots[i];
		}
		if (next == -1 && !touch->slots[i].output) {
			next = i;
		}
	}
	if (next == -1) {
		sway_log(SWAY_ERROR, "Ran out of touch slots");
		return NULL;
	}
	return &touch->slots[next];
}

static void wl_touch_down(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, struct wl_surface *surface,
		int32_t id, wl_fixed_t _x, wl_fixed_t _y) {
	struct swaybar *bar = data;
	struct swaybar_output *_output = NULL, *output = NULL;
	wl_list_for_each(_output, &bar->outputs, link) {
		if (_output->surface == surface) {
			output = _output;
			break;
		}
	}
	if (!output) {
		sway_log(SWAY_DEBUG, "Got touch event for unknown surface");
		return;
	}
	struct touch_slot *slot = get_touch_slot(&bar->touch, id);
	if (!slot) {
		return;
	}
	slot->id = id;
	slot->output = output;
	slot->x = slot->start_x = wl_fixed_to_double(_x);
	slot->y = slot->start_y = wl_fixed_to_double(_y);
	slot->time = time;
}

static void wl_touch_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id) {
	struct swaybar *bar = data;
	struct touch_slot *slot = get_touch_slot(&bar->touch, id);
	if (!slot) {
		return;
	}
	if (time - slot->time < 500) {
		// Tap, treat it like a pointer click
		process_hotspots(slot->output, slot->x, slot->y, BTN_LEFT);
	}
	slot->output = NULL;
}

static void wl_touch_motion(void *data, struct wl_touch *wl_touch,
		uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct swaybar *bar = data;
	struct touch_slot *slot = get_touch_slot(&bar->touch, id);
	if (!slot) {
		return;
	}
	int prev_progress = (int)((slot->x - slot->start_x)
			/ slot->output->width * 100);
	slot->x = wl_fixed_to_double(x);
	slot->y = wl_fixed_to_double(y);
	// "progress" is a measure from 0..100 representing the fraction of the
	// output the touch gesture has travelled, positive when moving to the right
	// and negative when moving to the left.
	int progress = (int)((slot->x - slot->start_x)
			/ slot->output->width * 100);
	if (abs(progress) / 20 != abs(prev_progress) / 20) {
		workspace_next(bar, slot->output, progress - prev_progress < 0);
	}
}

static void wl_touch_frame(void *data, struct wl_touch *wl_touch) {
	// Who cares
}

static void wl_touch_cancel(void *data, struct wl_touch *wl_touch) {
	struct swaybar *bar = data;
	struct swaybar_touch *touch = &bar->touch;
	for (size_t i = 0; i < sizeof(touch->slots) / sizeof(*touch->slots); ++i) {
		touch->slots[i].output = NULL;
	}
}

static void wl_touch_shape(void *data, struct wl_touch *wl_touch, int32_t id,
		wl_fixed_t major, wl_fixed_t minor) {
	// Who cares
}

static void wl_touch_orientation(void *data, struct wl_touch *wl_touch,
		int32_t id, wl_fixed_t orientation) {
	// Who cares
}

static const struct wl_touch_listener touch_listener = {
	.down = wl_touch_down,
	.up = wl_touch_up,
	.motion = wl_touch_motion,
	.frame = wl_touch_frame,
	.cancel = wl_touch_cancel,
	.shape = wl_touch_shape,
	.orientation = wl_touch_orientation,
};

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		enum wl_seat_capability caps) {
	struct swaybar *bar = data;
	if (bar->pointer.pointer != NULL) {
		wl_pointer_release(bar->pointer.pointer);
		bar->pointer.pointer = NULL;
	}
	if (bar->touch.touch != NULL) {
		wl_touch_release(bar->touch.touch);
		bar->touch.touch = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER)) {
		bar->pointer.pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(bar->pointer.pointer, &pointer_listener, bar);
	}
	if ((caps & WL_SEAT_CAPABILITY_TOUCH)) {
		bar->touch.touch = wl_seat_get_touch(wl_seat);
		wl_touch_add_listener(bar->touch.touch, &touch_listener, bar);
	}
}

static void seat_handle_name(void *data, struct wl_seat *wl_seat,
		const char *name) {
	// Who cares
}

const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};
