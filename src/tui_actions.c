#include "tui.h"
#include "subagents.h"
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void ui_notice(KmUi *ui, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(ui->notice, sizeof(ui->notice), format, arguments);
    va_end(arguments);
}

static void dialog_title(const char *title) {
    erase(); attron(A_BOLD); mvprintw(2, 3, "%s", title); attroff(A_BOLD);
    mvhline(3, 3, ACS_HLINE, COLS > 6 ? COLS - 6 : 1);
}

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
        dialog_title(title); mvprintw(5, 3, "%.120s", label);
        mvaddnstr(7, 3, value, COLS > 8 ? COLS - 8 : 1);
        mvprintw(10, 3, "Enter confirms. Esc cancels."); refresh();
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
    dialog_title("Confirm change"); mvaddnstr(5, 3, message, COLS - 6);
    mvaddstr(8, 3, "y: confirm   any other key: cancel"); refresh();
    int key;
    do { key = getch(); } while ((key == ERR || key == KEY_RESIZE) && !km_ui_interrupted);
    (void)ui; return key == 'y' || key == 'Y';
}

int ui_choice(const char *title, const char *const *choices, int count, int initial) {
    int row = initial >= 0 && initial < count ? initial : 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) return -1;
        dialog_title(title);
        for (int i = 0; i < count; i++) {
            if (i == row) attron(A_BOLD | A_REVERSE);
            mvprintw(6 + i * 2, 3, "%c %.*s", i == row ? '>' : ' ', COLS - 8, choices[i]);
            if (i == row) attroff(A_BOLD | A_REVERSE);
        }
        mvaddstr(7 + count * 2, 3, "j/k or arrows   Enter choose   Esc cancel"); refresh();
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
    if (result) ui_notice(ui, "%.480s", *ui->km->error ? ui->km->error : *line ? line : "Action failed; run the CLI for details.");
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
        dialog_title(title);
        const char *line = ui_text_lines(ui->output, 5, LINES - 9, offset);
        const char *hint = "j/k scroll   Enter apply   Esc cancel";
        mvaddnstr(LINES - 2, 3, ui->truncated ? "Output truncated. Inspect full output with the CLI. Esc returns." : hint, COLS - 6);
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
        dialog_title("Assign agents"); mvprintw(5, 3, "%s", id);
        mvaddstr(6, 3, group ? "Only this resource type; individual OFF overrides still win." :
                 "Changed rows become explicit overrides; unchanged rows keep inheriting.");
        for (int a = 0; a < KM_AGENT_COUNT; a++) {
            bool compatible = group || ui->tab != UI_SUBAGENTS || km_subagent_compatible(ui_item(ui, id), km_agents[a], NULL, 0);
            mvprintw(7 + a, 3, "%c [%c] %s%s", row == a ? '>' : ' ', choices[a] ? 'x' : ' ', km_agents[a], compatible ? "" : " (native variant required)");
        }
        if (globally_off) mvaddstr(10, 3, "Globally OFF: choices take effect after e enables this item.");
        mvprintw(12, 3, "j/k move   Space toggle   Enter save   Esc cancel"); refresh();
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
    dialog_title("Kitmux keyboard shortcuts");
    const char *lines[] = {"1-5 / Tab   MCPs, Skills, Subagents, Clients, Status", "j/k / arrows   Move selection or scroll Status", "Enter / Left / Right   Expand groups; Enter shows item details", "Space   Select rows; repeated items are selected once", "e Toggle availability   a Assign clients   v Open details", "g New group  n Add item  x Edit/rename  d Remove", "m Group members or item groups   / Search   Esc Clear/cancel", "r Reconnect MCP   s Preview/adopt native configuration", "i Import (Subagents: profiles only)  p Duplicate subagent", "u Preview/apply skill update   b Roll back skill", "f Capture Status observation   c Doctor   q Quit", "", "Group clients show effective availability; [-] means mixed.", "Another group can enable an item. Explicit/global OFF wins.", "Your coding agent may still need a refresh or reload.", "Press any key to return."};
    size_t count = sizeof(lines) / sizeof(*lines);
    for (size_t i = 0; i + 1 < count && (int)i + 5 < LINES - 3; i++) mvaddnstr(5 + (int)i, 3, lines[i], COLS - 6);
    mvaddnstr(LINES - 2, 3, lines[count - 1], COLS - 6);
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
