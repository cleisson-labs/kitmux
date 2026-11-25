#include "tui.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Palette: red (brand, focus, warnings), grey (secondary text, OFF, borders), and
 * the terminal's own foreground/background as white/black so light and dark
 * themes both stay readable. NO_COLOR or monochrome terminals use attributes. */
enum { PAIR_ACCENT = 1, PAIR_MUTED, PAIR_SELECT };

static bool colors, grey_color, unicode;

void ui_theme_init(bool enabled) {
    colors = enabled && has_colors();
    unicode = MB_CUR_MAX > 1;
    if (!colors) return;
    start_color(); use_default_colors();
    bool wide = COLORS >= 256;
    short red = wide ? 196 : COLOR_RED, black = wide ? 16 : COLOR_BLACK;
    short grey = wide ? 244 : COLORS >= 16 ? 8 : -1;
    grey_color = grey >= 0;
    init_pair(PAIR_ACCENT, red, -1);
    init_pair(PAIR_MUTED, grey_color ? grey : -1, -1);
    init_pair(PAIR_SELECT, black, red);
}

int ui_style(KmUiStyle style) {
    switch (style) {
    case UI_STYLE_STRONG: return A_BOLD;
    case UI_STYLE_ACCENT: return A_BOLD | (colors ? COLOR_PAIR(PAIR_ACCENT) : 0);
    case UI_STYLE_MUTED: return colors && grey_color ? COLOR_PAIR(PAIR_MUTED) : A_DIM;
    case UI_STYLE_SELECT: return A_BOLD | (colors ? COLOR_PAIR(PAIR_SELECT) : A_REVERSE);
    case UI_STYLE_WARN: return A_BOLD | (colors ? COLOR_PAIR(PAIR_ACCENT) : 0);
    default: return A_NORMAL;
    }
}

const char *ui_glyph(KmUiGlyph glyph) {
    static const char *const fancy[] = {"■", "·", "◐", "!", "▾", "▸", "›", "•"};
    static const char *const plain[] = {"[x]", "[ ]", "[-]", "[!]", "v", ">", ">", "*"};
    return (unicode ? fancy : plain)[glyph];
}

/* Short column labels; unknown clients fall back to their upper-cased ID. */
const char *ui_client_label(const char *agent) {
    static const char *const ids[] = {"codex", "claude", "opencode", "antigravity"};
    static const char *const labels[] = {"CODEX", "CLAUDE", "OPENCODE", "AGY"};
    static char fallback[16];
    for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); i++) if (!strcmp(agent, ids[i])) return labels[i];
    size_t i = 0;
    for (; agent[i] && i + 1 < sizeof(fallback); i++) fallback[i] = (char)toupper((unsigned char)agent[i]);
    fallback[i] = '\0';
    return fallback;
}

void ui_put(int y, int x, KmUiStyle style, const char *text, int width) {
    attron(ui_style(style)); mvaddnstr(y, x, text, width); attroff(ui_style(style));
}

/* Draws "key description" pairs with strong keys and grey descriptions. */
int ui_hints(int y, int x, const char *const *pairs, int count) {
    for (int i = 0; i + 1 < count * 2 && x < COLS - 4; i += 2) {
        int need = (int)(strlen(pairs[i]) + strlen(pairs[i + 1])) + 3;
        if (x + need > COLS - 2) { y++; x = 2; if (y >= LINES - 1) break; }
        ui_put(y, x, UI_STYLE_STRONG, pairs[i], COLS - x - 2); x += (int)strlen(pairs[i]) + 1;
        ui_put(y, x, UI_STYLE_MUTED, pairs[i + 1], COLS - x - 2); x += (int)strlen(pairs[i + 1]) + 2;
    }
    return y;
}

void ui_rule(int y, int x, int width) {
    attron(ui_style(UI_STYLE_MUTED)); mvhline(y, x, ACS_HLINE, width); attroff(ui_style(UI_STYLE_MUTED));
}

/* Word-wraps one line of text; returns the rows used (at least one). */
int ui_wrapped(int y, int x, int width, KmUiStyle style, const char *text, int rows) {
    int used = 0;
    do {
        size_t length = strlen(text), take = length < (size_t)width ? length : (size_t)width;
        if (take < length) { size_t space = take; while (space && text[space] != ' ') space--; if (space > (size_t)width / 2) take = space; }
        if (used < rows) ui_put(y + used, x, style, text, (int)take);
        used++; text += take;
        while (*text == ' ') text++;
    } while (*text && used < rows);
    return used;
}
