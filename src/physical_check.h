#ifndef HWA_PHYSICAL_CHECK_H
#define HWA_PHYSICAL_CHECK_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>

/*
 * A binding role is `side:kind:case`. The parser returns a view of the case
 * bytes in the caller-owned role string; it never allocates or changes it.
 * A case is nonempty and cannot contain a colon.
 */
typedef enum HWAPhysicalRoleSide {
    HWA_PHYSICAL_ROLE_REFERENCE = 1,
    HWA_PHYSICAL_ROLE_MODEL = 2,
    HWA_PHYSICAL_ROLE_SIDE_COUNT = 3
} HWAPhysicalRoleSide;

typedef enum HWAPhysicalRoleKind {
    HWA_PHYSICAL_ROLE_BODY = 1,
    HWA_PHYSICAL_ROLE_JOINT = 2,
    HWA_PHYSICAL_ROLE_ISOLATED_A = 3,
    HWA_PHYSICAL_ROLE_ISOLATED_B = 4,
    HWA_PHYSICAL_ROLE_RENDER_BASELINE = 5,
    HWA_PHYSICAL_ROLE_RENDER_VARIANT = 6,
    HWA_PHYSICAL_ROLE_SCAN = 7,
    HWA_PHYSICAL_ROLE_KIND_COUNT = 8
} HWAPhysicalRoleKind;

typedef struct HWAPhysicalRole {
    HWAPhysicalRoleSide side;
    HWAPhysicalRoleKind kind;
    const char *case_id;
    size_t case_id_length;
} HWAPhysicalRole;

const char *hwa_physical_check_kind_name(HWAPhysicalCheckKind kind);
int hwa_physical_check_kind_from_name(const char *name,
                                      HWAPhysicalCheckKind *kind);

const char *hwa_physical_unit_name(HWAPhysicalUnit unit);
int hwa_physical_unit_from_name(const char *name, HWAPhysicalUnit *unit);

const char *hwa_physical_availability_name(
    HWAPhysicalAvailability availability);
int hwa_physical_availability_from_name(
    const char *name,
    HWAPhysicalAvailability *availability);

const char *hwa_physical_finding_class_name(
    HWAPhysicalFindingClass finding_class);
int hwa_physical_finding_class_from_name(
    const char *name,
    HWAPhysicalFindingClass *finding_class);

const char *hwa_physical_severity_name(HWAPhysicalSeverity severity);
int hwa_physical_severity_from_name(const char *name,
                                    HWAPhysicalSeverity *severity);

const char *hwa_physical_role_side_name(HWAPhysicalRoleSide side);
const char *hwa_physical_role_kind_name(HWAPhysicalRoleKind kind);
int hwa_physical_role_parse(const char *text, HWAPhysicalRole *role);

#endif
