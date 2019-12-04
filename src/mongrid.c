/*
 * Copyright (C) 2017-2019  Minnesota Department of Transportation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gst/gst.h>
#include <gtk/gtk.h>
#include "modebar.h"
#include "elog.h"
#include "nstr.h"
#include "stream.h"
#include "lock.h"

#define ACCENT_GRAY	0x444444
#define ACCENT_LT_GRAY	0x888888
#define COLOR_MON	0xFFFF88

struct moncell {
	struct stream	stream;          /* must be first, due to casting */
	char		mid[8];          /* monitor ID */
	char		description[64]; /* location description */
	char		extra[24];       /* extra monitors */
	int32_t		accent;          /* accent color for title */
	uint32_t	font_sz;
	GtkCssProvider	*css_provider;
	GtkWidget	*box;
	GtkWidget	*video;
	GtkWidget	*title;
	GtkWidget	*mon_lbl;
	GtkWidget	*stat_lbl;
	GtkWidget	*cam_lbl;
	GtkWidget	*desc_lbl;
	GtkWidget	*ex_lbl;
	gboolean	started;
	gboolean        failed;
};

struct mongrid {
	struct lock	lock;
	bool		stats;
	GtkWidget	*window;
	GtkWidget	*tbox;
	GtkWidget	*grid;
	struct modebar	*mbar;
	uint32_t	n_cells;
	struct moncell	*cells;
	bool		running;
};

static struct mongrid grid;

static bool is_moncell_valid(const struct moncell *mc) {
	return (mc >= grid.cells) && mc < (grid.cells + grid.n_cells);
}

static bool moncell_has_title(const struct moncell *mc) {
	return mc->mid[0] != '\0';
}

static const char *moncell_get_cam_id(const struct moncell *mc) {
	return mc->stream.cam_id;
}

static void moncell_set_description(struct moncell *mc, nstr_t desc) {
	nstr_to_cstr(mc->description, sizeof(mc->description), desc);
}

static const char MONCELL_CSS[] =
	"* { "
		"color: white; "
		"font-family: Overpass; "
		"font-size: %upt; "
	"}\n"
	"box.title { "
		"margin-top: 1px; "
		"background-color: #%06x; "
	"}\n"
	"label {"
		"padding-left: 8px; "
		"padding-right: 8px; "
		"border-right: solid 1px white; "
	"}\n"
	"label#mon_lbl {"
		"color: #%06x; "
		"background-color: #%06x; "
		"font-weight: Bold; "
		"border-left: solid 1px white; "
	"}\n"
	"label#stat_lbl {"
		"color: #882222; "
		"background-color: #%06x; "
	"}\n"
	"label#cam_lbl {"
		"font-weight: Bold; "
	"}\n";

static void moncell_set_accent(struct moncell *mc) {
	char css[sizeof(MONCELL_CSS) + 16];
	GError *err = NULL;
	int32_t a0 = (mc->accent > 0) ? mc->accent : ACCENT_GRAY;
	int32_t a1 = (mc->started) ? a0 : ACCENT_GRAY;
	int32_t a2 = (grid.stats) ? ACCENT_LT_GRAY : a1;

	snprintf(css, sizeof(css), MONCELL_CSS, mc->font_sz, a1, COLOR_MON, a0,
		a2);
	gtk_css_provider_load_from_data(mc->css_provider, css, -1, &err);
	if (err != NULL)
		elog_err("CSS error: %s\n", err->message);
}

static void moncell_update_title(struct moncell *mc) {
	/* Hide titlebar when monitor ID is blank */
	if (moncell_has_title(mc)) {
		gtk_label_set_text(GTK_LABEL(mc->mon_lbl), mc->mid);
		gtk_label_set_text(GTK_LABEL(mc->cam_lbl),
			moncell_get_cam_id(mc));
		gtk_label_set_text(GTK_LABEL(mc->desc_lbl), mc->description);
		gtk_label_set_text(GTK_LABEL(mc->ex_lbl), mc->extra);
		gtk_widget_show_all(mc->title);
		if (0 == mc->extra[0])
			gtk_widget_hide(mc->ex_lbl);
	} else
		gtk_widget_hide(mc->title);
}

static void moncell_update_accent_title(struct moncell *mc) {
	moncell_set_accent(mc);
	moncell_update_title(mc);
}

static gboolean draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data) {
	struct moncell *mc = data;
	lock_acquire(&grid.lock, __func__);
	/* moncell may have been freed while timer ran */
	if (is_moncell_valid(mc)) {
		guint width = gtk_widget_get_allocated_width(widget);
		guint height = gtk_widget_get_allocated_height(widget);
		cairo_rectangle(cr, 0, 0, width, height);
		cairo_fill(cr);
	}
	lock_release(&grid.lock, __func__);
	return TRUE;
}

static void moncell_clear(struct moncell *mc) {
	guint width = gtk_widget_get_allocated_width(mc->video);
	guint height = gtk_widget_get_allocated_height(mc->video);
	gtk_widget_queue_draw_area(mc->video, 0, 0, width, height);
}

static void moncell_restart_stream(struct moncell *mc) {
	if (!mc->started) {
		bool s = stream_start(&mc->stream);
		mc->started = TRUE;
		if (grid.window && !s) {
			moncell_update_accent_title(mc);
			moncell_clear(mc);
		}
	}
}

static gboolean do_update_title(gpointer data) {
	struct moncell *mc = (struct moncell *) data;
	lock_acquire(&grid.lock, __func__);
	/* moncell may have been freed while timer ran */
	if (is_moncell_valid(mc)) {
		moncell_update_accent_title(mc);
		/* only update modebar if this is the first monitor */
		if (mc == grid.cells)
			modebar_set_accent(grid.mbar, mc->accent, mc->font_sz);
	}
	lock_release(&grid.lock, __func__);
	return FALSE;
}

static gboolean do_stop_stream(gpointer data) {
	struct moncell *mc = (struct moncell *) data;
	lock_acquire(&grid.lock, __func__);
	/* moncell may have been freed while timer ran */
	if (is_moncell_valid(mc)) {
		stream_stop(&mc->stream);
		if (grid.window)
			moncell_clear(mc);
	}
	lock_release(&grid.lock, __func__);
	return FALSE;
}

static gboolean do_restart(gpointer data) {
	struct moncell *mc = (struct moncell *) data;
	lock_acquire(&grid.lock, __func__);
	/* moncell may have been freed while timer ran */
	if (is_moncell_valid(mc))
		moncell_restart_stream(mc);
	lock_release(&grid.lock, __func__);
	return FALSE;
}

static void moncell_stop_stream(struct moncell *mc, guint delay) {
	mc->started = FALSE;
	if (grid.window)
		g_timeout_add(0, do_update_title, mc);
	g_timeout_add(0, do_stop_stream, mc);
	/* delay is needed to allow gtk+ to update accent color */
	g_timeout_add(delay, do_restart, mc);
}

static void moncell_stop(struct stream *st) {
	/* Cast requires stream is first member of struct */
	struct moncell *mc = (struct moncell *) st;
	mc->failed = mc->started;
	moncell_stop_stream(mc, 1000);
}

static void moncell_ack_started(struct stream *st) {
	/* Cast requires stream is first member of struct */
	struct moncell *mc = (struct moncell *) st;
	mc->failed = FALSE;
	g_timeout_add(0, do_update_title, mc);
}

static GtkWidget *create_title(const struct moncell *mc) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	GtkStyleContext *ctx = gtk_widget_get_style_context(box);
	gtk_style_context_add_class(ctx, "title");
	gtk_style_context_add_provider(ctx,
		GTK_STYLE_PROVIDER (mc->css_provider),
		GTK_STYLE_PROVIDER_PRIORITY_USER);
	return box;
}

static GtkWidget *create_label(GtkCssProvider *css_provider, const char *name,
	int n_chars)
{
	GtkWidget *lbl = gtk_label_new("");
	GtkStyleContext *ctx = gtk_widget_get_style_context(lbl);
	gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER (css_provider),
		GTK_STYLE_PROVIDER_PRIORITY_USER);
	gtk_widget_set_name(lbl, name);
	gtk_label_set_selectable(GTK_LABEL(lbl), FALSE);
	if (n_chars)
		gtk_label_set_max_width_chars(GTK_LABEL(lbl), n_chars);
	gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
	g_object_set(G_OBJECT(lbl), "single-line-mode", TRUE, NULL);
	return lbl;
}

static void moncell_init_gtk(struct moncell *mc) {
	mc->stream.ack_started = moncell_ack_started;
	mc->css_provider = gtk_css_provider_new();
	mc->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	mc->video = gtk_drawing_area_new();
	g_signal_connect(G_OBJECT(mc->video), "draw", G_CALLBACK(draw_cb), mc);
	mc->title = create_title(mc);
	mc->mon_lbl = create_label(mc->css_provider, "mon_lbl", 6);
	mc->stat_lbl = create_label(mc->css_provider, "stat_lbl", 0);
	mc->cam_lbl = create_label(mc->css_provider, "cam_lbl", 0);
	mc->desc_lbl = create_label(mc->css_provider, "desc_lbl", 0);
	mc->ex_lbl = create_label(mc->css_provider, "mon_lbl", 0);
	moncell_set_accent(mc);
	moncell_update_title(mc);
	gtk_box_pack_start(GTK_BOX(mc->title), mc->mon_lbl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(mc->title), mc->stat_lbl, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(mc->title), mc->ex_lbl, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(mc->title), mc->desc_lbl, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(mc->title), mc->cam_lbl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(mc->box), mc->video, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(mc->box), mc->title, FALSE, FALSE, 0);
}

static void moncell_init(struct moncell *mc, uint32_t idx, nstr_t sink_name) {
	memset(mc, 0, sizeof(struct moncell));
	stream_init(&mc->stream, idx, &grid.lock, sink_name);
	mc->stream.do_stop = moncell_stop;
	mc->font_sz = 32;
	mc->started = FALSE;
	mc->failed = FALSE;
	if (grid.window)
		moncell_init_gtk(mc);
}

static void moncell_destroy(struct moncell *mc) {
	stream_destroy(&mc->stream);
	if (grid.window) {
		gtk_widget_destroy(mc->mon_lbl);
		gtk_widget_destroy(mc->stat_lbl);
		gtk_widget_destroy(mc->cam_lbl);
		gtk_widget_destroy(mc->desc_lbl);
		gtk_widget_destroy(mc->ex_lbl);
		gtk_widget_destroy(mc->video);
		gtk_widget_destroy(mc->title);
		gtk_widget_destroy(mc->box);
	}
}

static void moncell_set_handle(struct moncell *mc) {
	guintptr handle = GDK_WINDOW_XID(gtk_widget_get_window(mc->video));
	stream_set_handle(&mc->stream, handle);
}

static void moncell_play_stream(struct moncell *mc, nstr_t cam_id, nstr_t loc,
	nstr_t desc, nstr_t encoding, uint32_t latency, nstr_t sprops)
{
	/* Only set text overlay description when there's no title bar */
	nstr_t dtxt = moncell_has_title(mc) ? nstr_init_empty() : desc;
	mc->failed = FALSE;
	moncell_set_description(mc, desc);
	stream_set_params(&mc->stream, cam_id, loc, dtxt, encoding, latency,
		sprops);
	/* Stopping the stream will trigger a restart */
	moncell_stop_stream(mc, 20);
}

static void moncell_set_mon(struct moncell *mc, nstr_t mid, int32_t accent,
	bool aspect, uint32_t font_sz, nstr_t crop, uint32_t hgap,
	uint32_t vgap, nstr_t extra)
{
	nstr_to_cstr(mc->mid, sizeof(mc->mid), mid);
	mc->accent = accent;
	stream_set_aspect(&mc->stream, aspect);
	stream_set_font_size(&mc->stream, font_sz);
	stream_set_crop(&mc->stream, crop, hgap, vgap);
	nstr_to_cstr(mc->extra, sizeof(mc->extra), extra);
	mc->font_sz = font_sz;
	if (grid.window)
		g_timeout_add(0, do_update_title, mc);
}

static void mongrid_set_handles(void) {
	for (uint32_t n = 0; n < grid.n_cells; n++)
		moncell_set_handle(grid.cells + n);
}

static uint32_t get_rows(uint32_t num) {
	uint32_t r = 1;
	uint32_t c = 1;
	while (r * c < num) {
		c++;
		if (r * c < num)
			r++;
	}
	return r;
}

static uint32_t get_cols(uint32_t num) {
	uint32_t r = 1;
	uint32_t c = 1;
	while (r * c < num) {
		c++;
		if (r * c < num)
			r++;
	}
	return c;
}

static void hide_cursor(GtkWidget *window) {
	GdkDisplay *display;
	GdkCursor *cursor;

	gtk_widget_realize(window);
	display = gtk_widget_get_display(window);
	cursor = gdk_cursor_new_for_display(display, GDK_BLANK_CURSOR);
	gdk_window_set_cursor(gtk_widget_get_window(window), cursor);
}

static void moncell_update_stats(struct moncell *mc, guint64 pushed,
	guint64 lost, guint64 late)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT "  %" G_GUINT64_FORMAT
	         "  %" G_GUINT64_FORMAT, pushed, lost, late);
	gtk_label_set_text(GTK_LABEL(mc->stat_lbl), buf);
}

static guint64 pkt_count(guint64 t0, guint64 t1) {
	return (t0 < t1) ? t1 - t0 : 0;
}

static gboolean do_check_sink(gpointer data) {
	lock_acquire(&grid.lock, __func__);
	for (uint32_t n = 0; n < grid.n_cells; n++) {
		struct moncell *mc = grid.cells + n;
		stream_check_eos(&mc->stream);
	}
	lock_release(&grid.lock, __func__);
	return TRUE;
}

static gboolean do_stats(gpointer data) {
	lock_acquire(&grid.lock, __func__);
	for (uint32_t n = 0; n < grid.n_cells; n++) {
		struct moncell *mc = grid.cells + n;
		guint64 pushed = mc->stream.pushed;
		guint64 lost = mc->stream.lost;
		guint64 late = mc->stream.late;
		if (stream_stats(&mc->stream)) {
			if (grid.window) {
				guint64 p = pkt_count(pushed,mc->stream.pushed);
				guint64 ls = pkt_count(lost, mc->stream.lost);
				guint64 lt = pkt_count(late, mc->stream.late);
				moncell_update_stats(mc, p, ls, lt);
			}
		}
	}
	lock_release(&grid.lock, __func__);
	return TRUE;
}

void mongrid_create(bool gui, bool stats) {
	gst_init(NULL, NULL);
	memset(&grid, 0, sizeof(struct mongrid));
	lock_init(&grid.lock);
	grid.stats = stats;
	if (gui) {
		gtk_init(NULL, NULL);
		GtkWidget *window = gtk_window_new(0);
		grid.window = window;
		grid.tbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		g_object_set(G_OBJECT(grid.tbox), "spacing", 4, NULL);
		grid.mbar = modebar_create(grid.window, &grid.lock);
		gtk_box_pack_start(GTK_BOX(grid.tbox), modebar_get_box(
			grid.mbar), FALSE, FALSE, 0);
		gtk_container_add(GTK_CONTAINER(grid.window), grid.tbox);
		gtk_window_set_title((GtkWindow *) window, "MonStream");
		gtk_window_fullscreen((GtkWindow *) window);
		hide_cursor(window);
	}
	g_timeout_add(2000, do_check_sink, NULL);
	if (stats)
		g_timeout_add(4000, do_stats, NULL);
}

static void mongrid_init_gtk(uint32_t n_cells) {
	int n_rows = get_rows(n_cells);
	int n_cols = get_cols(n_cells);
	grid.grid = gtk_grid_new();
	GtkGrid *gr = (GtkGrid *) grid.grid;
	gtk_grid_set_column_spacing(gr, 4);
	gtk_grid_set_row_spacing(gr, 4);
	gtk_grid_set_column_homogeneous(gr, TRUE);
	gtk_grid_set_row_homogeneous(gr, TRUE);
	for (uint32_t r = 0; r < n_rows; r++) {
		for (uint32_t c = 0; c < n_cols; c++) {
			uint32_t i = r * n_cols + c;
			struct moncell *mc = grid.cells + i;
			gtk_grid_attach(gr, mc->box, c, r, 1, 1);
		}
	}
	gtk_box_pack_end(GTK_BOX(grid.tbox), GTK_WIDGET(grid.grid),TRUE,TRUE,0);
	gtk_widget_show_all(grid.window);
	if (!modebar_is_visible(grid.mbar))
		modebar_hide(grid.mbar);
	gtk_widget_realize(grid.window);
	mongrid_set_handles();
}

int32_t mongrid_init(uint32_t num, pthread_t tid, nstr_t sink_name) {
	lock_acquire(&grid.lock, __func__);
	if (num > 16) {
		grid.n_cells = 0;
		elog_err("Grid too large: %d\n", num);
		goto err;
	}
	grid.n_cells = get_rows(num) * get_cols(num);
	grid.cells = calloc(grid.n_cells, sizeof(struct moncell));
	for (uint32_t n = 0; n < grid.n_cells; n++)
		moncell_init(grid.cells + n, n, sink_name);
	if (grid.window) {
		mongrid_init_gtk(grid.n_cells);
		modebar_set_tid(grid.mbar, tid);
	}
	grid.running = false;
	lock_release(&grid.lock, __func__);
	return 0;
err:
	lock_release(&grid.lock, __func__);
	return 1;
}

void mongrid_run(void) {
	lock_acquire(&grid.lock, __func__);
	grid.running = true;
	lock_release(&grid.lock, __func__);
	gtk_main();
}

static bool mongrid_is_running(void) {
	bool running = false;
	lock_acquire(&grid.lock, __func__);
	running = grid.running;
	lock_release(&grid.lock, __func__);
	return running;
}

void mongrid_restart(void) {
	lock_acquire(&grid.lock, __func__);
	grid.running = false;
	lock_release(&grid.lock, __func__);
	gtk_main_quit();
	while (true) {
		if (mongrid_is_running())
			break;
		sleep(1);
	}
}

void mongrid_reset(void) {
	lock_acquire(&grid.lock, __func__);
	for (uint32_t n = 0; n < grid.n_cells; n++)
		moncell_destroy(grid.cells + n);
	free(grid.cells);
	grid.cells = NULL;
	grid.n_cells = 0;
	if (grid.window) {
		gtk_container_remove(GTK_CONTAINER(grid.tbox), grid.grid);
		grid.grid = NULL;
	}
	lock_release(&grid.lock, __func__);
}

void mongrid_set_mon(uint32_t idx, nstr_t mid, int32_t accent, bool aspect,
	uint32_t font_sz, nstr_t crop, uint32_t hgap, uint32_t vgap,
	nstr_t extra)
{
	lock_acquire(&grid.lock, __func__);
	if (idx < grid.n_cells) {
		struct moncell *mc = grid.cells + idx;
		moncell_set_mon(mc, mid, accent, aspect, font_sz, crop, hgap,
			vgap, extra);
	}
	lock_release(&grid.lock, __func__);
}

void mongrid_play_stream(uint32_t idx, nstr_t cam_id, nstr_t loc, nstr_t desc,
	nstr_t encoding, uint32_t latency, nstr_t sprops)
{
	lock_acquire(&grid.lock, __func__);
	if (idx < grid.n_cells) {
		struct moncell *mc = grid.cells + idx;
		moncell_play_stream(mc, cam_id, loc, desc, encoding, latency,
			sprops);
	}
	lock_release(&grid.lock, __func__);
}

void mongrid_destroy(void) {
	mongrid_reset();
	if (grid.window)
		gtk_widget_destroy(grid.window);
	lock_destroy(&grid.lock);
	memset(&grid, 0, sizeof(struct mongrid));
}

/* ASCII separators */
static const char RECORD_SEP = '\x1E';
static const char UNIT_SEP = '\x1F';

static nstr_t moncell_status(struct moncell *mc, nstr_t str, uint32_t idx,
	bool full)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "status%c%d%c%s%c%s%c%s%c", UNIT_SEP,
		idx, UNIT_SEP,
		moncell_get_cam_id(mc), UNIT_SEP,
		(mc->failed) ? "failed" : "", UNIT_SEP,
		(full) ? "full" : "", RECORD_SEP);
	nstr_cat_z(&str, buf);
	return str;
}

nstr_t mongrid_status(nstr_t str) {
	lock_acquire(&grid.lock, __func__);
	bool full = (1 == grid.n_cells);
	for (uint32_t n = 0; n < grid.n_cells; n++)
		str = moncell_status(grid.cells + n, str, n, full);
	if (grid.mbar)
		str = modebar_status(grid.mbar, str);
	lock_release(&grid.lock, __func__);
	return str;
}

bool mongrid_mon_selected(void) {
	return (grid.mbar) && modebar_has_mon(grid.mbar);
}

void mongrid_display(nstr_t mon, nstr_t cam, nstr_t seq) {
	if (grid.mbar) {
		lock_acquire(&grid.lock, __func__);
		modebar_display(grid.mbar, mon, cam, seq);
		lock_release(&grid.lock, __func__);
	}
}

bool mongrid_joy_event(int fd) {
	if (grid.mbar) {
		struct js_event ev;
		int n_bytes = read(fd, &ev, sizeof(ev));
		if (n_bytes < 0) {
			elog_err("joystick read: %s\n", strerror(errno));
			lock_acquire(&grid.lock, __func__);
			modebar_hide(grid.mbar);
			lock_release(&grid.lock, __func__);
			return false;
		}
		if (n_bytes == sizeof(ev)) {
			lock_acquire(&grid.lock, __func__);
			modebar_joy_event(grid.mbar, &ev);
			lock_release(&grid.lock, __func__);
		}
		return true;
	} else
		return false;
}

void mongrid_set_online(bool online) {
	if (grid.mbar) {
		lock_acquire(&grid.lock, __func__);
		modebar_set_online(grid.mbar, online);
		lock_release(&grid.lock, __func__);
	}
}
