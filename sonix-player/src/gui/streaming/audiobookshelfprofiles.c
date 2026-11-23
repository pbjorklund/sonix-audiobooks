#include "audiobookshelfprofiles.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/abs_profiles.h"

// The green of the language page's chosen-language tick: the "this is the one
// in use" colour, so a marked profile row reads like a language row.
#define PICKER_CHECK_GREEN lv_color_make(46, 194, 126)

static gui_config_t *config;
static lv_obj_t *picker_screen;
static lv_obj_t *picker_list;
static audiobookshelfprofiles_delegate_t module_delegate;

// Calibration aid for the UI smoke tests: while SONIX_PROFNAV is set, every
// navigation action lands in the player log. Host builds only; inert on the
// device.
#ifdef HOST_BUILD
static void profnav(const char *fmt, ...) {
	const char *on = getenv("SONIX_PROFNAV");
	if (!on || !on[0]) return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "PROFNAV ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	// The harness greps the log live, sometimes with nothing else logging
	// before it; leave the line on disk at once.
	fflush(stderr);
	va_end(ap);
}
#else
static void profnav(const char *fmt, ...) { (void)fmt; }
#endif

// What a row needs after the store read: identity for selecting and display
// text. Deliberately not abs_profile_t -- the store's token bytes are dropped
// on the way through and never retained anywhere in this module (§11.6).
typedef struct {
	char id[ABS_PROFILE_ID_MAX];
	char name[ABS_PROFILE_NAME_MAX];
	char server[ABS_SERVER_MAX];
} picker_entry_t;

static picker_entry_t entries[ABS_PROFILE_MAX];
static int entry_count;
static lv_obj_t *marks[ABS_PROFILE_MAX];

// The focused editor (§11.6): one screen for Add and Edit, with name, server
// (immutable after creation) and token fields plus the shared keyboard. An
// empty editor_id means adding a new profile. A saved token is never read back
// into the field -- a blank token on save keeps the stored one (§11.4).
static lv_obj_t *editor_screen;
static lv_obj_t *editor_title_label; // retitled per mode on open
static lv_obj_t *editor_name_field;
static lv_obj_t *editor_server_field;
static lv_obj_t *editor_token_field;
static lv_obj_t *editor_eye_icon;
static lv_obj_t *editor_note_edit;   // edit mode: server fixed, blank token keeps
static lv_obj_t *editor_note_apikey; // add mode: where an API key comes from
static keyboard_t *editor_keyboard;
static char editor_id[ABS_PROFILE_ID_MAX];

static void select_cb(lv_event_t *e);
static void add_cb(lv_event_t *e);
static void edit_cb(lv_event_t *e);
static void remove_cb(lv_event_t *e);
static void remove_confirmed_cb(void *user);
static void editor_accept_cb(lv_event_t *e);

// The marks are painted from the store's active id: the id is the durable
// identity, the row order is only this screen's display order.
static void paint_checks(void) {
	char active_id[ABS_PROFILE_ID_MAX];
	abs_profiles_active(active_id, sizeof(active_id));
	for (int i = 0; i < entry_count; i++) {
		if (marks[i]) lv_obj_set_hidden(marks[i], strcmp(entries[i].id, active_id) != 0);
	}
}

// A row's edit/remove button: a quiet square that absorbs its own taps -- no
// event bubble -- so pressing it never selects the row underneath.
static lv_obj_t *make_row_button(lv_obj_t *row, const lv_image_dsc_t *icon, int x_off, lv_event_cb_t cb,
								 void *user_data) {
	lv_obj_t *btn = lv_btn_create(row);
	lv_obj_set_size(btn, 64, 64);
	lv_obj_align(btn, LV_ALIGN_RIGHT_MID, x_off, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

	lv_obj_t *image = lv_image_create(btn);
	lv_image_set_src(image, icon);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_center(image);
	return btn;
}

#ifdef HOST_BUILD
static int picker_dump_count;

static void picker_dump_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (++picker_dump_count > 12) { // a handful of settled snapshots is enough
		lv_timer_delete(timer);
		return;
	}
	if (!picker_list || !lv_obj_is_valid(picker_screen)) return;
	lv_area_t list_a, screen_a;
	lv_obj_get_coords(picker_list, &list_a);
	lv_obj_get_coords(picker_screen, &screen_a);
	fprintf(stderr, "PICKERDUMP screen x=%ld..%ld y=%ld..%ld\n", (long)screen_a.x1, (long)screen_a.x2, (long)screen_a.y1,
			(long)screen_a.y2);
	fprintf(stderr, "PICKERDUMP list x=%ld..%ld y=%ld..%ld\n", (long)list_a.x1, (long)list_a.x2, (long)list_a.y1,
			(long)list_a.y2);
	uint32_t rows = lv_obj_get_child_count(picker_list);
	for (uint32_t i = 0; i < rows; i++) {
		lv_obj_t *row = lv_obj_get_child(picker_list, (int32_t)i);
		lv_area_t a;
		lv_obj_get_coords(row, &a);
		fprintf(stderr, "PICKERDUMP row%u x=%ld..%ld y=%ld..%ld\n", i, (long)a.x1, (long)a.x2, (long)a.y1,
				(long)a.y2);
		uint32_t kids = lv_obj_get_child_count(row);
		for (uint32_t k = 0; k < kids; k++) {
			lv_obj_t *kid = lv_obj_get_child(row, (int32_t)k);
			if (!lv_obj_check_type(kid, &lv_button_class)) continue;
			lv_area_t b;
			lv_obj_get_coords(kid, &b);
			fprintf(stderr, "PICKERDUMP row%u btn x=%ld..%ld y=%ld..%ld\n", i, (long)b.x1, (long)b.x2, (long)b.y1,
					(long)b.y2);
		}
	}
	fflush(stderr); // the harness greps these to calibrate tap coordinates
}
#endif

// Redraws the whole list. Runs on init and on every entry; a selection repaints
// only the checks, so a switch does not flash the list.
static void draw(void) {
	lv_obj_clean(picker_list);

	abs_profile_t profiles[ABS_PROFILE_MAX];
	int count = abs_profiles_list(profiles, ABS_PROFILE_MAX);
	entry_count = 0;
	for (int i = 0; i < count && entry_count < ABS_PROFILE_MAX; i++) {
		picker_entry_t *entry = &entries[entry_count++];
		// Same-size fixed fields, copied directly: an snprintf("%s") from one
		// fixed buffer into another of the same size is correct but makes GCC
		// suspect truncation (-Wformat-truncation) in this very same-size case.
		memcpy(entry->id, profiles[i].id, sizeof(entry->id));
		memcpy(entry->name, profiles[i].name, sizeof(entry->name));
		memcpy(entry->server, profiles[i].server, sizeof(entry->server));
		// The token bytes served their one pass through this copy; drop them.
		memset(profiles[i].token, 0, sizeof(profiles[i].token));
	}

	for (int i = 0; i < entry_count; i++) {
		lv_obj_t *row = settingsrow_action(picker_list, entries[i].name[0] ? entries[i].name
																		   : entries[i].server,
										   select_cb, (void *)(intptr_t)i);
		// A profile set up late may carry a long URL where its name would be;
		// wrap like a search result rather than stepping the font down.
		settingsrow_name_lines(row, 2);

		// Right to left: remove, edit, and the active mark. The buttons sit in
		// the row's right padding zone, clear of the wrapped name text.
		lv_obj_t *mark = lv_image_create(row);
		lv_image_set_src(mark, &icon_check);
		lv_obj_set_style_image_recolor(mark, PICKER_CHECK_GREEN, 0);
		lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
		lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -152, 0);
		lv_obj_set_hidden(mark, true);
		marks[i] = mark;
		make_row_button(row, &icon_pencil, -76, edit_cb, (void *)(intptr_t)i);
		make_row_button(row, &icon_trash, 0, remove_cb, (void *)(intptr_t)i);
	}

	// The Add row disappears at the four-profile cap: there is nowhere to put a
	// fifth, and a row that could only fail would be noise.
	if (entry_count < ABS_PROFILE_MAX) {
		settingsrow_action(picker_list, "audiobookshelf_profile_add", add_cb, NULL);
	}

	lv_obj_t *note = lv_label_create(picker_list);
	lv_label_set_text(note, tr("audiobookshelf_profile_note"));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_obj_set_style_pad_ver(note, 22, 0);

	paint_checks();
	profnav("picker_draw entries=%d", entry_count);

#ifdef HOST_BUILD
	// Calibration aid: while SONIX_PICKER_DUMP is set, print the laid-out row and
	// button centres a moment after draw. Host builds only; inert on the device.
	if (getenv("SONIX_PICKER_DUMP") && getenv("SONIX_PICKER_DUMP")[0]) {
		lv_timer_create(picker_dump_timer_cb, 500, NULL);
	}
#endif
}

static void picker_loaded_cb(lv_event_t *e) {
	(void)e;
	draw();
}

static void select_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) return;

	char active_id[ABS_PROFILE_ID_MAX];
	abs_profiles_active(active_id, sizeof(active_id));
	if (strcmp(entries[index].id, active_id) == 0) {
		profnav("select_skip id=%s already active", entries[index].id);
		return;
	}
	profnav("select id=%s name=%s", entries[index].id, entries[index].name);

	// The store persists before the page is told: the active id is what every
	// browse request will read, so the switch must be on file first (§11.4).
	// A failing save leaves the previous id in place, and the store lock covers
	// the memory flip and the file write together.
	if (!abs_profiles_select(entries[index].id)) {
		toast_error("audiobookshelf_profile_select_failed");
		return;
	}
	paint_checks();
	if (module_delegate.on_selected) module_delegate.on_selected(module_delegate.user);
}

// ---------------------------------------------------------------------------
// The focused editor (§11.6)
// ---------------------------------------------------------------------------

static void editor_field_focus_cb(lv_event_t *e) {
	lv_obj_t *field = lv_event_get_target(e);
	keyboard_set_field(editor_keyboard, field);
	lv_obj_t *fields[3] = {editor_name_field, editor_server_field, editor_token_field};
	for (int i = 0; i < 3; i++) {
		if (fields[i] == field) {
			lv_obj_add_state(field, LV_STATE_FOCUSED);
			keyboard_show_caret(field, true);
		} else {
			lv_obj_remove_state(fields[i], LV_STATE_FOCUSED);
			keyboard_show_caret(fields[i], false);
		}
	}
}

static void editor_eye_cb(lv_event_t *e) {
	(void)e;
	bool hidden = !lv_textarea_get_password_mode(editor_token_field);
	lv_textarea_set_password_mode(editor_token_field, hidden);
	lv_image_set_src(editor_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	keyboard_refresh_password(editor_token_field);
}

// The field shape the setup screen used: one line, card fill, caret styled for
// the keyboard. `max_len` caps what a store call would accept whole anyway --
// over-length values are rejected, never truncated (§11.1).
static lv_obj_t *make_editor_field(int y, const char *placeholder, int max_len) {
	lv_obj_t *field = lv_textarea_create(editor_screen);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_max_length(field, max_len);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, config->screen_width - 2 * config->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, config->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_22, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, editor_field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void build_editor(void) {
	editor_screen = lv_obj_create(NULL);
	lv_obj_add_style(editor_screen, &theme_style_screen, 0);
	// The title is retitled per mode on open; the label itself is kept.
	editor_title_label = settingsrow_title(editor_screen, config, "audiobookshelf_profile_edit");
	int top = settingsrow_content_top(config);

	// Three fields at 70px pitch (the setup screen's two used 78): the note and
	// the keyboard leave no room for more. content_top is 130 on the panel, so
	// the token field ends at 332 and the notes fit above the keyboard at 404.
	editor_name_field = make_editor_field(top, "audiobookshelf_profile_name_ph", ABS_PROFILE_NAME_MAX - 1);
	// The server is the profile's identity and cannot change after creation
	// (§11.2): in edit mode the field shows it but takes no input.
	editor_server_field = make_editor_field(top + 70, "audiobookshelf_server_address", ABS_SERVER_MAX - 1);
	editor_token_field = make_editor_field(top + 140, "audiobookshelf_access_token", ABS_TOKEN_MAX - 1);
	keyboard_style_password(editor_token_field, 0);
	lv_obj_set_style_pad_right(editor_token_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(editor_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -config->padding - 4, top + 143);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, editor_eye_cb, LV_EVENT_CLICKED, NULL);
	editor_eye_icon = lv_image_create(eye);
	lv_obj_add_style(editor_eye_icon, &theme_style_icon, 0);
	lv_image_set_src(editor_eye_icon, &icon_eye);
	lv_obj_center(editor_eye_icon);

	// The notes are mode-specific: add explains where a token comes from (the
	// setup screen's note), edit says what is fixed and what a blank means.
	// Both sit at the same y so either fits above the keyboard; the text is set
	// on every open, which also picks up a language switch made elsewhere.
	editor_note_apikey = lv_label_create(editor_screen);
	lv_label_set_long_mode(editor_note_apikey, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(editor_note_apikey, config->screen_width - 2 * config->padding);
	lv_obj_align(editor_note_apikey, LV_ALIGN_TOP_LEFT, config->padding, top + 212);
	lv_obj_add_style(editor_note_apikey, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(editor_note_apikey, &font_ui_18, 0);

	editor_note_edit = lv_label_create(editor_screen);
	lv_label_set_long_mode(editor_note_edit, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(editor_note_edit, config->screen_width - 2 * config->padding);
	lv_obj_align(editor_note_edit, LV_ALIGN_TOP_LEFT, config->padding, top + 212);
	lv_obj_add_style(editor_note_edit, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(editor_note_edit, &font_ui_18, 0);

	// The check is the commit: save what stands in the fields.
	editor_keyboard = keyboard_create(editor_screen, config->screen_width, 316, editor_name_field,
									  &icon_check, NULL, editor_accept_cb, NULL);
	switcher_attach_back_gesture(editor_screen);
}

// Opens the editor for `id`, or in add mode when id is NULL. The token field
// starts blank in both modes: on edit a blank save keeps the stored token, so
// there is no reason -- and no safe way -- to show what was saved (§11.6).
static void open_editor(const char *id) {
	bool add = (id == NULL || id[0] == '\0');
	if (add) editor_id[0] = '\0';
	else snprintf(editor_id, sizeof(editor_id), "%s", id);
	if (add) profnav("editor_add");
	else profnav("editor_edit id=%s", id);

	char name[ABS_PROFILE_NAME_MAX], server[ABS_SERVER_MAX];
	name[0] = server[0] = '\0';
	if (!add) {
		// There is no single-profile getter: the list read is reduced to this
		// one entry's id/name/server, and its token bytes are dropped on the
		// spot (§11.6).
		abs_profile_t profiles[ABS_PROFILE_MAX];
		int count = abs_profiles_list(profiles, ABS_PROFILE_MAX);
		for (int i = 0; i < count; i++) {
			if (strcmp(profiles[i].id, id) != 0) continue;
			memcpy(name, profiles[i].name, sizeof(name));
			memcpy(server, profiles[i].server, sizeof(server));
			break;
		}
		memset(profiles, 0, sizeof(profiles)); // the one token copy is done with
	}

	lv_label_set_text(editor_title_label, tr(add ? "audiobookshelf_profile_add" : "audiobookshelf_profile_edit"));
	lv_textarea_set_text(editor_name_field, add ? "" : name);
	lv_textarea_set_text(editor_server_field, add ? "" : server);

	lv_obj_set_clickable(editor_server_field, add); // fixed after creation (§11.2)
	char edit_note[160];
	snprintf(edit_note, sizeof(edit_note), "%s\n%s", tr("audiobookshelf_profile_server_fixed"),
			 tr("audiobookshelf_profile_token_keep"));
	lv_label_set_text(editor_note_apikey, tr("audiobookshelf_token_note"));
	lv_label_set_text(editor_note_edit, edit_note);
	lv_obj_set_hidden(editor_note_apikey, !add);
	lv_obj_set_hidden(editor_note_edit, add);

	lv_textarea_set_text(editor_token_field, "");
	lv_textarea_set_password_mode(editor_token_field, true);
	lv_image_set_src(editor_eye_icon, &icon_eye);
	keyboard_refresh_password(editor_token_field);
	keyboard_reset(editor_keyboard);

	// The first editable field takes the focus so typing starts at once: the
	// server on add (the one thing that cannot be changed later), the name on
	// edit. Same hand-sent focus as the setup screen used to get.
	lv_obj_t *first = add ? editor_server_field : editor_name_field;
	keyboard_set_field(editor_keyboard, first);
	for (int i = 0; i < 3; i++) {
		lv_obj_t *f = i == 0 ? editor_name_field : (i == 1 ? editor_server_field : editor_token_field);
		if (f == first) lv_obj_add_state(f, LV_STATE_FOCUSED);
		else lv_obj_remove_state(f, LV_STATE_FOCUSED);
		keyboard_show_caret(f, f == first);
	}
	switch_screen(editor_screen);
}

static void add_cb(lv_event_t *e) {
	(void)e;
	open_editor(NULL);
}

static void edit_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) return;
	open_editor(entries[index].id);
}

// ---------------------------------------------------------------------------
// Saving: add, or edit name/token of an existing profile
// ---------------------------------------------------------------------------

static void editor_accept_cb(lv_event_t *e) {
	(void)e;
	char name[ABS_PROFILE_NAME_MAX], server[ABS_SERVER_MAX], token[ABS_TOKEN_MAX];
	snprintf(name, sizeof(name), "%s", lv_textarea_get_text(editor_name_field));
	snprintf(server, sizeof(server), "%s", lv_textarea_get_text(editor_server_field));
	snprintf(token, sizeof(token), "%s", lv_textarea_get_text(editor_token_field));

	// Display names are stored as typed, minus the whitespace a tap-and-type
	// session tends to leave at either end.
	size_t len = strlen(name);
	while (len && isspace((unsigned char)name[len - 1])) name[--len] = '\0';
	size_t start = 0;
	while (len - start > 0 && isspace((unsigned char)name[start])) start++;
	if (start) memmove(name, name + start, len - start + 1);

	if (editor_id[0] == '\0') {
		// --- Add: the store validates and normalizes; mirror its rules here so
		// a rejection names the cause instead of failing silently (§11.2).
		char normalized[ABS_SERVER_MAX], reason[64];
		if (!abs_profiles_server_valid(server, normalized, sizeof(normalized), reason, sizeof(reason))) {
			toast_error("audiobookshelf_profile_server_invalid");
			return;
		}
		if (abs_profiles_has_server(normalized)) {
			toast_error("audiobookshelf_profile_duplicate");
			return;
		}
		if (!token[0]) {
			toast_error("audiobookshelf_token_required");
			return;
		}
		char new_id[ABS_PROFILE_ID_MAX];
		if (!abs_profiles_add(name, server, token, new_id, sizeof(new_id))) {
			toast_error("audiobookshelf_profile_save_failed");
			return;
		}
		profnav("save_added id=%s name=%s", new_id, name);
		lv_textarea_set_text(editor_token_field, ""); // the typed secret leaves with the save
		back_btn_cb(NULL);                            // back to the picker
		// The new profile is active: the page clears its remote state and loads
		// that server's libraries -- the load doubles as validation (§11.4).
		if (module_delegate.on_selected) module_delegate.on_selected(module_delegate.user);
		return;
	}

	// --- Edit: the server never changes (§11.2), so only name and token are
	// offered to it. A blank token keeps the stored one, which is exactly what
	// abs_profiles_edit does with an empty string -- no read-back needed.
	if (!abs_profiles_edit(editor_id, name, token)) {
		toast_error("audiobookshelf_profile_save_failed");
		return;
	}
	profnav("save_edited id=%s name=%s", editor_id, name);
	lv_textarea_set_text(editor_token_field, "");

	char active_id[ABS_PROFILE_ID_MAX];
	bool was_active = abs_profiles_active(active_id, sizeof(active_id)) && strcmp(active_id, editor_id) == 0;
	if (was_active && module_delegate.on_validate) {
		// Stay on the editor while the page checks the credentials with a real
		// GET /api/libraries: success steps back to the picker and toasts, a
		// failure pops over the fields so the token can be corrected in place.
		module_delegate.on_validate(module_delegate.user);
	} else {
		back_btn_cb(NULL); // non-active profile: nothing to validate against
	}
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

static void remove_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) return;

	// The name goes in the question, because on a list of rows the one being
	// removed is not obvious by the time the dialog is up. Formatted here and
	// passed as the title: confirm_show() puts its message through tr(), and a
	// profile name is not a translation key (the files page does it this way).
	char question[ABS_PROFILE_NAME_MAX + 64];
	snprintf(question, sizeof(question), tr("audiobookshelf_profile_remove_question"),
			 entries[index].name[0] ? entries[index].name : entries[index].server);
	profnav("remove_confirm id=%s", entries[index].id);
	confirm_show(question, NULL, "remove", remove_confirmed_cb, (void *)(intptr_t)index);
}

static void remove_confirmed_cb(void *user) {
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= entry_count) return;

	char id[ABS_PROFILE_ID_MAX];
	snprintf(id, sizeof(id), "%s", entries[index].id);
	profnav("remove_confirmed id=%s", id);

	char active_id[ABS_PROFILE_ID_MAX];
	bool was_active = abs_profiles_active(active_id, sizeof(active_id)) && strcmp(active_id, id) == 0;

	// The store blocks while a download queue references the profile (none
	// exists yet in 0.6.0 M1); existing linked media is never touched (§11.4).
	if (!abs_profiles_remove(id)) {
		toast_error("audiobookshelf_profile_remove_failed");
		return;
	}
	draw(); // rows and checks straight away: we are on the picker when this runs

	// The active profile is gone, whatever remains: the page's remote state was
	// that server's, so it clears to the unconfigured view. Picking one of the
	// remaining profiles (if any) is a deliberate tap, not an implicit switch.
	if (was_active && module_delegate.on_active_lost) {
		module_delegate.on_active_lost(module_delegate.user);
	}
}

lv_obj_t *audiobookshelfprofiles_screen(void) { return picker_screen; }

lv_obj_t *audiobookshelfprofiles_editor_screen(void) { return editor_screen; }

lv_obj_t *audiobookshelfprofiles_init(gui_config_t *cfg, const audiobookshelfprofiles_delegate_t *delegate) {
	if (delegate) module_delegate = *delegate;
	config = cfg;
	picker_screen = lv_obj_create(NULL);
	picker_list = settingsrow_page(picker_screen, cfg, "audiobookshelf_profiles");
	// settingsrow_page() only gestures on the container; row taps land on the
	// screen's empty margins, and the setup page wires the screen for the same
	// reason.
	switcher_attach_back_gesture(picker_screen);
	lv_obj_add_event_cb(picker_screen, picker_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	build_editor();
	draw();
	return picker_screen;
}
