#include "tui.h"
#include "subagents.h"
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void ui_notice(KmUi *ui, const char *format, ...) {
    ui->notice_error = false;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(ui->notice, sizeof(ui->notice), format, arguments);
    va_end(arguments);
}

void ui_dialog_title(const char *title) {
    erase();
    ui_put(1, 3, UI_STYLE_ACCENT, "kitmux", 6);
    ui_put(2, 3, UI_STYLE_STRONG, title, COLS - 6);
    ui_rule(3, 3, COLS > 6 ? COLS - 6 : 1);
}

static void hint(int y, const char *const *pairs, int count) { ui_hints(y, 3, pairs, count); }

bool ui_modal_ready(void) {
    while (COLS < 74 || LINES < 20) {
        erase(); mvaddnstr(0, 0, "Resize to 74 columns x 20 rows. Esc cancels.", COLS > 0 ? COLS - 1 : 0); refresh();
        int key = getch();
        if (key == 27 || key == 'q' || km_ui_interrupted) return false;
    }
    return !km_ui_interrupted;
}

bool ui_input(KmUi *ui, const char *title, const char *label, char *value, size_t capacity) {
    size_t length = strlen(value);
    curs_set(1);
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) { curs_set(0); return false; }
        static const char *const keys[] = {"enter", "confirm", "esc", "cancel"};
        ui_dialog_title(title); ui_put(5, 3, UI_STYLE_MUTED, label, COLS - 6);
        ui_put(7, 3, UI_STYLE_STRONG, value, COLS > 8 ? COLS - 8 : 1);
        hint(10, keys, 2); move(7, 3 + (int)(length < (size_t)(COLS - 8) ? length : (size_t)(COLS - 8))); refresh();
        int key = getch();
        if (key == 27) { curs_set(0); return false; }
        if (key == '\n' || key == KEY_ENTER) { curs_set(0); return true; }
        if (key == KEY_BACKSPACE || key == 127 || key == 8) { if (length) value[--length] = '\0'; }
        else if (key >= 32 && key <= 126 && length + 1 < capacity) { value[length++] = (char)key; value[length] = '\0'; }
    }
    curs_set(0); (void)ui; return false;
}

bool ui_confirm(KmUi *ui, const char *message) {
    if (!ui_modal_ready()) return false;
    static const char *const keys[] = {"y", "confirm", "any other key", "cancel"};
    ui_dialog_title("Confirm change"); ui_put(5, 3, UI_STYLE_TEXT, message, COLS - 6);
    hint(8, keys, 2); refresh();
    int key;
    do { key = getch(); } while ((key == ERR || key == KEY_RESIZE) && !km_ui_interrupted);
    (void)ui; return key == 'y' || key == 'Y';
}

int ui_choice(const char *title, const char *const *choices, int count, int initial) {
    int row = initial >= 0 && initial < count ? initial : 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) return -1;
        ui_dialog_title(title);
        static const char *const keys[] = {"j/k", "move", "enter", "choose", "esc", "cancel"};
        for (int i = 0; i < count; i++) {
            char line[256]; snprintf(line, sizeof(line), " %s %s", i == row ? ui_glyph(UI_GLYPH_CURSOR) : " ", choices[i]);
            if (i == row) { attron(ui_style(UI_STYLE_SELECT)); mvhline(6 + i * 2, 2, ' ', COLS - 4); attroff(ui_style(UI_STYLE_SELECT)); }
            ui_put(6 + i * 2, 2, i == row ? UI_STYLE_SELECT : UI_STYLE_TEXT, line, COLS - 4);
        }
        hint(7 + count * 2, keys, 3); refresh();
        int key = getch();
        if (key == 27) return -1;
        if (key == '\n' || key == KEY_ENTER) return row;
        if (key == 'j' || key == KEY_DOWN) row = (row + 1) % count;
        if (key == 'k' || key == KEY_UP) row = (row + count - 1) % count;
    }
    return -1;
}

/* Runs one Kitmux command in-process with stdout/stderr captured, so curses output stays intact. */
int ui_capture(KmUi *ui, int argc, char **argv, char *output, size_t size, bool *truncated) {
    output[0] = '\0'; *truncated = false;
    FILE *capture = tmpfile();
    if (!capture) return km_error(ui->km, "Cannot capture command output: %s", strerror(errno));
    fflush(stdout); fflush(stderr);
    int out = dup(STDOUT_FILENO), err = dup(STDERR_FILENO);
    if (out < 0 || err < 0) {
        int saved = errno;
        if (out >= 0) close(out);
        if (err >= 0) close(err);
        fclose(capture); return km_error(ui->km, "Cannot capture command output: %s", strerror(saved));
    }
    if (dup2(fileno(capture), STDOUT_FILENO) < 0 || dup2(fileno(capture), STDERR_FILENO) < 0) {
        int saved = errno;
        dup2(out, STDOUT_FILENO); dup2(err, STDERR_FILENO); close(out); close(err); fclose(capture);
        return km_error(ui->km, "Cannot redirect command output: %s", strerror(saved));
    }
    int result = km_dispatch(ui->km, argc, argv);
    fflush(stdout); fflush(stderr); dup2(out, STDOUT_FILENO); dup2(err, STDERR_FILENO); close(out); close(err);
    rewind(capture);
    size_t length = fread(output, 1, size - 1, capture);
    output[length] = '\0'; *truncated = fgetc(capture) != EOF;
    for (size_t i = 0; i < length; i++) if (!isprint((unsigned char)output[i]) && output[i] != '\n' && output[i] != '\t') output[i] = ' ';
    fclose(capture);
    return result;
}

static void command_notice(KmUi *ui, int result) {
    size_t first = strcspn(ui->output, "\n"), lines = 0;
    for (const char *c = ui->output; *c; c++) if (*c == '\n' && c[1]) lines++;
    char line[256];
    snprintf(line, sizeof(line), "%.*s", (int)(first < sizeof(line) - 1 ? first : sizeof(line) - 1), ui->output);
    if (result) { ui_notice(ui, "%.480s", *ui->km->error ? ui->km->error : *line ? line : "Action failed; run the CLI for details."); ui->notice_error = true; }
    else if (!*line) ui_notice(ui, "Saved.");
    else if (lines) ui_notice(ui, "%s (+%zu more lines)", line, lines);
    else ui_notice(ui, "%s", line);
}

int ui_command(KmUi *ui, int argc, char **argv) {
    int result = ui_capture(ui, argc, argv, ui->output, sizeof(ui->output), &ui->truncated);
    ui->sync_deferred = result && ui->km->sync_pending;
    command_notice(ui, result);
    if (ui->sync_deferred) snprintf(ui->sync_error, sizeof(ui->sync_error), "%s", ui->km->error);
    char error[sizeof(ui->km->error)]; snprintf(error, sizeof(error), "%s", ui->km->error);
    if (km_load(ui->km) < 0) ui_notice(ui, "%.480s", ui->km->error);
    else if (!ui->sync_deferred) ui_check_sync(ui);
    snprintf(ui->km->error, sizeof(ui->km->error), "%s", error);
    ui->rows_stale = true; ui_catalog_changed(ui);
    return result;
}

static const char *text_line(const char *text, int row, int width) {
    size_t length = strcspn(text, "\n"), take = length < (size_t)width ? length : (size_t)width;
    if (take < length) {
        size_t space = take;
        while (space && text[space] != ' ') space--;
        if (space > (size_t)width / 2) take = space;
    }
    if (row >= 0) mvaddnstr(row, 3, text, (int)take);
    text += take;
    if (*text == '\n') text++;
    else if (take < length) while (*text == ' ') text++;
    return text;
}

const char *ui_text_lines(const char *text, int row, int rows, int offset) {
    int width = COLS > 6 ? COLS - 6 : 1;
    for (int i = 0; i < offset && *text; i++) text = text_line(text, -1, width);
    for (int i = 0; i < rows && *text; i++) text = text_line(text, row + i, width);
    return text;
}

static bool review_output(KmUi *ui, const char *title) {
    int offset = 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) return false;
        ui_dialog_title(title);
        const char *line = ui_text_lines(ui->output, 5, LINES - 9, offset);
        static const char *const keys[] = {"j/k", "scroll", "enter", "apply", "esc", "cancel"};
        if (ui->truncated) ui_put(LINES - 2, 3, UI_STYLE_WARN, "Output truncated. Inspect full output with the CLI. Esc returns.", COLS - 6);
        else hint(LINES - 2, keys, 3);
        refresh(); int key = getch();
        if (key == 27 || key == 'q') return false;
        if ((key == '\n' || key == KEY_ENTER) && !ui->truncated) return true;
        if (key == KEY_DOWN || key == 'j') { if (*line) offset++; }
        if ((key == KEY_UP || key == 'k') && offset) offset--;
    }
    return false;
}

bool ui_review(KmUi *ui, const char *title) { return review_output(ui, title); }

void ui_view(KmUi *ui, const char *title) { ui_document_view(ui, title, ui->output, false); }

static int toggle_target(KmUi *ui, const char *key, bool removing, int *done) {
    bool group = key[0] == 'g'; const char *id = key + 2;
    json_object *item = group ? ui_group_item(ui, id) : ui_item(ui, id);
    if (!item) return 0;
    json_object *flag = km_get(item, "enabled");
    char *verb = removing ? "remove" : (!flag || json_object_get_boolean(flag) ? "disable" : "enable");
    char *args[] = {(char *)ui_kind(ui), "group", verb, (char *)id};
    if (!group) { args[1] = verb; args[2] = (char *)id; }
    int result = ui_command(ui, group ? 4 : 3, args);
    if (!result || ui->sync_deferred) {
        (*done)++;
        if (!ui->sync_deferred) ui_notice(ui, "Saved: %s %sd.", id, verb);
    }
    return result && !ui->sync_deferred ? -1 : 0;
}

/* Marked rows hidden by a collapsed group or search are still selected, so act on
 * the marks themselves rather than the visible rows. */
static void toggle_items(KmUi *ui, bool removing) {
    if (ui->tab >= UI_CLIENTS) return;
    size_t marked = (size_t)json_object_object_length(ui->marks);
    if (!marked && !ui_current(ui)) return;
    char confirmation[128];
    if (marked) snprintf(confirmation, sizeof(confirmation), "Remove %zu selected groups/items? Group members remain installed.", marked);
    else snprintf(confirmation, sizeof(confirmation), "%s", ui_group_selected(ui) ? "Remove this group? Members remain installed." : "Remove this item?");
    if (removing && !ui_confirm(ui, confirmation)) return;
    json_object *targets = json_object_get(ui->marks);
    if (!marked) {
        json_object_put(targets); targets = json_object_new_object();
        char key[80]; ui_mark_key(ui_current_row(ui), key, sizeof(key));
        json_object_object_add(targets, key, json_object_new_boolean(true));
    }
    int done = 0;
    json_object_object_foreach(targets, key, value) {
        (void)value;
        if (toggle_target(ui, key, removing, &done) < 0) break;
    }
    json_object_put(targets);
    if (done > 1 && !*ui->sync_error) ui_notice(ui, "Saved: %d items updated.", done);
    json_object_put(ui->marks); ui->marks = json_object_new_object(); ui->rows_stale = true;
}

static bool has_agent(json_object *array, const char *agent) {
    for (size_t i = 0; array && i < json_object_array_length(array); i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(array, i)), agent)) return true;
    return false;
}

static bool assign_dialog(KmUi *ui, const char *id, bool group, bool globally_off, bool *choices) {
    int row = 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) return false;
        static const char *const keys[] = {"j/k", "move", "space", "toggle", "enter", "save", "esc", "cancel"};
        ui_dialog_title("Assign agents"); ui_put(5, 3, UI_STYLE_STRONG, id, COLS - 6);
        ui_put(6, 3, UI_STYLE_MUTED, group ? "Only this resource type; individual OFF overrides still win." :
               "Changed rows become explicit overrides; unchanged rows keep inheriting.", COLS - 6);
        for (int a = 0; a < KM_AGENT_COUNT; a++) {
            bool compatible = group || ui->tab != UI_SUBAGENTS || km_subagent_compatible(ui_item(ui, id), km_agents[a], NULL, 0);
            char line[160]; snprintf(line, sizeof(line), " %s [%c] %s%s", row == a ? ui_glyph(UI_GLYPH_CURSOR) : " ",
                                     choices[a] ? 'x' : ' ', km_agents[a], compatible ? "" : " (native variant required)");
            if (row == a) { attron(ui_style(UI_STYLE_SELECT)); mvhline(8 + a, 2, ' ', COLS - 4); attroff(ui_style(UI_STYLE_SELECT)); }
            ui_put(8 + a, 2, row == a ? UI_STYLE_SELECT : choices[a] ? UI_STYLE_STRONG : UI_STYLE_TEXT, line, COLS - 4);
        }
        if (globally_off) ui_put(9 + KM_AGENT_COUNT, 3, UI_STYLE_WARN, "Globally OFF: choices take effect after e enables this item.", COLS - 6);
        hint(11 + KM_AGENT_COUNT, keys, 4); refresh();
        int key = getch();
        if (key == 27) return false;
        if (key == '\n' || key == KEY_ENTER) return true;
        if (key == KEY_DOWN || key == 'j') row = (row + 1) % KM_AGENT_COUNT;
        if (key == KEY_UP || key == 'k') row = (row + KM_AGENT_COUNT - 1) % KM_AGENT_COUNT;
        if (key == ' ') choices[row] = !choices[row];
    }
    return false;
}

static int save_assignments(KmUi *ui, const char *id, bool group, const bool *before, const bool *choices, int *changes) {
    if (group) {
        if (!memcmp(before, choices, sizeof(bool) * KM_AGENT_COUNT)) return 0;
        char csv[64] = "";
        for (int a = 0; a < KM_AGENT_COUNT; a++) if (choices[a]) { if (*csv) strcat(csv, ","); strcat(csv, km_agents[a]); }
        char *args[] = {(char *)ui_kind(ui), "group", "assign", (char *)id, "--agents", csv};
        (*changes)++; return ui_command(ui, 6, args);
    }
    int result = 0;
    for (int a = 0; a < KM_AGENT_COUNT; a++) {
        if (choices[a] == before[a]) continue;
        char *args[] = {(char *)ui_kind(ui), choices[a] ? "enable" : "disable", (char *)id, "--agent", (char *)km_agents[a]};
        (*changes)++; result = ui_command(ui, 5, args);
        if (result && !ui->sync_deferred) return result;
    }
    return result;
}

/* Seeds from assignments (ignoring global OFF) and writes only rows the user changed,
 * so unchanged clients keep following their groups. */
void ui_assign(KmUi *ui) {
    const char *current = ui_current(ui);
    if (ui->tab >= UI_CLIENTS || !current) return;
    char id[65]; snprintf(id, sizeof(id), "%s", current);
    bool group = ui_group_selected(ui), before[KM_AGENT_COUNT], choices[KM_AGENT_COUNT];
    json_object *item = group ? ui_group_item(ui, id) : ui_item(ui, id), *flag = km_get(item, "enabled");
    bool globally_off = !group && flag && !json_object_get_boolean(flag);
    for (int a = 0; a < KM_AGENT_COUNT; a++)
        before[a] = choices[a] = group ? has_agent(km_get(item, "agents"), km_agents[a]) : km_assigned(ui->km, ui_kind(ui), id, km_agents[a]);
    if (!assign_dialog(ui, id, group, globally_off, choices)) return;
    int changes = 0, result = save_assignments(ui, id, group, before, choices, &changes);
    if (!changes) ui_notice(ui, "Assignments unchanged.");
    else if (!result) ui_notice(ui, "Assignments saved. Agent reload may be required.");
    else if (ui->sync_deferred) ui_notice(ui, "Assignments saved; native sync pending (see the banner or Clients tab).");
    ui->rows_stale = true;
}

static void show_help(KmUi *ui) {
    if (!ui_modal_ready()) return;
    ui_dialog_title("Keyboard shortcuts");
    static const char *const keys[][2] = {
        {"1-5, Tab", "MCPs, Skills, Subagents, Clients, Status"}, {"j/k, arrows", "Move selection or scroll Status"},
        {"Enter, Left, Right", "Expand groups; Enter shows item details"}, {"Space", "Select rows for bulk actions"},
        {"e, a, v", "Toggle availability, assign clients, open details"}, {"g, n, x, d", "New group, add, edit or rename, remove"},
        {"m, /, Esc", "Group members or item groups, search, clear or cancel"}, {"r, s, i", "Reconnect MCP, preview sync, preview import"},
        {"p, u, b", "Duplicate subagent, update skill, roll back skill"}, {"f, c, q", "Refresh Status or Clients, doctor, quit"}};
    int y = 5;
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys) && y < LINES - 6; i++, y++) {
        ui_put(y, 3, UI_STYLE_STRONG, keys[i][0], 22); ui_put(y, 26, UI_STYLE_MUTED, keys[i][1], COLS - 29);
    }
    const char *notes[] = {"Groups show effective client availability; a half symbol or [-] means mixed.",
                           "Explicit and global OFF win over groups. Coding agents may need a reload."};
    for (size_t i = 0; i < 2 && y + 1 < LINES - 3; i++) ui_put(++y, 3, UI_STYLE_MUTED, notes[i], COLS - 6);
    ui_put(LINES - 2, 3, UI_STYLE_STRONG, "Press any key to return.", COLS - 6);
    refresh(); while (getch() == ERR && !km_ui_interrupted) {} (void)ui;
}

static void skill_action(KmUi *ui, bool rollback) {
    if (ui->tab != UI_SKILLS || !ui_resource_selected(ui)) return;
    char *id = strdup(ui_current(ui));
    if (!id) return;
    char *args[] = {"skill", rollback ? "rollback" : "update", id, "--preview"};
    if (rollback) { if (ui_confirm(ui, "Restore this skill's previous pinned revision?")) ui_command(ui, 3, args); }
    else {
        /* A current skill has nothing to apply; report it instead of offering a no-op review. */
        if (!ui_command(ui, 4, args) && !strstr(ui->output, " is already current") &&
            ui_review(ui, "Review skill update")) { args[3] = "--apply"; ui_command(ui, 4, args); }
    }
    free(id); ui_rebuild(ui);
}

void ui_action(KmUi *ui, int key) {
    if (key == 'e') toggle_items(ui, false);
    else if (key == 'd') toggle_items(ui, true);
    else if (key == 'a') ui_assign(ui);
    else if (key == 'm') ui_members(ui);
    else if (key == 'g') ui_add_group(ui, false);
    else if (key == 'x' && ui_group_selected(ui)) ui_add_group(ui, true);
    else if (key == 'n' || key == 'x') ui_add(ui, key == 'x');
    else if (key == 'p') ui_duplicate(ui);
    else if (key == 'r') ui_reconnect(ui);
    else if (key == 'f' && ui->tab == UI_STATUS) ui_refresh_status(ui);
    else if (key == 'f' && ui->tab == UI_CLIENTS) { ui_clients_refresh(ui); ui_notice(ui, "Client state refreshed."); }
    else if (key == 's' || key == 'i') ui_sync(ui, key == 'i');
    else if (key == 'u' || key == 'b') skill_action(ui, key == 'b');
    else if (key == 'c') { char *args[] = {"doctor"}; ui_command(ui, 1, args); ui_view(ui, "Configuration doctor"); }
    else if (key == '?') show_help(ui);
}
