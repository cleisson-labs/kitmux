#include "tui.h"
#include "subagents.h"
#include <string.h>

/* Column geometry derived from the client list, so adding a client needs no
 * drawing changes. The name column absorbs any extra terminal width. */
typedef struct {
    int name, name_width, state, client[KM_AGENT_COUNT], width[KM_AGENT_COUNT];
} Layout;

/* Longest visible label; group rows add their expand symbol and member count. */
static int longest_name(const KmUi *ui) {
    int longest = 0;
    for (int i = 0; i < ui->count; i++) {
        int length = (int)strlen(ui->rows[i].id) + (ui->rows[i].type == UI_RESOURCE ? 0 : 7);
        if (length > longest) longest = length;
    }
    return longest;
}

/* The name column fits the longest visible name, so state and client columns stay
 * close to the names they describe instead of drifting to the far edge. */
static Layout layout(const KmUi *ui) {
    Layout l = {.name = 8};
    int clients = 0;
    for (int a = 0; a < KM_AGENT_COUNT; a++) {
        int label = (int)strlen(ui_client_label(km_agents[a]));
        l.width[a] = label < 3 ? 3 : label;
        clients += l.width[a] + 2;
    }
    int available = COLS - 2 - clients - l.name - 7, wanted = longest_name(ui) + 2;
    available = available < 18 ? 18 : available > 64 ? 64 : available;
    l.name_width = wanted < 18 ? 18 : wanted > available ? available : wanted;
    l.state = l.name + l.name_width + 1;
    for (int a = 0, x = l.state + 7; a < KM_AGENT_COUNT; x += l.width[a] + 2, a++) l.client[a] = x;
    return l;
}

static size_t array_length(json_object *array) {
    return json_object_is_type(array, json_type_array) ? json_object_array_length(array) : 0;
}

static bool grouped(KmUi *ui, const char *id) {
    char reference[80]; snprintf(reference, sizeof(reference), "%s:%s", ui_kind(ui), id);
    json_object_object_foreach(km_get(ui->km->catalog, "groups"), key, group) {
        (void)key;
        const char *kind = km_string(group, "kind");
        if (!kind || strcmp(kind, ui_kind(ui))) continue;
        json_object *members = km_get(group, "members");
        for (size_t m = 0; m < array_length(members); m++)
            if (!strcmp(json_object_get_string(json_object_array_get_idx(members, m)), reference)) return true;
    }
    return false;
}

static size_t member_count(KmUi *ui, const KmUiRow *row) {
    if (row->type == UI_GROUP) return array_length(km_get(ui_group_item(ui, row->id), "members"));
    size_t count = 0;
    json_object_object_foreach(km_resources(ui->km, ui_kind(ui)), id, item) { (void)item; if (!grouped(ui, id)) count++; }
    return count;
}

static KmUiGlyph group_cell(KmUi *ui, json_object *group, const char *agent) {
    json_object *members = km_get(group, "members"); size_t on = 0, count = array_length(members);
    for (size_t i = 0; i < count; i++) {
        const char *id = strchr(json_object_get_string(json_object_array_get_idx(members, i)), ':');
        if (id && km_effective(ui->km, ui_kind(ui), id + 1, agent, NULL, 0)) on++;
    }
    return !on ? UI_GLYPH_OFF : on == count ? UI_GLYPH_ON : UI_GLYPH_MIXED;
}

static KmUiGlyph resource_cell(KmUi *ui, json_object *item, const char *id, const char *agent) {
    if (ui->tab == UI_SUBAGENTS && !km_subagent_compatible(item, agent, NULL, 0)) return UI_GLYPH_UNUSABLE;
    return km_effective(ui->km, ui_kind(ui), id, agent, NULL, 0) ? UI_GLYPH_ON : UI_GLYPH_OFF;
}

static KmUiStyle cell_style(KmUiGlyph glyph) {
    return glyph == UI_GLYPH_ON ? UI_STYLE_STRONG : glyph == UI_GLYPH_UNUSABLE ? UI_STYLE_WARN :
           glyph == UI_GLYPH_MIXED ? UI_STYLE_TEXT : UI_STYLE_MUTED;
}

static void put_name(int y, int x, KmUiStyle style, const char *name, int width) {
    char text[128];
    if ((int)strlen(name) <= width) snprintf(text, sizeof(text), "%s", name);
    else snprintf(text, sizeof(text), "%.*s~", width - 1, name);
    ui_put(y, x, style, text, width);
}

static KmUiStyle pick(bool selected, KmUiStyle style) { return selected ? UI_STYLE_SELECT : style; }

/* Symbols are one cell wide; ASCII fallbacks such as "[x]" are wider. */
static int glyph_cells(const char *text) { return (unsigned char)text[0] >= 0x80 ? 1 : (int)strlen(text); }

static void draw_cells(KmUi *ui, const Layout *l, const KmUiRow *row, json_object *item, int y, bool selected) {
    for (int a = 0; a < KM_AGENT_COUNT; a++) {
        KmUiGlyph glyph = row->type == UI_GROUP ? group_cell(ui, item, km_agents[a]) : resource_cell(ui, item, row->id, km_agents[a]);
        const char *text = ui_glyph(glyph);
        ui_put(y, l->client[a] + (l->width[a] - glyph_cells(text)) / 2, pick(selected, cell_style(glyph)), text, 4);
    }
}

/* Writes the name or group label and reports whether a group has mixed members. */
static bool draw_name_column(KmUi *ui, const Layout *l, const KmUiRow *row, json_object *item, int y, bool selected, bool enabled) {
    if (row->type == UI_RESOURCE) {
        put_name(y, l->name, pick(selected, enabled ? UI_STYLE_TEXT : UI_STYLE_MUTED), row->id, l->name_width);
        return false;
    }
    char label[96];
    snprintf(label, sizeof(label), "%s %s (%zu)", ui_glyph(ui_is_collapsed(ui, row) ? UI_GLYPH_COLLAPSED : UI_GLYPH_EXPANDED),
             row->id, member_count(ui, row));
    put_name(y, l->name - 2, pick(selected, UI_STYLE_STRONG), label, l->name_width + 2);
    bool mixed = false;
    for (int a = 0; row->type == UI_GROUP && a < KM_AGENT_COUNT; a++)
        if (group_cell(ui, item, km_agents[a]) == UI_GLYPH_MIXED) mixed = true;
    return mixed;
}

/* A selected row is one red bar; otherwise ON reads strong and OFF recedes to grey. */
static void draw_row(KmUi *ui, const Layout *l, int i, int y) {
    const KmUiRow *row = &ui->rows[i]; bool selected = i == ui->row;
    json_object *item = row->type == UI_GROUP ? ui_group_item(ui, row->id) : ui_item(ui, row->id);
    json_object *flag = km_get(item, "enabled");
    bool enabled = !flag || json_object_get_boolean(flag);
    if (selected) {
        attron(ui_style(UI_STYLE_SELECT)); mvhline(y, 1, ' ', COLS - 2); attroff(ui_style(UI_STYLE_SELECT));
        ui_put(y, 2, UI_STYLE_SELECT, ui_glyph(UI_GLYPH_CURSOR), 2);
    }
    char key[80]; ui_mark_key(row, key, sizeof(key));
    if (row->type != UI_UNGROUPED && km_get(ui->marks, key)) ui_put(y, 4, pick(selected, UI_STYLE_ACCENT), ui_glyph(UI_GLYPH_MARK), 2);
    bool mixed = draw_name_column(ui, l, row, item, y, selected, enabled);
    if (row->type == UI_UNGROUPED) return;
    ui_put(y, l->state, pick(selected, enabled ? UI_STYLE_STRONG : UI_STYLE_MUTED), !enabled ? "OFF" : mixed ? "MIX" : "ON", 5);
    draw_cells(ui, l, row, item, y, selected);
}

static int draw_resource_clients(KmUi *ui, json_object *item, const char *id, int y) {
    for (int a = 0; a < KM_AGENT_COUNT && y < LINES - 5; a++, y++) {
        char reason[192] = "";
        bool compatible = ui->tab != UI_SUBAGENTS || km_subagent_compatible(item, km_agents[a], reason, sizeof(reason));
        bool on = compatible && km_effective(ui->km, ui_kind(ui), id, km_agents[a], reason, sizeof(reason));
        ui_put(y, 4, UI_STYLE_TEXT, km_agents[a], 12);
        ui_put(y, 17, on ? UI_STYLE_STRONG : UI_STYLE_MUTED, !compatible ? "N/A" : on ? "ON" : "OFF", 4);
        ui_put(y, 22, UI_STYLE_MUTED, reason, COLS - 26);
    }
    return y;
}

static void draw_details(KmUi *ui, int y) {
    const char *id = ui_current(ui);
    json_object *item = ui_current_item(ui);
    if (!id || !item || y >= LINES - 6) return;
    ui_rule(y++, 2, COLS - 4);
    ui_put(y++, 2, UI_STYLE_STRONG, id, COLS - 4);
    if (ui_group_selected(ui)) {
        const char *lines[] = {"Clients show effective member availability; a half symbol or [-] means mixed.",
                               "Other groups can enable members. Explicit and global OFF still win.",
                               "m choose members   a group clients   e group availability"};
        for (size_t i = 0; i < 3 && y < LINES - 5; i++) ui_put(y++, 4, UI_STYLE_MUTED, lines[i], COLS - 8);
    } else y = draw_resource_clients(ui, item, id, y);
    if (ui->tab == UI_SUBAGENTS && y < LINES - 5)
        ui_put(y, 4, UI_STYLE_MUTED, "Native reload required after changes; running work stays native.", COLS - 8);
}

void ui_draw_list(KmUi *ui) {
    Layout l = layout(ui);
    ui_put(6, l.name - 2, UI_STYLE_MUTED, "NAME", l.name_width);
    ui_put(6, l.state, UI_STYLE_MUTED, "STATE", 6);
    for (int a = 0; a < KM_AGENT_COUNT; a++) ui_put(6, l.client[a], UI_STYLE_MUTED, ui_client_label(km_agents[a]), l.width[a]);
    int visible = ui->details ? (LINES - 15) / 2 : LINES - 13;
    if (ui->row >= ui->top + visible) ui->top = ui->row - visible + 1;
    for (int i = ui->top; i < ui->count && i < ui->top + visible; i++) draw_row(ui, &l, i, 8 + i - ui->top);
    if (!ui->count) {
        char empty[160];
        if (*ui->filter) snprintf(empty, sizeof(empty), "No matches for \"%.40s\". Esc clears the search.", ui->filter);
        else snprintf(empty, sizeof(empty), "Nothing here yet. Press n to add, or i to import.");
        ui_put(9, 6, UI_STYLE_MUTED, empty, COLS - 10);
    }
    if (ui->details) draw_details(ui, 8 + visible);
}
