/*
 * units.h — Unit conversion engine
 *
 * Supports: length, angle, temperature, mass/weight, area, volume, time,
 *           speed, pressure, energy, and digital storage.
 * Usage: convert(value, "from_unit", "to_unit")
 */
#ifndef UNITS_H
#define UNITS_H

#include "calcengine.h"

/* Supported unit categories */
typedef enum {
    UNIT_CAT_LENGTH = 0,
    UNIT_CAT_ANGLE,
    UNIT_CAT_TEMPERATURE,
    UNIT_CAT_MASS,
    UNIT_CAT_AREA,
    UNIT_CAT_VOLUME,
    UNIT_CAT_TIME,
    UNIT_CAT_SPEED,
    UNIT_CAT_PRESSURE,
    UNIT_CAT_ENERGY,
    UNIT_CAT_UNKNOWN
} unit_category_t;

/*
 * Convert value from from_unit to to_unit.
 * Returns converted value; sets *ok to true on success.
 * Both unit strings are case-insensitive (e.g., "ft", "FT", "Ft").
 * Description of conversion written to desc_buf if non-NULL.
 */
ce_float_t units_convert(ce_float_t value,
                          const char *from_unit, const char *to_unit,
                          char *desc_buf, int desc_size,
                          bool *ok);

/*
 * Look up the category of a unit string.
 * Returns UNIT_CAT_UNKNOWN if not found.
 */
unit_category_t units_category(const char *unit);

/*
 * Human-readable category name.
 */
const char *units_category_name(unit_category_t cat);

/*
 * List all supported units for a given category.
 * Writes comma-separated names to out_buf.
 */
void units_list(unit_category_t cat, char *out_buf, int buf_size);

#endif /* UNITS_H */
