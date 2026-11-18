/*
 * version.h — CalcEngine version information
 *
 * Single source of truth for version numbers.
 * Override at build time via: -DCE_VERSION_MAJOR=X -DCE_VERSION_MINOR=Y -DCE_VERSION_PATCH=Z
 */
#ifndef VERSION_H
#define VERSION_H

/* Allow build-time override via -D flags */
#ifndef CE_VERSION_MAJOR
#define CE_VERSION_MAJOR  2
#endif
#ifndef CE_VERSION_MINOR
#define CE_VERSION_MINOR  0
#endif
#ifndef CE_VERSION_PATCH
#define CE_VERSION_PATCH  1
#endif

/* String helpers */
#define CE_STRINGIFY_(x) #x
#define CE_STRINGIFY(x) CE_STRINGIFY_(x)

#define CE_VERSION_STR \
    CE_STRINGIFY(CE_VERSION_MAJOR) "." \
    CE_STRINGIFY(CE_VERSION_MINOR) "." \
    CE_STRINGIFY(CE_VERSION_PATCH)

/* Numeric version for comparisons: 1.2.3 → 10203 */
#define CE_VERSION_NUM \
    (CE_VERSION_MAJOR * 10000 + CE_VERSION_MINOR * 100 + CE_VERSION_PATCH)

#endif /* VERSION_H */
