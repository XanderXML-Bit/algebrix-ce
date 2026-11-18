/*
 * units.c — Unit conversion engine
 */
#include "units.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>

/* ── Unit descriptor ─────────────────────────────────────── */
typedef struct {
    const char      *name;      /* canonical name (lowercase)   */
    const char      *aliases[4]; /* alternate spellings          */
    unit_category_t  cat;
    double           to_si;     /* multiply by this to get SI   */
                                /*  (K for temp, Pa for pressure, etc.) */
    double           si_offset; /* add this after multiply (for °C, °F) */
} unit_t;

/* ── Unit table ──────────────────────────────────────────── */
static const unit_t g_units[] = {
    /* ── LENGTH (SI: metres) ─────────────────────────── */
    { "m",    {"metre","meter","meters","metres"},    UNIT_CAT_LENGTH, 1.0,        0 },
    { "km",   {"kilometer","kilometre","km",NULL},    UNIT_CAT_LENGTH, 1000.0,     0 },
    { "cm",   {"centimeter","centimetre",NULL,NULL},  UNIT_CAT_LENGTH, 0.01,       0 },
    { "mm",   {"millimeter","millimetre",NULL,NULL},  UNIT_CAT_LENGTH, 0.001,      0 },
    { "um",   {"micrometer","micrometre","μm",NULL},  UNIT_CAT_LENGTH, 1e-6,       0 },
    { "nm",   {"nanometer","nanometre",NULL,NULL},    UNIT_CAT_LENGTH, 1e-9,       0 },
    { "in",   {"inch","inches","\"",NULL},            UNIT_CAT_LENGTH, 0.0254,     0 },
    { "ft",   {"foot","feet","'",NULL},               UNIT_CAT_LENGTH, 0.3048,     0 },
    { "yd",   {"yard","yards",NULL,NULL},             UNIT_CAT_LENGTH, 0.9144,     0 },
    { "mi",   {"mile","miles",NULL,NULL},             UNIT_CAT_LENGTH, 1609.344,   0 },
    { "nmi",  {"nautical_mile","nmi",NULL,NULL},      UNIT_CAT_LENGTH, 1852.0,     0 },
    { "au",   {"astronomical_unit",NULL,NULL,NULL},   UNIT_CAT_LENGTH, 1.496e11,   0 },
    { "ly",   {"lightyear","light_year",NULL,NULL},   UNIT_CAT_LENGTH, 9.461e15,   0 },

    /* ── ANGLE (SI: radians) ─────────────────────────── */
    { "rad",  {"radian","radians",NULL,NULL},         UNIT_CAT_ANGLE,  1.0,        0 },
    { "deg",  {"degree","degrees","°",NULL},          UNIT_CAT_ANGLE,  0.017453293, 0 },
    { "grad", {"gradian","gradians","gon",NULL},      UNIT_CAT_ANGLE,  0.015707963, 0 },
    { "rev",  {"revolution","turn",NULL,NULL},        UNIT_CAT_ANGLE,  6.283185307, 0 },

    /* ── TEMPERATURE (conversion is special) ─────────── */
    /* We store to-Kelvin conversion: K = to_si * value + si_offset */
    { "k",    {"kelvin","kelvins",NULL,NULL},         UNIT_CAT_TEMPERATURE, 1.0,    0 },
    { "c",    {"celsius","°c","degc",NULL},           UNIT_CAT_TEMPERATURE, 1.0,  273.15 },
    { "f",    {"fahrenheit","°f","degf",NULL},        UNIT_CAT_TEMPERATURE, 5.0/9.0, 255.372 },
    { "r",    {"rankine",NULL,NULL,NULL},             UNIT_CAT_TEMPERATURE, 5.0/9.0, 0 },

    /* ── MASS (SI: kg) ───────────────────────────────── */
    { "kg",   {"kilogram","kilograms",NULL,NULL},     UNIT_CAT_MASS, 1.0,          0 },
    { "g",    {"gram","grams",NULL,NULL},             UNIT_CAT_MASS, 0.001,        0 },
    { "mg",   {"milligram","milligrams",NULL,NULL},   UNIT_CAT_MASS, 1e-6,         0 },
    { "t",    {"tonne","metric_ton",NULL,NULL},       UNIT_CAT_MASS, 1000.0,       0 },
    { "lb",   {"pound","pounds","lbs",NULL},          UNIT_CAT_MASS, 0.45359237,   0 },
    { "oz",   {"ounce","ounces",NULL,NULL},           UNIT_CAT_MASS, 0.02834952,   0 },
    { "ton",  {"short_ton","us_ton",NULL,NULL},       UNIT_CAT_MASS, 907.1847,     0 },
    { "st",   {"stone","stones",NULL,NULL},           UNIT_CAT_MASS, 6.35029,      0 },

    /* ── AREA (SI: m^2) ──────────────────────────────── */
    { "m2",   {"sq_m","sqm","square_meter",NULL},     UNIT_CAT_AREA, 1.0,          0 },
    { "km2",  {"sq_km","sqkm","square_km",NULL},      UNIT_CAT_AREA, 1e6,          0 },
    { "cm2",  {"sq_cm","sqcm",NULL,NULL},             UNIT_CAT_AREA, 1e-4,         0 },
    { "mm2",  {"sq_mm","sqmm",NULL,NULL},             UNIT_CAT_AREA, 1e-6,         0 },
    { "ft2",  {"sq_ft","sqft","square_foot",NULL},    UNIT_CAT_AREA, 0.09290304,   0 },
    { "in2",  {"sq_in","sqin","square_inch",NULL},    UNIT_CAT_AREA, 6.4516e-4,    0 },
    { "ac",   {"acre","acres",NULL,NULL},             UNIT_CAT_AREA, 4046.8564,    0 },
    { "ha",   {"hectare","hectares",NULL,NULL},       UNIT_CAT_AREA, 1e4,          0 },
    { "mi2",  {"sq_mi","sqmi","square_mile",NULL},    UNIT_CAT_AREA, 2.589988e6,   0 },

    /* ── VOLUME (SI: m^3) ────────────────────────────── */
    { "m3",   {"cubic_m","cubed_m",NULL,NULL},        UNIT_CAT_VOLUME, 1.0,        0 },
    { "l",    {"liter","litre","liters",NULL},         UNIT_CAT_VOLUME, 0.001,      0 },
    { "ml",   {"milliliter","millilitre","cc",NULL},   UNIT_CAT_VOLUME, 1e-6,       0 },
    { "ft3",  {"cubic_ft","cu_ft",NULL,NULL},          UNIT_CAT_VOLUME, 0.02831685, 0 },
    { "in3",  {"cubic_in","cu_in",NULL,NULL},          UNIT_CAT_VOLUME, 1.6387e-5,  0 },
    { "gal",  {"gallon","gallons","us_gal",NULL},      UNIT_CAT_VOLUME, 0.003785412, 0 },
    { "qt",   {"quart","quarts",NULL,NULL},            UNIT_CAT_VOLUME, 9.46353e-4, 0 },
    { "pt",   {"pint","pints",NULL,NULL},              UNIT_CAT_VOLUME, 4.73176e-4, 0 },
    { "cup",  {"cups",NULL,NULL,NULL},                 UNIT_CAT_VOLUME, 2.36588e-4, 0 },
    { "tbsp", {"tablespoon","tablespoons",NULL,NULL},  UNIT_CAT_VOLUME, 1.47868e-5, 0 },
    { "tsp",  {"teaspoon","teaspoons",NULL,NULL},      UNIT_CAT_VOLUME, 4.92892e-6, 0 },
    { "ukgal",{"imperial_gallon","imp_gal",NULL,NULL}, UNIT_CAT_VOLUME, 0.00454609, 0 },

    /* ── TIME (SI: seconds) ──────────────────────────── */
    { "s",    {"second","seconds","sec",NULL},         UNIT_CAT_TIME, 1.0,         0 },
    { "ms",   {"millisecond","milliseconds","msec",NULL}, UNIT_CAT_TIME, 0.001,    0 },
    { "us",   {"microsecond","microseconds","μs",NULL},UNIT_CAT_TIME, 1e-6,        0 },
    { "min",  {"minute","minutes",NULL,NULL},          UNIT_CAT_TIME, 60.0,        0 },
    { "hr",   {"hour","hours","h",NULL},               UNIT_CAT_TIME, 3600.0,      0 },
    { "day",  {"days","d",NULL,NULL},                  UNIT_CAT_TIME, 86400.0,     0 },
    { "wk",   {"week","weeks",NULL,NULL},              UNIT_CAT_TIME, 604800.0,    0 },
    { "yr",   {"year","years",NULL,NULL},              UNIT_CAT_TIME, 31557600.0,  0 },

    /* ── SPEED (SI: m/s) ─────────────────────────────── */
    { "mps",  {"m/s","meters_per_second",NULL,NULL},  UNIT_CAT_SPEED, 1.0,         0 },
    { "kph",  {"km/h","kmh","kilometers_per_hour",NULL}, UNIT_CAT_SPEED, 1.0/3.6,  0 },
    { "mph",  {"miles_per_hour","mi/h",NULL,NULL},    UNIT_CAT_SPEED, 0.44704,     0 },
    { "fps",  {"ft/s","feet_per_second",NULL,NULL},   UNIT_CAT_SPEED, 0.3048,      0 },
    { "kn",   {"knot","knots","kt",NULL},              UNIT_CAT_SPEED, 0.514444,    0 },
    { "mach", {"mach_number",NULL,NULL,NULL},          UNIT_CAT_SPEED, 340.29,      0 },

    /* ── PRESSURE (SI: Pa) ───────────────────────────── */
    { "pa",   {"pascal","pascals",NULL,NULL},          UNIT_CAT_PRESSURE, 1.0,     0 },
    { "kpa",  {"kilopascal","kilopascals",NULL,NULL},  UNIT_CAT_PRESSURE, 1000.0,  0 },
    { "mpa",  {"megapascal","megapascals",NULL,NULL},  UNIT_CAT_PRESSURE, 1e6,     0 },
    { "atm",  {"atmosphere","atmospheres",NULL,NULL},  UNIT_CAT_PRESSURE, 101325.0, 0 },
    { "bar",  {"bars",NULL,NULL,NULL},                 UNIT_CAT_PRESSURE, 1e5,     0 },
    { "psi",  {"pounds_per_sq_in",NULL,NULL,NULL},     UNIT_CAT_PRESSURE, 6894.76, 0 },
    { "mmhg", {"torr","mmhg",NULL,NULL},               UNIT_CAT_PRESSURE, 133.322, 0 },

    /* ── ENERGY (SI: joules) ─────────────────────────── */
    { "j",    {"joule","joules",NULL,NULL},            UNIT_CAT_ENERGY, 1.0,       0 },
    { "kj",   {"kilojoule","kilojoules",NULL,NULL},    UNIT_CAT_ENERGY, 1000.0,    0 },
    { "mj",   {"megajoule","megajoules",NULL,NULL},    UNIT_CAT_ENERGY, 1e6,       0 },
    { "cal",  {"calorie","calories","cal",NULL},        UNIT_CAT_ENERGY, 4.184,     0 },
    { "kcal", {"kilocalorie","kilocalories","Cal",NULL},UNIT_CAT_ENERGY, 4184.0,   0 },
    { "kwh",  {"kilowatt_hour","kw_h",NULL,NULL},      UNIT_CAT_ENERGY, 3.6e6,     0 },
    { "btu",  {"british_thermal_unit","btus",NULL,NULL},UNIT_CAT_ENERGY, 1055.06,  0 },
    { "ev",   {"electronvolt","electronvolts","eV",NULL},UNIT_CAT_ENERGY, 1.60218e-19, 0 },
    { "wh",   {"watt_hour","w_h",NULL,NULL},           UNIT_CAT_ENERGY, 3600.0,    0 },

    { NULL, {NULL,NULL,NULL,NULL}, UNIT_CAT_UNKNOWN, 0, 0 }
};

/* ── Internal lookup ─────────────────────────────────────── */

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return !*a && !*b;
}

static const unit_t *find_unit(const char *name)
{
    int i, j;
    if (!name || !*name) return NULL;
    for (i = 0; g_units[i].name; i++) {
        if (ci_eq(name, g_units[i].name)) return &g_units[i];
        for (j = 0; j < 4 && g_units[i].aliases[j]; j++) {
            if (ci_eq(name, g_units[i].aliases[j])) return &g_units[i];
        }
    }
    return NULL;
}

/* ── Temperature conversion (special case) ───────────────── */
/* to_kelvin: K = to_si * val + si_offset */
/* from_kelvin: val = (K - si_offset) / to_si */

static double to_kelvin(double val, const unit_t *u)
{
    return u->to_si * val + u->si_offset;
}

static double from_kelvin(double k, const unit_t *u)
{
    return (k - u->si_offset) / u->to_si;
}

/* ── Public API ──────────────────────────────────────────── */

ce_float_t units_convert(ce_float_t value,
                          const char *from_unit, const char *to_unit,
                          char *desc_buf, int desc_size,
                          bool *ok)
{
    const unit_t *from = find_unit(from_unit);
    const unit_t *to   = find_unit(to_unit);

    if (!from || !to) {
        if (ok) *ok = false;
        if (desc_buf && desc_size > 0)
            snprintf(desc_buf, (size_t)desc_size,
                     "Unknown unit: %s",
                     !from ? from_unit : to_unit);
        return 0;
    }

    if (from->cat != to->cat) {
        if (ok) *ok = false;
        if (desc_buf && desc_size > 0)
            snprintf(desc_buf, (size_t)desc_size,
                     "Cannot convert %s (%s) to %s (%s)",
                     from->name, units_category_name(from->cat),
                     to->name,   units_category_name(to->cat));
        return 0;
    }

    double result;
    if (from->cat == UNIT_CAT_TEMPERATURE) {
        double k = to_kelvin((double)value, from);
        result = from_kelvin(k, to);
    } else {
        /* Generic: convert via SI */
        double si_val = (double)value * from->to_si;
        result = si_val / to->to_si;
    }

    if (ok) *ok = true;
    if (desc_buf && desc_size > 0) {
        snprintf(desc_buf, (size_t)desc_size,
                 "%.6g %s = %.6g %s",
                 (double)value, from->name, result, to->name);
    }
    return (ce_float_t)result;
}

unit_category_t units_category(const char *unit)
{
    const unit_t *u = find_unit(unit);
    return u ? u->cat : UNIT_CAT_UNKNOWN;
}

const char *units_category_name(unit_category_t cat)
{
    switch (cat) {
    case UNIT_CAT_LENGTH:      return "Length";
    case UNIT_CAT_ANGLE:       return "Angle";
    case UNIT_CAT_TEMPERATURE: return "Temperature";
    case UNIT_CAT_MASS:        return "Mass/Weight";
    case UNIT_CAT_AREA:        return "Area";
    case UNIT_CAT_VOLUME:      return "Volume";
    case UNIT_CAT_TIME:        return "Time";
    case UNIT_CAT_SPEED:       return "Speed";
    case UNIT_CAT_PRESSURE:    return "Pressure";
    case UNIT_CAT_ENERGY:      return "Energy";
    default:                   return "Unknown";
    }
}

void units_list(unit_category_t cat, char *out_buf, int buf_size)
{
    int i, pos = 0;
    bool first = true;
    if (!out_buf || buf_size <= 0) return;
    out_buf[0] = '\0';
    for (i = 0; g_units[i].name && pos < buf_size - 4; i++) {
        if (g_units[i].cat == cat) {
            if (!first) pos += snprintf(out_buf + pos, (size_t)(buf_size - pos), ", ");
            pos += snprintf(out_buf + pos, (size_t)(buf_size - pos), "%s", g_units[i].name);
            first = false;
        }
    }
}
