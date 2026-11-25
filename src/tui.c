#include <signal.h>
#include "tui.h"
#include "subagents.h"
#include <ctype.h>
#include <locale.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

volatile sig_atomic_t km_ui_interrupted;
static const char *const tabs[] = {"MCPs", "Skills", "Subagents", "Clients", "Status"};

/* Each tab label is one run of text, so "1 MCPs" stays searchable in tests and logs. */
static void draw_tabs(KmUi *ui) {
    int x = 2;
    for (int i = 0; i < UI_TAB_COUNT; i++) {
        char label[32]; snprintf(label, sizeof(label), " %d %s ", i + 1, tabs[i]);
        ui_put(3, x, i == ui->tab ? UI_STYLE_SELECT : UI_STYLE_TEXT, label, COLS - x - 2);
        x += (int)strlen(label) + 1;
    }
    ui_rule(4, 2, COLS - 4);
}

/* Clients text marks each client's first line and pending-sync lines for styling. */
static KmUiStyle client_line_style(const char *line) {
    if (!strncmp(line, "  Native sync pending", 21)) return UI_STYLE_WARN;
    if (!strncmp(line, "  ", 2) || !*line) return UI_STYLE_MUTED;
    for (int a = 0; a < KM_AGENT_COUNT; a++) {
        size_t length = strlen(km_agents[a]);
        if (!strncmp(line, km_agents[a], length) && line[length] == ':') return UI_STYLE_STRONG;
    }
    return UI_STYLE_MUTED;
}

static void draw_clients(KmUi *ui) {
    ui_put(6, 3, UI_STYLE_STRONG, "Native clients", COLS - 6);
    ui_put(6, 18, UI_STYLE_MUTED, "adoption and native configuration state", COLS - 22);
    ui_rule(7, 3, COLS - 6);
    int y = 8;
    for (const char *line = ui->clients; *line && y < LINES - 5;) {
        size_t length = strcspn(line, "\n");
        char text[1024]; snprintf(text, sizeof(text), "%.*s", (int)(length < sizeof(text) - 1 ? length : sizeof(text) - 1), line);
        KmUiStyle style = client_line_style(text);
        char *colon = style == UI_STYLE_STRONG ? strchr(text, ':') : NULL;
        if (colon) {   /* bold client name; the rest reads as normal or grey text */
            *colon = '\0'; ui_put(y, 3, UI_STYLE_STRONG, text, COLS - 6);
            int x = 3 + (int)strlen(text);
            ui_put(y, x, strstr(colon + 1, "not adopted") ? UI_STYLE_MUTED : UI_STYLE_TEXT, ":", 1);
            y += ui_wrapped(y, x + 1, COLS - 6 - x, strstr(colon + 1, "not adopted") ? UI_STYLE_MUTED : UI_STYLE_TEXT, colon + 1, LINES - 5 - y);
        } else y += ui_wrapped(y, 3, COLS - 6, style, text, LINES - 5 - y);
        line += length + (line[length] == '\n');
    }
}

void ui_refresh_status(KmUi *ui) {
    char *args[] = {"status"};
    if (ui_command(ui, 1, args)) return;
    snprintf(ui->snapshot, sizeof(ui->snapshot), "%s", ui->output);
    ui->snapshot_at = time(NULL); ui->snapshot_top = 0; ui->snapshot_truncated = ui->truncated;
    ui_notice(ui, "Captured runtime observation. Native client refresh remains unverified.");
}

static void draw_status(KmUi *ui) {
    ui_put(6, 3, UI_STYLE_STRONG, "Runtime snapshot", COLS - 6);
    ui_put(6, 20, UI_STYLE_MUTED, "Catalog and upstream observation", COLS - 24);
    char captured[64] = "Not captured yet"; struct tm local;
    if (ui->snapshot_at && localtime_r(&ui->snapshot_at, &local)) strftime(captured, sizeof(captured), "Captured %H:%M:%S", &local);
    ui_put(7, 3, UI_STYLE_MUTED, captured, COLS - 6);
    ui_rule(8, 3, COLS - 6);
    ui->snapshot_more = *ui_text_lines(*ui->snapshot ? ui->snapshot : "No observation available. Press f to retry.", 9, LINES - 15, ui->snapshot_top) != '\0';
    ui_put(LINES - 5, 3, ui->snapshot_truncated ? UI_STYLE_WARN : UI_STYLE_MUTED, ui->snapshot_truncated ?
           "Snapshot truncated. Use kitmux status for full diagnostics." : "Upstream observation only; native client refresh is unverified.", COLS - 6);
}

static void draw_header(KmUi *ui) {
    ui_put(1, 2, UI_STYLE_ACCENT, "kitmux", 6);
    ui_put(1, 9, UI_STYLE_MUTED, KM_VERSION, 12);
    ui_put(1, 18, UI_STYLE_MUTED, MB_CUR_MAX > 1 ? "MCPs · skills · subagents for your coding clients" :
           "MCPs, skills, and subagents for your coding clients", COLS - 20);
    if (*ui->sync_error) {
        char banner[640]; snprintf(banner, sizeof(banner), "! Native sync pending (4: Clients): %s", ui->sync_error);
        ui_put(2, 2, UI_STYLE_WARN, banner, COLS - 4);
    }
    draw_tabs(ui);
}

static void draw_footer(KmUi *ui) {
    if (*ui->filter && ui->tab < UI_CLIENTS) {
        char filter[160]; snprintf(filter, sizeof(filter), "Filter: %.100s", ui->filter);
        ui_put(LINES - 5, 2, UI_STYLE_TEXT, filter, COLS - 26);
    }
    size_t marked = (size_t)json_object_object_length(ui->marks);
    char selected[32]; snprintf(selected, sizeof(selected), "%zu selected", marked);
    if (marked && ui->tab < UI_CLIENTS) ui_put(LINES - 5, COLS - 22, UI_STYLE_ACCENT, selected, 20);
    ui_rule(LINES - 4, 2, COLS - 4);
    static const char *const list[] = {"enter", "details", "space", "select", "e", "toggle", "a", "clients", "m", "groups",
                                       "g", "new group", "n", "add", "x", "edit", "d", "remove", "/", "search", "?", "help", "q", "quit"};
    static const char *const clients[] = {"s", "preview sync", "i", "preview import", "c", "doctor", "f", "refresh",
                                          "1-5", "tabs", "?", "help", "q", "quit"};
    static const char *const status[] = {"j/k", "scroll", "f", "capture", "s", "sync", "c", "doctor",
                                         "1-5", "tabs", "?", "help", "q", "quit"};
    if (ui->tab == UI_STATUS) ui_hints(LINES - 3, 2, status, 7);
    else if (ui->tab == UI_CLIENTS) ui_hints(LINES - 3, 2, clients, 7);
    else ui_hints(LINES - 3, 2, list, 12);
    if (*ui->notice) ui_put(LINES - 1, 2, ui->notice_error ? UI_STYLE_WARN : UI_STYLE_STRONG, ui->notice, COLS - 4);
}

void ui_draw(KmUi *ui) {
    erase();
    if (COLS < 74 || LINES < 20) {
        mvaddnstr(0, 0, "Kitmux needs a terminal of at least 74 columns x 20 rows. q exits.", COLS - 1);
        refresh(); return;
    }
    draw_header(ui);
    if (ui->tab < UI_CLIENTS) ui_draw_list(ui);
    else if (ui->tab == UI_CLIENTS) draw_clients(ui);
    else draw_status(ui);
    draw_footer(ui);
    refresh();
}

static void interrupted(int signal_number) { (void)signal_number; km_ui_interrupted = 1; }

static void change_tab(KmUi *ui, int tab) {
    ui_free_rows(ui); ui->tab = tab; ui->row = ui->top = 0; ui->filter[0] = '\0';
    json_object_put(ui->marks); ui->marks = json_object_new_object(); ui_rebuild(ui);
    if (tab == UI_STATUS) ui_refresh_status(ui);
    if (tab == UI_CLIENTS) ui_clients_refresh(ui);
}

static void handle_key(KmUi *ui, int key) {
    if ((COLS < 74 || LINES < 20) && key != 'q') return;
    if (key == 'q') ui->done = true;
    else if (key >= '1' && key <= '5') change_tab(ui, key - '1');
    else if (key == '\t') change_tab(ui, (ui->tab + 1) % UI_TAB_COUNT);
    else if (key == KEY_DOWN || key == 'j') { if (ui->tab == UI_STATUS) { if (ui->snapshot_more) ui->snapshot_top++; } else if (ui->row + 1 < ui->count) ui->row++; }
    else if (key == KEY_UP || key == 'k') { if (ui->tab == UI_STATUS) { if (ui->snapshot_top) ui->snapshot_top--; } else if (ui->row > 0) ui->row--; }
    else if (key == '\n' || key == KEY_ENTER) {
        if (ui_resource_selected(ui)) ui_resource_details(ui);
        else if (ui_current_row(ui)) ui_expand(ui, ui_is_collapsed(ui, ui_current_row(ui)));
    } else if (key == KEY_LEFT || key == KEY_RIGHT) ui_expand(ui, key == KEY_RIGHT);
    else if (key == 'v') { if (ui_resource_selected(ui)) ui_resource_details(ui); else ui->details = !ui->details; }
    else if (key == ' ' && ui_current(ui)) {
        char id[80]; ui_mark_key(ui_current_row(ui), id, sizeof(id));
        if (km_get(ui->marks, id)) json_object_object_del(ui->marks, id);
        else json_object_object_add(ui->marks, id, json_object_new_boolean(true));
    } else if (key == '/') {
        char filter[sizeof(ui->filter)]; snprintf(filter, sizeof(filter), "%s", ui->filter);
        if (ui_input(ui, "Filter resources", "Search", filter, sizeof(filter))) {
            snprintf(ui->filter, sizeof(ui->filter), "%s", filter);
            ui->row = ui->top = 0; ui_rebuild(ui);
        }
    } else if (key == 27) { ui->filter[0] = '\0'; ui->details = false; ui_notice(ui, "Filter cleared."); ui_rebuild(ui); }
    else if (key != ERR && key != KEY_RESIZE) ui_action(ui, key);
}

/* Keeps the list current when other Kitmux processes change the catalog, and never
 * draws rows that refer to resources or groups from an older catalog. */
static void refresh_catalog(KmUi *ui) {
    if (ui_catalog_changed(ui)) {
        if (km_load(ui->km) < 0) ui_notice(ui, "%.480s", ui->km->error);
        else {
            ui->rows_stale = true; ui_check_sync(ui);
            if (ui->tab == UI_CLIENTS) ui_clients_refresh(ui);
        }
    }
    if (ui->rows_stale) { ui->rows_stale = false; ui_rebuild(ui); }
}

static int prepare_groups(KmUi *ui) {
    if (!km_groups_need_migration(ui->km)) return 0;
    char *args[] = {"group", "migrate", "--preview"};
    int result = ui_command(ui, 3, args);
    if (!result && !ui_review(ui, "Migrate groups into resource tabs")) { ui->done = true; return 0; }
    if (!result) { args[2] = "--apply"; result = ui_command(ui, 3, args); }
    if (result) {
        snprintf(ui->output, sizeof(ui->output), "%s", ui->notice);
        ui_view(ui, "Group migration failed"); ui->done = true;
    }
    return result;
}

int km_tui(Km *km) {
    const char *term = getenv("TERM");
    if (!isatty(STDIN_FILENO) || !term || !strcmp(term, "dumb")) { km_help(stdout); return 0; }
    struct termios original;
    if (tcgetattr(STDIN_FILENO, &original) < 0) return km_error(km, "Cannot read terminal state");
    setlocale(LC_ALL, "");
    KmUi ui = {.km = km, .marks = json_object_new_object(), .collapsed = json_object_new_object()};
    if (!initscr()) { json_object_put(ui.marks); json_object_put(ui.collapsed); return km_error(km, "Cannot initialize the terminal"); }
    cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(100);
    if (!getenv("ESCDELAY")) set_escdelay(25);
    ui.colors = !getenv("NO_COLOR") && has_colors();
    ui_theme_init(ui.colors);
    struct sigaction action = {.sa_handler = interrupted}, old_int, old_term;
    sigemptyset(&action.sa_mask); km_ui_interrupted = 0;
    sigaction(SIGINT, &action, &old_int); sigaction(SIGTERM, &action, &old_term);
    int result = prepare_groups(&ui);
    ui_rebuild(&ui);
    ui_catalog_changed(&ui); ui_check_sync(&ui);
    while (!ui.done && !km_ui_interrupted) {
        refresh_catalog(&ui);
        ui_draw(&ui); handle_key(&ui, getch());
    }
    endwin();
    int restored;
    do { restored = tcsetattr(STDIN_FILENO, TCSANOW, &original); } while (restored < 0 && errno == EINTR);
    sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL);
    ui_free_rows(&ui); json_object_put(ui.marks); json_object_put(ui.collapsed);
    return restored < 0 ? km_error(km, "Cannot restore terminal state") : result;
}
