/* Characters: page encodings and HTML entities to Unicode, Unicode to what
 * CyOS's fonts have (Latin-1, with other scripts spelled in Latin letters),
 * and the Cybiko keyboard to characters. */
#include <string.h>
#include <strings.h>
#include "cyb_internal.h"

/* ---- single-byte character sets, 0x80-0xff ---- */

static const uint16_t cp1252[32] = {
    0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0x8d, 0x17d, 0x8f,
    0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d, 0x17e, 0x178,
};

static const uint16_t cp1251[64] = {
    0x402, 0x403, 0x201a, 0x453, 0x201e, 0x2026, 0x2020, 0x2021, 0x20ac, 0x2030, 0x409, 0x2039, 0x40a, 0x40c, 0x40b, 0x40f,
    0x452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x98, 0x2122, 0x459, 0x203a, 0x45a, 0x45c, 0x45b, 0x45f,
    0xa0, 0x40e, 0x45e, 0x408, 0xa4, 0x490, 0xa6, 0xa7, 0x401, 0xa9, 0x404, 0xab, 0xac, 0xad, 0xae, 0x407,
    0xb0, 0xb1, 0x406, 0x456, 0x491, 0xb5, 0xb6, 0xb7, 0x451, 0x2116, 0x454, 0xbb, 0x458, 0x405, 0x455, 0x457,
};

/* KOI8-R 0xc0-0xff (the letters); 0x80-0xbf are box drawing, except ё Ё */
static const char koi8_letters[] = "юабцдефгхийклмнопярстужвьызшэщчъЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧЪ";

static uint32_t utf8_at(const char *s, int *len)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0xc0) { *len = 1; return p[0]; }
    int n = p[0] >= 0xf0 ? 3 : p[0] >= 0xe0 ? 2 : 1;
    uint32_t cp = p[0] & (0x3f >> n);
    for (int i = 1; i <= n; i++) cp = cp << 6 | (p[i] & 0x3f);
    *len = n + 1;
    return cp;
}

uint32_t cyb_cs_byte(int cs, uint8_t b)
{
    if (b < 0x80) return b;
    switch (cs) {
    case CS_CP1251:
        if (b >= 0xc0) return 0x410 + (b - 0xc0u);
        return cp1251[b - 0x80];
    case CS_KOI8R:
        if (b == 0xa3) return 0x451;
        if (b == 0xb3) return 0x401;
        if (b >= 0xc0) {
            int len, i = 0;
            const char *s = koi8_letters;
            for (int k = 0; k < b - 0xc0; k++) { utf8_at(s, &len); s += len; i++; }
            return utf8_at(s, &len);
        }
        return '?';
    default:
        if (b < 0xa0) return cp1252[b - 0x80];
        return b;
    }
}

int cyb_charset(const char *name)
{
    static const struct { const char *name; int cs; } sets[] = {
        { "utf-8", CS_UTF8 }, { "utf8", CS_UTF8 },
        { "iso-8859-1", CS_LATIN1 }, { "latin1", CS_LATIN1 }, { "windows-1252", CS_LATIN1 },
        { "us-ascii", CS_LATIN1 }, { "iso-8859-15", CS_LATIN1 }, { "cp1252", CS_LATIN1 },
        { "windows-1251", CS_CP1251 }, { "cp1251", CS_CP1251 },
        { "koi8-r", CS_KOI8R }, { "koi8-u", CS_KOI8R },
    };
    for (size_t i = 0; i < sizeof sets / sizeof sets[0]; i++)
        if (!strncasecmp(name, sets[i].name, strlen(sets[i].name))) return sets[i].cs;
    return -1;
}

/* ---- Unicode to Latin-1 ---- */

/* Latin Extended-A (0x100-0x17f) without the accent */
static const char ext_a[] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

static const char *const cyrillic[64] = {   /* 0x410-0x44f */
    "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
    "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "\"", "Y", "'", "E", "Yu", "Ya",
    "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
    "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "\"", "y", "'", "e", "yu", "ya",
};

static const struct { uint16_t cp; const char *s; } others[] = {
    { 0x401, "Yo" }, { 0x451, "yo" }, { 0x404, "Ye" }, { 0x454, "ye" }, { 0x406, "I" }, { 0x456, "i" },
    { 0x407, "Yi" }, { 0x457, "yi" }, { 0x490, "G" }, { 0x491, "g" }, { 0x40e, "U" }, { 0x45e, "u" },
    { 0x2002, " " }, { 0x2003, " " }, { 0x2009, " " }, { 0x200a, " " }, { 0x202f, " " },
    { 0x200b, "" }, { 0x200c, "" }, { 0x200d, "" }, { 0x200e, "" }, { 0x200f, "" }, { 0xfeff, "" },
    { 0xad, "" }, { 0x2010, "-" }, { 0x2011, "-" }, { 0x2012, "-" }, { 0x2013, "-" }, { 0x2014, "-" },
    { 0x2015, "-" }, { 0x2212, "-" }, { 0x2018, "'" }, { 0x2019, "'" }, { 0x201a, "," }, { 0x2032, "'" },
    { 0x201c, "\"" }, { 0x201d, "\"" }, { 0x201e, "\"" }, { 0x2033, "\"" }, { 0xab, "\"" }, { 0xbb, "\"" },
    { 0x2039, "<" }, { 0x203a, ">" }, { 0x2022, "\xb7" }, { 0x2023, "\xb7" }, { 0x25cf, "\xb7" },
    { 0x2026, "..." }, { 0x20ac, "EUR" }, { 0x2122, "TM" }, { 0x2116, "No" }, { 0x2190, "<-" },
    { 0x2192, "->" }, { 0x2191, "^" }, { 0x2193, "v" }, { 0x2264, "<=" }, { 0x2265, ">=" },
    { 0x2260, "!=" }, { 0x2248, "~" }, { 0x221e, "oo" }, { 0x2020, "+" }, { 0x2021, "+" },
    { 0x2030, "%o" }, { 0x152, "OE" }, { 0x153, "oe" }, { 0x160, "S" }, { 0x161, "s" },
    { 0x178, "Y" }, { 0x17d, "Z" }, { 0x17e, "z" }, { 0x192, "f" }, { 0x2c6, "^" }, { 0x2dc, "~" },
    { 0x25b6, ">" }, { 0x25c0, "<" }, { 0x2713, "v" }, { 0x2714, "v" }, { 0x2715, "x" },
    { 0x2716, "x" }, { 0x2605, "*" }, { 0x2606, "*" }, { 0x2302, "Home" }, { 0x2630, "=" },
};

/* Latin-1 letters CyOS's fonts leave blank (or do not have: they stop at
 * 0xfc), spelled with ones they have. "" for the soft hyphen. */
static const char *const latin1_missing[0x100 - 0xa1] = {
    [0xa2 - 0xa1] = "c", [0xa3 - 0xa1] = "L", [0xa4 - 0xa1] = "o", [0xa5 - 0xa1] = "Y",
    [0xa6 - 0xa1] = "|", [0xa7 - 0xa1] = "S", [0xa8 - 0xa1] = "\"", [0xaa - 0xa1] = "a",
    [0xab - 0xa1] = "<<", [0xac - 0xa1] = "-", [0xad - 0xa1] = "", [0xae - 0xa1] = "(R)",
    [0xaf - 0xa1] = "-", [0xb0 - 0xa1] = "o", [0xb1 - 0xa1] = "+-", [0xb2 - 0xa1] = "2",
    [0xb3 - 0xa1] = "3", [0xb4 - 0xa1] = "'", [0xb5 - 0xa1] = "u", [0xb6 - 0xa1] = "P",
    [0xb7 - 0xa1] = ".", [0xb8 - 0xa1] = ",", [0xb9 - 0xa1] = "1", [0xba - 0xa1] = "o",
    [0xbb - 0xa1] = ">>", [0xbc - 0xa1] = "1/4", [0xbd - 0xa1] = "1/2", [0xbe - 0xa1] = "3/4",
    [0xc3 - 0xa1] = "A", [0xc5 - 0xa1] = "A", [0xc6 - 0xa1] = "AE", [0xcb - 0xa1] = "E",
    [0xcc - 0xa1] = "I", [0xd0 - 0xa1] = "D", [0xd5 - 0xa1] = "O", [0xd7 - 0xa1] = "x",
    [0xd8 - 0xa1] = "O", [0xdd - 0xa1] = "Y", [0xde - 0xa1] = "Th", [0xe3 - 0xa1] = "a",
    [0xe5 - 0xa1] = "a", [0xe6 - 0xa1] = "ae", [0xeb - 0xa1] = "e", [0xec - 0xa1] = "i",
    [0xf0 - 0xa1] = "d", [0xf5 - 0xa1] = "o", [0xf7 - 0xa1] = "/", [0xf8 - 0xa1] = "o",
    [0xfd - 0xa1] = "y", [0xfe - 0xa1] = "th", [0xff - 0xa1] = "y",
};

int cyb_latin1(uint32_t cp, char *out)
{
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xa0) { out[0] = ' '; return 1; }
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;
    if (cp < 0x7f) { out[0] = (char)cp; return 1; }
    if (cp <= 0xff) {
        const char *r = latin1_missing[cp - 0xa1];
        if (!r) { out[0] = (char)cp; return 1; }
        size_t n = strlen(r);
        memcpy(out, r, n);
        return (int)n;
    }
    if (cp >= 0x100 && cp < 0x100 + sizeof ext_a - 1) { out[0] = ext_a[cp - 0x100]; return 1; }
    if (cp >= 0x410 && cp < 0x450) {
        size_t n = strlen(cyrillic[cp - 0x410]);
        memcpy(out, cyrillic[cp - 0x410], n);
        return (int)n;
    }
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
        if (others[i].cp == cp) {
            size_t n = strlen(others[i].s);
            memcpy(out, others[i].s, n);
            return (int)n;
        }
    out[0] = '?';
    return 1;
}

/* ---- entities ---- */

/* Latin-1 names, 0xa0 to 0xff in order */
static const char latin1_names[] =
    "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr "
    "deg plusmn sup2 sup3 acute micro para middot cedil sup1 ordm raquo frac14 frac12 frac34 iquest "
    "Agrave Aacute Acirc Atilde Auml Aring AElig Ccedil Egrave Eacute Ecirc Euml Igrave Iacute Icirc Iuml "
    "ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times Oslash Ugrave Uacute Ucirc Uuml Yacute THORN szlig "
    "agrave aacute acirc atilde auml aring aelig ccedil egrave eacute ecirc euml igrave iacute icirc iuml "
    "eth ntilde ograve oacute ocirc otilde ouml divide oslash ugrave uacute ucirc uuml yacute thorn yuml ";

static const struct { const char *name; uint16_t cp; } entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "sbquo", 0x201a }, { "ldquo", 0x201c }, { "rdquo", 0x201d },
    { "bdquo", 0x201e }, { "bull", 0x2022 }, { "euro", 0x20ac }, { "trade", 0x2122 },
    { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 },
    { "lsaquo", 0x2039 }, { "rsaquo", 0x203a }, { "minus", 0x2212 }, { "thinsp", 0x2009 },
    { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "zwnj", 0x200c }, { "zwj", 0x200d },
    { "OElig", 0x152 }, { "oelig", 0x153 }, { "Scaron", 0x160 }, { "scaron", 0x161 },
    { "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "permil", 0x2030 }, { "le", 0x2264 },
    { "ge", 0x2265 }, { "ne", 0x2260 }, { "infin", 0x221e }, { "prime", 0x2032 },
    { "nbsp", 0xa0 },
};

uint32_t cyb_entity(const char *name, int len)
{
    for (size_t i = 0; i < sizeof entities / sizeof entities[0]; i++)
        if ((int)strlen(entities[i].name) == len && !memcmp(entities[i].name, name, (size_t)len))
            return entities[i].cp;
    const char *p = latin1_names;
    for (uint32_t cp = 0xa0; cp <= 0xff; cp++) {
        const char *sp = strchr(p, ' ');
        if (sp - p == len && !memcmp(p, name, (size_t)len)) return cp;
        p = sp + 1;
    }
    return 0;
}

/* ---- keyboard ---- */

/* What the keys type with Fn: digits on the top row, symbols on the others.
 * The punctuation keys follow CyOS. key is the key's own character. */
int cyb_key_char(int key, bool shift, bool fn)
{
    static const struct { int16_t key; char plain, fn, shift; } map[] = {
        { 'q', 'q', '1', 'Q' }, { 'w', 'w', '2', 'W' }, { 'e', 'e', '3', 'E' },
        { 'r', 'r', '4', 'R' }, { 't', 't', '5', 'T' }, { 'y', 'y', '6', 'Y' },
        { 'u', 'u', '7', 'U' }, { 'i', 'i', '8', 'I' }, { 'o', 'o', '9', 'O' },
        { 'p', 'p', '0', 'P' },
        { 'a', 'a', '@', 'A' }, { 's', 's', '#', 'S' }, { 'd', 'd', '$', 'D' },
        { 'f', 'f', '%', 'F' }, { 'g', 'g', '&', 'G' }, { 'h', 'h', '*', 'H' },
        { 'j', 'j', '-', 'J' }, { 'k', 'k', '+', 'K' }, { 'l', 'l', '=', 'L' },
        { 'z', 'z', '<', 'Z' }, { 'x', 'x', '>', 'X' }, { 'c', 'c', '"', 'C' },
        { 'v', 'v', '_', 'V' }, { 'b', 'b', '{', 'B' }, { 'n', 'n', '}', 'N' },
        { 'm', 'm', '\'', 'M' },
        { '.', '.', '/', '\\' }, { ',', ',', '\'', '~' }, { ';', ';', ':', '|' },
        { '!', '!', '?', '^' }, { '(', '(', '[', '`' }, { ')', ')', ']', ')' },
        { ' ', ' ', ' ', ' ' },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].key == key) return (uint8_t)(fn ? map[i].fn : shift ? map[i].shift : map[i].plain);
    return 0;
}

/* exported for cyb_html.c */
uint32_t cyb_utf8_at(const char *s, int *len);
uint32_t cyb_utf8_at(const char *s, int *len)
{
    return utf8_at(s, len);
}
