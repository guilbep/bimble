/* IFC export of the model's grids (ADR 0056, ticket G1): the one hook ifc_out.c calls. Layer L2. */
#ifndef CCORE_IFC_OUT_GRID_H
#define CCORE_IFC_OUT_GRID_H
#include <stdint.h>
#include "buf.h"
#include "model.h"

/* Appends, as IFC4 entities numbered from *next (advanced): a 'FootPrint' subcontext of `ctx`,
 * then per grid (id order) its axes (IfcGridAxis on a two-point IfcPolyline, plan mm), an
 * IfcGeometricCurveSet of them as the 'FootPrint' representation, a placement at the ground
 * storey's (axes parallel to the first one are UAxes, the others VAxes), the IfcGrid, and one
 * IfcRelContainedInSpatialStructure of the grids in the ground storey: the storey (not a datum
 * level) whose elevation is nearest 0, lower first, then by id; the building when there is none.
 * `st_pl`, `st_ent`: per storey of m (model order) its placement and entity, set for every
 * storey; `world`: the origin IfcAxis2Placement3D. Writes nothing for a model without grids.
 * 0 ok, -1 on a value that is not finite or out of memory. */
int cc_ifc_out_grids(const cc_model *m, Buf *d, uint64_t *next, uint64_t ctx, uint64_t world, const uint64_t *st_pl,
                     const uint64_t *st_ent, uint64_t bldg_pl, uint64_t bldg);
#endif
