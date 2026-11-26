/* See verb_table.h. The schema and summary texts below are Rust's (crates/bimble-api/src/
 * dispatch.rs), compacted from golden/verbs_schema/<verb>.json and golden/verbs_list.json. */
#include "verb_table.h"
#include "verbs_materials.h" /* MAT1 */
#include <string.h>
#include "json_w.h"

static const char SCHEMA_PLACE[] =
    "{\"$defs\":{\"Kind\":{\"oneOf\":[{\"enum\":[\"wall\"],\"type\":\"string\"},{\"const\":\"column\",\"d"
    "escription\":\"One click: its centre.\",\"type\":\"string\"},{\"const\":\"beam\",\"description\":\"T"
    "wo clicks: start and end of its axis.\",\"type\":\"string\"},{\"const\":\"slab\",\"description\":\"T"
    "wo clicks: opposite corners of a rectangle.\",\"type\":\"string\"}]},\"Sizes\":{\"description\":\"Si"
    "zes of new elements, mm.\",\"properties\":{\"beam_height\":{\"default\":500.0,\"format\":\"double\","
    "\"type\":\"number\"},\"beam_width\":{\"default\":250.0,\"format\":\"double\",\"type\":\"number\"},\""
    "column\":{\"default\":300.0,\"format\":\"double\",\"type\":\"number\"},\"slab\":{\"default\":250.0,"
    "\"format\":\"double\",\"type\":\"number\"},\"wall\":{\"default\":200.0,\"format\":\"double\",\"type"
    "\":\"number\"}},\"type\":\"object\"}},\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
    "\"additionalProperties\":false,\"description\":\"Arguments of `place`.\",\"properties\":{\"a\":{\"de"
    "scription\":\"First point [x, y], mm: a column's centre, an axis start, a slab corner.\",\"items\":{"
    "\"format\":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"b\":{"
    "\"description\":\"Second point [x, y], mm; not used for a column.\",\"items\":{\"format\":\"double\""
    ",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":[\"array\",\"null\"]},\"kind\":{\"$ref"
    "\":\"#/$defs/Kind\"},\"material\":{\"description\":\"A model material's name or a library grade (\\"
    "\"C35/45\\\"); default: the model's concrete.\",\"type\":[\"string\",\"null\"]},\"sizes\":{\"anyOf\""
    ":[{\"$ref\":\"#/$defs/Sizes\"},{\"type\":\"null\"}],\"description\":\"Sizes of new elements, mm; mis"
    "sing ones take defaults.\"},\"storey\":{\"description\":\"GlobalId of the storey.\",\"type\":\"strin"
    "g\"}},\"required\":[\"kind\",\"storey\",\"a\"],\"title\":\"PlaceArgs\",\"type\":\"object\"}";

static const char SCHEMA_MOVE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `move` and `copy`.\",\"properties\":{\"d\":{\"description\":\"Vector [dx, dy"
    ", dz], mm.\",\"items\":{\"format\":\"double\",\"type\":\"number\"},\"maxItems\":3,\"minItems\":3,\"t"
    "ype\":\"array\"},\"ids\":{\"description\":\"GlobalIds of the elements.\",\"items\":{\"type\":\"strin"
    "g\"},\"type\":\"array\"}},\"required\":[\"ids\",\"d\"],\"title\":\"MoveArgs\",\"type\":\"object\"}";

static const char SCHEMA_COPY[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `move` and `copy`.\",\"properties\":{\"d\":{\"description\":\"Vector [dx, dy"
    ", dz], mm.\",\"items\":{\"format\":\"double\",\"type\":\"number\"},\"maxItems\":3,\"minItems\":3,\"t"
    "ype\":\"array\"},\"ids\":{\"description\":\"GlobalIds of the elements.\",\"items\":{\"type\":\"strin"
    "g\"},\"type\":\"array\"}},\"required\":[\"ids\",\"d\"],\"title\":\"MoveArgs\",\"type\":\"object\"}";

static const char SCHEMA_DELETE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `delete`.\",\"properties\":{\"ids\":{\"description\":\"GlobalIds of the elem"
    "ents.\",\"items\":{\"type\":\"string\"},\"type\":\"array\"}},\"required\":[\"ids\"],\"title\":\"IdsA"
    "rgs\",\"type\":\"object\"}";

static const char SCHEMA_SET_GENERATOR[] =
    "{\"$defs\":{\"GeneratorParams\":{\"additionalProperties\":false,\"properties\":{\"depth\":{\"format"
    "\":\"double\",\"type\":[\"number\",\"null\"]},\"height\":{\"format\":\"double\",\"type\":[\"number\""
    ",\"null\"]},\"length\":{\"format\":\"double\",\"type\":[\"number\",\"null\"]},\"thickness\":{\"forma"
    "t\":\"double\",\"type\":[\"number\",\"null\"]},\"width\":{\"format\":\"double\",\"type\":[\"number\""
    ",\"null\"]}},\"type\":\"object\"}},\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"ad"
    "ditionalProperties\":false,\"description\":\"Arguments of `set_generator`.\",\"properties\":{\"id\":"
    "{\"description\":\"GlobalId of the element.\",\"type\":\"string\"},\"params\":{\"$ref\":\"#/$defs/Ge"
    "neratorParams\",\"description\":\"New sizes, mm, > 0; only the ones the element's generator has.\"}}"
    ",\"required\":[\"id\",\"params\"],\"title\":\"SetGeneratorArgs\",\"type\":\"object\"}";

static const char SCHEMA_SET_VIEW[] =
    "{\"$defs\":{\"CropArgs\":{\"additionalProperties\":false,\"properties\":{\"max\":{\"items\":{\"forma"
    "t\":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"min\":{\"ite"
    "ms\":{\"format\":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"}},"
    "\"required\":[\"min\",\"max\"],\"type\":\"object\"},\"PlaneArgs\":{\"additionalProperties\":false,\""
    "properties\":{\"offset_mm\":{\"format\":\"double\",\"type\":[\"number\",\"null\"]}},\"type\":\"objec"
    "t\"},\"RangeArgs\":{\"additionalProperties\":false,\"properties\":{\"bottom\":{\"anyOf\":[{\"$ref\":"
    "\"#/$defs/PlaneArgs\"},{\"type\":\"null\"}]},\"cut\":{\"anyOf\":[{\"$ref\":\"#/$defs/PlaneArgs\"},{"
    "\"type\":\"null\"}]},\"depth\":{\"anyOf\":[{\"$ref\":\"#/$defs/PlaneArgs\"},{\"type\":\"null\"}]},\""
    "top\":{\"anyOf\":[{\"$ref\":\"#/$defs/PlaneArgs\"},{\"type\":\"null\"}]}},\"type\":\"object\"}},\"$s"
    "chema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descriptio"
    "n\":\"Arguments of `set_view`: the `View` fields to change, others kept.\",\"properties\":{\"crop\":"
    "{\"anyOf\":[{\"$ref\":\"#/$defs/CropArgs\"},{\"type\":\"null\"}],\"description\":\"The crop rectangl"
    "e in the view's plane, world mm; null: no crop.\"},\"id\":{\"description\":\"GlobalId of the view.\""
    ",\"type\":\"string\"},\"range\":{\"anyOf\":[{\"$ref\":\"#/$defs/RangeArgs\"},{\"type\":\"null\"}],\""
    "description\":\"A plan's view range: the planes' offsets from their levels, mm.\"},\"scale\":{\"desc"
    "ription\":\"1:N, N from 1 to 5000.\",\"format\":\"uint32\",\"minimum\":0,\"type\":[\"integer\",\"nul"
    "l\"]},\"true_north\":{\"description\":\"Drawn in true north rather than project north.\",\"type\":["
    "\"boolean\",\"null\"]}},\"required\":[\"id\"],\"title\":\"SetViewArgs\",\"type\":\"object\"}";

static const char SCHEMA_SET_SCHEDULE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `set_schedule`: the `ScheduleDef` fields to change, others kept.\",\"propert"
    "ies\":{\"columns\":{\"description\":\"The columns, in order: field names (\\\"level\\\", \\\"name\\"
    "\", \\\"volume\\\", ...).\",\"items\":{\"type\":\"string\"},\"type\":[\"array\",\"null\"]},\"family"
    "\":{\"default\":null,\"description\":\"Only elements of this material family (\\\"concrete\\\", \\\""
    "steel\\\", ...); null: every family.\",\"type\":[\"string\",\"null\"]},\"group_by_level\":{\"descrip"
    "tion\":\"Rows grouped by level, a subtotal under each.\",\"type\":[\"boolean\",\"null\"]},\"name\":{"
    "\"description\":\"The schedule's name (built-in or the model's own), case-insensitive.\",\"type\":\""
    "string\"}},\"required\":[\"name\"],\"title\":\"SetScheduleArgs\",\"type\":\"object\"}";

static const char SCHEMA_SET_MATERIAL[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `set_material`.\",\"properties\":{\"ids\":{\"description\":\"GlobalIds of th"
    "e elements.\",\"items\":{\"type\":\"string\"},\"type\":\"array\"},\"material\":{\"description\":\"A "
    "model material's name or a library grade (\\\"C35/45\\\").\",\"type\":\"string\"}},\"required\":[\"i"
    "ds\",\"material\"],\"title\":\"SetMaterialArgs\",\"type\":\"object\"}";

static const char SCHEMA_SET_NOTE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `set_note`.\",\"properties\":{\"id\":{\"description\":\"GlobalId of the elem"
    "ent.\",\"type\":\"string\"},\"text\":{\"description\":\"The note, one line. Empty: remove the note."
    "\",\"type\":\"string\"}},\"required\":[\"id\",\"text\"],\"title\":\"SetNoteArgs\",\"type\":\"object"
    "\"}";

static const char SCHEMA_SET_PROJECT_FIELD[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `set_project_field`.\",\"properties\":{\"label\":{\"description\":\"The fiel"
    "d's label (\\\"Ma\xc3\xaetre d'ouvrage\\\"); the key is made from it (`project::key_of`).\",\"type\""
    ":\"string\"},\"value\":{\"description\":\"The value; empty removes the field.\",\"type\":\"string\"}"
    "},\"required\":[\"label\",\"value\"],\"title\":\"SetProjectFieldArgs\",\"type\":\"object\"}";

static const char SCHEMA_ROTATE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `rotate`.\",\"properties\":{\"about\":{\"description\":\"Centre [x, y], mm; "
    "default: the centre of the elements' placement origins.\",\"items\":{\"format\":\"double\",\"type\":"
    "\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":[\"array\",\"null\"]},\"angle_deg\":{\"descriptio"
    "n\":\"Angle, degrees, counter-clockwise seen from above; |angle| \xe2\x89\xa4 360.\",\"format\":\"do"
    "uble\",\"type\":\"number\"},\"ids\":{\"description\":\"GlobalIds of the elements.\",\"items\":{\"typ"
    "e\":\"string\"},\"type\":\"array\"}},\"required\":[\"ids\",\"angle_deg\"],\"title\":\"RotateArgs\","
    "\"type\":\"object\"}";

static const char SCHEMA_MIRROR[] =
    "{\"$defs\":{\"MirrorAxis\":{\"additionalProperties\":false,\"description\":\"A vertical mirror plane"
    ": through `point`, along `dir` (plan, mm).\",\"properties\":{\"dir\":{\"items\":{\"format\":\"double"
    "\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"point\":{\"items\":{\"fo"
    "rmat\":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"}},\"required"
    "\":[\"point\",\"dir\"],\"type\":\"object\"}},\"$schema\":\"https://json-schema.org/draft/2020-12/sch"
    "ema\",\"additionalProperties\":false,\"description\":\"Arguments of `mirror`.\",\"properties\":{\"ax"
    "is\":{\"$ref\":\"#/$defs/MirrorAxis\"},\"ids\":{\"description\":\"GlobalIds of the elements.\",\"ite"
    "ms\":{\"type\":\"string\"},\"type\":\"array\"}},\"required\":[\"ids\",\"axis\"],\"title\":\"MirrorAr"
    "gs\",\"type\":\"object\"}";

static const char SCHEMA_ADD_SHEET[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `add_sheet`.\",\"properties\":{\"name\":{\"type\":[\"string\",\"null\"]},\"n"
    "umber\":{\"description\":\"Drawing number, unique; default: the next free one (\\\"S-101\\\" \xe2"
    "\x86\x92 \\\"S-102\\\").\",\"type\":[\"string\",\"null\"]},\"paper\":{\"description\":\"A4, A3, A2, "
    "A1 or A0.\",\"type\":[\"string\",\"null\"]},\"portrait\":{\"type\":[\"boolean\",\"null\"]}},\"title"
    "\":\"AddSheetArgs\",\"type\":\"object\"}";

static const char SCHEMA_SET_SHEET[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `set_sheet`: the fields to change, others kept.\",\"properties\":{\"id\":{\""
    "description\":\"GlobalId of the sheet.\",\"type\":\"string\"},\"name\":{\"type\":[\"string\",\"null"
    "\"]},\"number\":{\"type\":[\"string\",\"null\"]},\"paper\":{\"type\":[\"string\",\"null\"]},\"portra"
    "it\":{\"type\":[\"boolean\",\"null\"]},\"revision\":{\"description\":\"The title block's {revision};"
    " empty: removed.\",\"type\":[\"string\",\"null\"]}},\"required\":[\"id\"],\"title\":\"SetSheetArgs\""
    ",\"type\":\"object\"}";

static const char SUMMARY_PLACE[] =
    "Place a new column, beam, wall or slab on a storey (storey-relative mm).";
static const char SUMMARY_MOVE[] =
    "Move elements by a vector d = [dx, dy, dz], mm.";
static const char SUMMARY_COPY[] =
    "Copy elements, moved by d = [dx, dy, dz] mm, with new GlobalIds and names.";
static const char SUMMARY_DELETE[] =
    "Delete elements.";
static const char SUMMARY_SET_GENERATOR[] =
    "Change an element's sizes (width, height, length, depth, thickness), mm; missing ones are kept.";
static const char SUMMARY_SET_VIEW[] =
    "Change a view's scale (1:N), true_north, crop (null: none) or plan range offsets, mm; missing ones a"
    "re kept.";
static const char SUMMARY_SET_SCHEDULE[] =
    "Change a schedule's grouping by level, family filter (null: every family) or columns (field names); "
    "missing ones are kept.";
static const char SUMMARY_SET_MATERIAL[] =
    "Give elements a material: a model material's name or a library grade (\"C35/45\").";
static const char SUMMARY_SET_NOTE[] =
    "Write a one-line note on an element (the property \"Note\"); an empty text removes it.";
static const char SUMMARY_SET_PROJECT_FIELD[] =
    "Set one project information field by its label (the key is made from it); an empty value removes it.";
static const char SUMMARY_ROTATE[] =
    "Rotate elements by angle_deg about the vertical axis through about = [x, y] mm (default: the centre "
    "of their origins).";
static const char SUMMARY_MIRROR[] =
    "Mirror elements in the vertical plane through axis.point along axis.dir; placements stay right-hande"
    "d.";
static const char SUMMARY_ADD_SHEET[] =
    "Add a sheet (default: the next free number, \"New sheet\", A1 landscape) with a new GlobalId.";
static const char SUMMARY_SET_SHEET[] =
    "Change a sheet's number, name, paper (A4..A0), portrait or revision; missing ones are kept, an empty"
    " revision removes it.";

/* C additions (verb_table.h), in schemars' shape. */
static const char SCHEMA_RENAME[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"descr"
    "iption\":\"Arguments of `rename`.\",\"properties\":{\"id\":{\"description\":\"GlobalId of the elem"
    "ent.\",\"type\":\"string\"},\"name\":{\"description\":\"The new name, not blank.\",\"type\":\"st"
    "ring\"}},\"required\":[\"id\",\"name\"],\"title\":\"RenameArgs\",\"type\":\"object\"}";
static const char SUMMARY_RENAME[] = "Rename an element (a C addition: Rust's dispatch has no rename verb).";

static const char SCHEMA_ADD_DIMENSION[] =
    "{\"$defs\":{\"AnchorArgs\":{\"additionalProperties\":false,\"description\":\"A point taken on an"
    " element: `feature` (\\\"origin\\\", \\\"end\\\", \\\"corner:N\\\") of `element`, followed when "
    "the element moves; or `at` with the element it was taken on.\",\"properties\":{\"at\":{\"anyOf\""
    ":[{\"$ref\":\"#/$defs/Point\"},{\"type\":\"null\"}]},\"element\":{\"description\":\"GlobalId of "
    "the element.\",\"type\":[\"string\",\"null\"]},\"feature\":{\"description\":\"\\\"origin\\\", \\"
    "\"end\\\" or \\\"corner:N\\\" of the element's generator.\",\"type\":[\"string\",\"null\"]}},\"t"
    "ype\":\"object\"},\"Point\":{\"description\":\"A point [x, y] or [x, y, z], world mm (z: the pla"
    "n's storey elevation when left out).\",\"items\":{\"format\":\"double\",\"type\":\"number\"},\"m"
    "axItems\":3,\"minItems\":2,\"type\":\"array\"}},\"$schema\":\"https://json-schema.org/draft/2020"
    "-12/schema\",\"additionalProperties\":false,\"description\":\"Arguments of `add_dimension`.\",\""
    "properties\":{\"a\":{\"anyOf\":[{\"$ref\":\"#/$defs/Point\"},{\"$ref\":\"#/$defs/AnchorArgs\"}],"
    "\"description\":\"First end.\"},\"axis\":{\"description\":\"\\\"aligned\\\" (default), \\\"horiz"
    "ontal\\\" or \\\"vertical\\\".\",\"enum\":[\"aligned\",\"horizontal\",\"vertical\",null],\"type\""
    ":[\"string\",\"null\"]},\"b\":{\"anyOf\":[{\"$ref\":\"#/$defs/Point\"},{\"$ref\":\"#/$defs/Ancho"
    "rArgs\"}],\"description\":\"Second end.\"},\"kind\":{\"description\":\"Its dimension type (graph"
    "ic style); none: the plan template's.\",\"type\":[\"string\",\"null\"]},\"offset\":{\"descriptio"
    "n\":\"Where the dimension line goes: mm to the left of a->b as seen in the view.\",\"format\":\""
    "double\",\"type\":\"number\"},\"view\":{\"description\":\"GlobalId of the 2D view (a plan or a s"
    "ection; a default plan is stored on first use).\",\"type\":\"string\"}},\"required\":[\"view\",\""
    "a\",\"b\",\"offset\"],\"title\":\"AddDimensionArgs\",\"type\":\"object\"}";
static const char SUMMARY_ADD_DIMENSION[] =
    "Add a dimension to a 2D view: two ends (points, or features of elements it then follows) and an"
    " offset (a C addition).";
static const char SCHEMA_ADD_TEXT[] =
    "{\"$defs\":{\"Point\":{\"description\":\"A point [x, y] or [x, y, z], world mm (z: the plan's st"
    "orey elevation when left out).\",\"items\":{\"format\":\"double\",\"type\":\"number\"},\"maxItem"
    "s\":3,\"minItems\":2,\"type\":\"array\"}},\"$schema\":\"https://json-schema.org/draft/2020-12/sc"
    "hema\",\"additionalProperties\":false,\"description\":\"Arguments of `add_text`.\",\"properties\""
    ":{\"angle_deg\":{\"description\":\"Its angle, degrees counter-clockwise (default 0).\",\"format\""
    ":\"double\",\"type\":[\"number\",\"null\"]},\"at\":{\"$ref\":\"#/$defs/Point\"},\"kind\":{\"desc"
    "ription\":\"Its text type.\",\"type\":[\"string\",\"null\"]},\"size_mm\":{\"description\":\"Its "
    "height on paper, mm (default 3.5).\",\"format\":\"double\",\"type\":[\"number\",\"null\"]},\"tex"
    "t\":{\"description\":\"The text, up to 2000 characters.\",\"type\":\"string\"},\"view\""
    ":{\"description\":\"GlobalId of the 2D view (a plan or a section; a default plan is stored on fi"
    "rst use).\",\"type\":\"string\"}},\"required\":[\"view\",\"at\",\"text\"],\"title\":\"AddTextArg"
    "s\",\"type\":\"object\"}";
static const char SUMMARY_ADD_TEXT[] = "Add a text to a 2D view at a point (a C addition).";
static const char SCHEMA_MOVE_ANNOTATION[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `move_annotation`.\",\"properties\":{\"by\":{\"description\":\"A tex"
    "t: moved by [dx, dy] mm; a dimension: its line moved across itself by that much.\",\"items\":{\""
    "format\":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":[\"array\",\"nul"
    "l\"]},\"id\":{\"description\":\"GlobalId of the dimension or text.\",\"type\":\"string\"},\"offs"
    "et\":{\"description\":\"A dimension: its new offset, mm.\",\"format\":\"double\",\"type\":[\"num"
    "ber\",\"null\"]},\"view\":{\"description\":\"GlobalId of the 2D view (a plan or a section; a def"
    "ault plan is stored on first use).\",\"type\":\"string\"}},\"required\":[\"view\",\"id\"],\"titl"
    "e\":\"MoveAnnotationArgs\",\"type\":\"object\"}";
static const char SUMMARY_MOVE_ANNOTATION[] =
    "Move a text, or a dimension's line (by, or a new offset), in its view (a C addition).";
static const char SCHEMA_DELETE_ANNOTATION[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `delete_annotation`.\",\"properties\":{\"id\":{\"description\":\"Glo"
    "balId of the dimension or text.\",\"type\":\"string\"},\"view\":{\"description\":\"GlobalId of t"
    "he 2D view (a plan or a section; a default plan is stored on first use).\",\"type\":\"string\"}}"
    ",\"required\":[\"view\",\"id\"],\"title\":\"DeleteAnnotationArgs\",\"type\":\"object\"}";
static const char SUMMARY_DELETE_ANNOTATION[] = "Delete a dimension or a text from its view (a C addition).";

/* C additions of ticket G1 (ADR 0056): levels and grids, handlers in verbs_levels.c. */
static const char SCHEMA_LEVEL_ADD[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `level_add`.\",\"properties\":{\"level_mm\":{\"description\":\"The l"
    "evel's height above project zero, mm (|level| <= 1e7).\",\"format\":\"double\",\"type\":\"number"
    "\"},\"name\":{\"description\":\"The level's name, not blank.\",\"type\":\"string\"},\"storey\":{"
    "\"description\":\"An IfcBuildingStorey that holds elements (default true); false: a datum level."
    "\",\"type\":[\"boolean\",\"null\"]}},\"required\":[\"name\",\"level_mm\"],\"title\":\"LevelAddAr"
    "gs\",\"type\":\"object\"}";
static const char SUMMARY_LEVEL_ADD[] =
    "Add a level (a storey by default, or a datum level with storey false) with a new GlobalId (a C a"
    "ddition).";
static const char SCHEMA_LEVEL_SET[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `level_set`: the fields to change, others kept.\",\"properties\":{\""
    "id\":{\"description\":\"GlobalId of the level.\",\"type\":\"string\"},\"level_mm\":{\"descriptio"
    "n\":\"The new level, mm; its elements move with it.\",\"format\":\"double\",\"type\":[\"number\""
    ",\"null\"]},\"name\":{\"description\":\"The new name, not blank.\",\"type\":[\"string\",\"null\""
    "]},\"storey\":{\"description\":\"true: a storey; false: a datum level (refused while elements ar"
    "e on it).\",\"type\":[\"boolean\",\"null\"]}},\"required\":[\"id\"],\"title\":\"LevelSetArgs\","
    "\"type\":\"object\"}";
static const char SUMMARY_LEVEL_SET[] =
    "Change a level's name, level (its elements move with it) or storey flag; missing ones are kept ("
    "a C addition).";
static const char SCHEMA_LEVEL_REMOVE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `level_remove`.\",\"properties\":{\"id\":{\"description\":\"GlobalId"
    " of the level.\",\"type\":\"string\"}},\"required\":[\"id\"],\"title\":\"LevelRemoveArgs\",\"typ"
    "e\":\"object\"}";
static const char SUMMARY_LEVEL_REMOVE[] =
    "Delete a level; refused while elements are on it (a C addition).";
static const char SCHEMA_GRID_ADD[] =
    "{\"$defs\":{\"AxisArgs\":{\"additionalProperties\":false,\"description\":\"One straight axis in "
    "plan, world mm.\",\"properties\":{\"end\":{\"description\":\"End [x, y], mm.\",\"items\":{\"form"
    "at\":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"start\""
    ":{\"description\":\"Start [x, y], mm.\",\"items\":{\"format\":\"double\",\"type\":\"number\"},\""
    "maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"tag\":{\"description\":\"Its tag (\\\"A\\\", "
    "\\\"1\\\"), unique in the grid.\",\"type\":\"string\"}},\"required\":[\"tag\",\"start\",\"end\"]"
    ",\"type\":\"object\"}},\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additional"
    "Properties\":false,\"description\":\"Arguments of `grid_add`: `axes`, or the rectangular form `x"
    "` and `y`.\",\"properties\":{\"axes\":{\"description\":\"The axes; they run in at least two dire"
    "ctions.\",\"items\":{\"$ref\":\"#/$defs/AxisArgs\"},\"type\":[\"array\",\"null\"]},\"name\":{\"d"
    "escription\":\"The grid's name, not blank.\",\"type\":\"string\"},\"tags\":{\"description\":\"Re"
    "ctangular form: one tag per axis, the x axes first; default 1, 2, ... and A, B, ...\",\"items\":"
    "{\"type\":\"string\"},\"type\":[\"array\",\"null\"]},\"x\":{\"description\":\"Rectangular form: "
    "x of the axes parallel to Y, mm; they run 1000 mm past the outer y axes.\",\"items\":{\"format\""
    ":\"double\",\"type\":\"number\"},\"type\":[\"array\",\"null\"]},\"y\":{\"description\":\"Rectang"
    "ular form: y of the axes parallel to X, mm.\",\"items\":{\"format\":\"double\",\"type\":\"number"
    "\"},\"type\":[\"array\",\"null\"]}},\"required\":[\"name\"],\"title\":\"GridAddArgs\",\"type\":"
    "\"object\"}";
static const char SUMMARY_GRID_ADD[] =
    "Add a grid: its axes, or a rectangular grid from x and y (tags 1, 2, ... and A, B, ...), with ne"
    "w GlobalIds (a C addition).";
static const char SCHEMA_GRID_AXIS_SET[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `grid_axis_set`: the fields to change, others kept.\",\"properties\""
    ":{\"axis\":{\"description\":\"GlobalId of the axis.\",\"type\":\"string\"},\"end\":{\"descriptio"
    "n\":\"New end [x, y], mm.\",\"items\":{\"format\":\"double\",\"type\":\"number\"},\"maxItems\":2"
    ",\"minItems\":2,\"type\":[\"array\",\"null\"]},\"grid\":{\"description\":\"GlobalId of the grid."
    "\",\"type\":\"string\"},\"start\":{\"description\":\"New start [x, y], mm.\",\"items\":{\"format"
    "\":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":[\"array\",\"null\"]},"
    "\"tag\":{\"description\":\"New tag, unique in the grid.\",\"type\":[\"string\",\"null\"]}},\"req"
    "uired\":[\"grid\",\"axis\"],\"title\":\"GridAxisSetArgs\",\"type\":\"object\"}";
static const char SUMMARY_GRID_AXIS_SET[] =
    "Change one grid axis: its tag, start or end; missing ones are kept (a C addition).";
static const char SCHEMA_GRID_REMOVE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `grid_remove`.\",\"properties\":{\"id\":{\"description\":\"GlobalId "
    "of the grid.\",\"type\":\"string\"}},\"required\":[\"id\"],\"title\":\"GridRemoveArgs\",\"type\""
    ":\"object\"}";
static const char SUMMARY_GRID_REMOVE[] =
    "Delete a grid and its axes (a C addition).";
int cc_vh_level_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_level_set(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_level_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_grid_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_grid_axis_set(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_grid_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);

/* ---- SLAB1: single grid axes, handlers in verbs_levels.c ---- */
static const char SCHEMA_GRID_AXIS_ADD[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `grid_axis_add`: one straight axis in plan, world mm.\",\"properties"
    "\":{\"end\":{\"description\":\"End [x, y], mm.\",\"items\":{\"format\":\"double\",\"type\":\"num"
    "ber\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"grid\":{\"description\":\"GlobalId of"
    " the grid.\",\"type\":\"string\"},\"start\":{\"description\":\"Start [x, y], mm.\",\"items\":{\""
    "format\":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"tag"
    "\":{\"description\":\"Its tag (\\\"A\\\", \\\"1\\\"), unique in the grid.\",\"type\":\"string\"}"
    "},\"required\":[\"grid\",\"tag\",\"start\",\"end\"],\"title\":\"GridAxisAddArgs\",\"type\":\"obj"
    "ect\"}";
static const char SUMMARY_GRID_AXIS_ADD[] =
    "Add one axis to a grid, with a new GlobalId (a C addition).";
static const char SCHEMA_GRID_AXIS_REMOVE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `grid_axis_remove`.\",\"properties\":{\"axis\":{\"description\":\"Gl"
    "obalId of the axis.\",\"type\":\"string\"},\"grid\":{\"description\":\"GlobalId of the grid.\","
    "\"type\":\"string\"}},\"required\":[\"grid\",\"axis\"],\"title\":\"GridAxisRemoveArgs\",\"type\":"
    "\"object\"}";
static const char SUMMARY_GRID_AXIS_REMOVE[] =
    "Delete one grid axis; the last axis of a direction stays (IfcGrid needs U and V axes; a C addition).";
int cc_vh_grid_axis_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_grid_axis_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
/* ---- end SLAB1 ---- */

/* C additions of ticket SH1: views placed on sheets, handlers in verbs_sheets.c. */
static const char SCHEMA_SHEET_PLACE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `sheet_place`.\",\"properties\":{\"at_mm\":{\"description\":\"Centre"
    " of the view's drawing on the paper, mm from the sheet's top-left corner.\",\"items\":{\"format\""
    ":\"double\",\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"scale\":{\"d"
    "escription\":\"1:N; default: the view's. Only the first placement of a view whose scale is not s"
    "aid may give another.\",\"format\":\"uint32\",\"minimum\":0,\"type\":[\"integer\",\"null\"]},\"s"
    "heet\":{\"description\":\"GlobalId of the sheet.\",\"type\":\"string\"},\"view\":{\"description\""
    ":\"GlobalId of the view.\",\"type\":\"string\"}},\"required\":[\"sheet\",\"view\",\"at_mm\"],\"ti"
    "tle\":\"SheetPlaceArgs\",\"type\":\"object\"}";
static const char SUMMARY_SHEET_PLACE[] =
    "Place a view on a sheet at at_mm (paper mm from the top-left corner), with a new GlobalId (a C a"
    "ddition).";
static const char SCHEMA_SHEET_MOVE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `sheet_move`.\",\"properties\":{\"at_mm\":{\"description\":\"The new"
    " centre on the paper, mm from the sheet's top-left corner.\",\"items\":{\"format\":\"double\",\"t"
    "ype\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},\"placed\":{\"description\":\""
    "GlobalId of the placed view.\",\"type\":\"string\"},\"sheet\":{\"description\":\"GlobalId of the "
    "sheet.\",\"type\":\"string\"}},\"required\":[\"sheet\",\"placed\",\"at_mm\"],\"title\":\"SheetMov"
    "eArgs\",\"type\":\"object\"}";
static const char SUMMARY_SHEET_MOVE[] = "Move a placed view on its sheet to at_mm (a C addition).";
static const char SCHEMA_SHEET_UNPLACE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `sheet_unplace`.\",\"properties\":{\"placed\":{\"description\":\"Glo"
    "balId of the placed view.\",\"type\":\"string\"},\"sheet\":{\"description\":\"GlobalId of the she"
    "et.\",\"type\":\"string\"}},\"required\":[\"sheet\",\"placed\"],\"title\":\"SheetUnplaceArgs\",\"t"
    "ype\":\"object\"}";
static const char SUMMARY_SHEET_UNPLACE[] = "Take a placed view off its sheet; the view stays (a C addition).";
static const char SCHEMA_SHEET_REMOVE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `sheet_remove`.\",\"properties\":{\"id\":{\"description\":\"GlobalId "
    "of the sheet.\",\"type\":\"string\"}},\"required\":[\"id\"],\"title\":\"SheetRemoveArgs\",\"type\""
    ":\"object\"}";
static const char SUMMARY_SHEET_REMOVE[] = "Delete a sheet; its views stay (a C addition).";
int cc_vh_sheet_place(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_sheet_move(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_sheet_unplace(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_sheet_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);

static const char SCHEMA_GEOREF_SET[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\""
    "description\":\"Arguments of `georef_set`.\",\"properties\":{\"clear\":{\"description\":\"Remo"
    "ve the georeference (the model's own coordinates only).\",\"type\":[\"boolean\",\"null\"]},\"c"
    "rs\":{\"description\":\"The survey system, e.g. \\\"EPSG:2056\\\" (LV95); \\\"\\\" for none.\""
    ",\"type\":[\"string\",\"null\"]},\"east_m\":{\"description\":\"What the survey system adds to "
    "x, m.\",\"format\":\"double\",\"type\":[\"number\",\"null\"]},\"height_m\":{\"description\":\""
    "What the survey system adds to z, m.\",\"format\":\"double\",\"type\":[\"number\",\"null\"]},\""
    "map_rotation_deg\":{\"description\":\"The turn of the model's x axis in the survey system, deg"
    "rees counter-clockwise.\",\"format\":\"double\",\"type\":[\"number\",\"null\"]},\"north_m\":{\""
    "description\":\"What the survey system adds to y, m.\",\"format\":\"double\",\"type\":[\"numbe"
    "r\",\"null\"]},\"rotation_deg\":{\"description\":\"Project north: degrees counter-clockwise fr"
    "om true north.\",\"format\":\"double\",\"type\":[\"number\",\"null\"]},\"survey_point_mm\":{\"des"
    "cription\":\"The survey point (a benchmark of the site), world mm [x, y, z]; [] removes it (SETOUT, A"
    "DR 0075).\",\"items\":{\"format\":\"double\",\"type\":\"number\"},\"type\":[\"array\",\"null\"]}},"
    "\"title\":\"GeorefSetArgs\",\"type\":\"object\"}";
static const char SUMMARY_GEOREF_SET[] =
    "Set the model's georeference: project north, the survey system (LV95), the map conversion and the survey "
    "point; missing ones are kept, angles come back in (-180, 180] (a C addition, ADR 0045, 0075).";
int cc_vh_georef_set(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
/* ---- SEC1 ---- C additions of ticket SEC1: views, handlers in verbs_views.c. */
static const char SCHEMA_VIEW_ADD[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `view_add`: a new view (kind), or a copy of one (from).\",\"properti"
    "es\":{\"depth_mm\":{\"description\":\"A section's depth beyond the cut, mm; 0 (default): all.\",\"form"
    "at\":\"double\",\"type\":[\"number\",\"null\"]},\"facing\":{\"description\":\"A section's look direction.\",\"enum\":[\"north\",\"east\","
    "\"south\",\"west\",null],\"type\":[\"string\",\"null\"]},\"from\":{\"description\":\"GlobalId of"
    " a view to duplicate (its copy is named \\\"<name> (copy)\\\").\",\"type\":[\"string\",\"null\"]"
    "},\"kind\":{\"description\":\"plan, ceiling (a ceiling plan looking up), section or three_d.\","
    "\"enum\":[\"plan\",\"ceiling\",\"section\",\"three_d\",null],\"type\":[\"string\",\"null\"]},\"n"
    "ame\":{\"description\":\"The view's name; default: as Rust's viewer names a new view.\",\"type\""
    ":[\"string\",\"null\"]},\"origin\":{\"description\":\"A section's cut: a point it goes through, "
    "world mm [x, y]; default: the middle of the model.\",\"items\":{\"format\":\"double\",\"type\":"
    "\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":[\"array\",\"null\"]},\"scale\":{\"descripti"
    "on\":\"1:N the view is drawn at (1 to 5000); default 100.\",\"format\":\"uint32\",\"type\":[\"integer\",\"null\"]},\"storey\":{\"descripti"
    "on\":\"A plan's level: its GlobalId.\",\"type\":[\"string\",\"null\"]}},\"title\":\"ViewAddArgs"
    "\",\"type\":\"object\"}";
static const char SUMMARY_VIEW_ADD[] =
    "Add a view: a plan, a ceiling plan, a section or a 3D view, named as Rust's viewer names it, or a copy of a view (a C addition).";
static const char SCHEMA_VIEW_REMOVE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `view_remove`.\",\"properties\":{\"id\":{\"description\":\"GlobalId "
    "of a stored view (a changed default view goes back to its default).\",\"type\":\"string\"}},\"re"
    "quired\":[\"id\"],\"title\":\"ViewRemoveArgs\",\"type\":\"object\"}";
static const char SUMMARY_VIEW_REMOVE[] =
    "Delete a stored view; a changed default view goes back to its default (a C addition).";
static const char SCHEMA_VIEW_EDIT[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `view_edit`: the fields to change, others kept.\",\"properties\":{\""
    "depth_mm\":{\"description\":\"A section's depth beyond the cut, mm; 0: all.\",\"format\":\"doubl"
    "e\",\"type\":[\"number\",\"null\"]},\"facing\":{\"description\":\"A section's look direction.\","
    "\"enum\":[\"north\",\"east\",\"south\",\"west\",null],\"type\":[\"string\",\"null\"]},\"id\":{\""
    "description\":\"GlobalId of the view (a default view is stored on its first change).\",\"type\":"
    "\"string\"},\"levels\":{\"description\":\"A plan's view range: the level each plane refers to (o"
    "ffsets: set_view).\",\"properties\":{\"bottom\":{\"description\":\"associated, above, below or u"
    "nlimited.\",\"enum\":[\"associated\",\"above\",\"below\",\"unlimited\",null],\"type\":[\"string"
    "\",\"null\"]},\"cut\":{\"description\":\"associated, above, below or unlimited.\",\"enum\":[\"as"
    "sociated\",\"above\",\"below\",\"unlimited\",null],\"type\":[\"string\",\"null\"]},\"depth\":{\""
    "description\":\"associated, above, below or unlimited.\",\"enum\":[\"associated\",\"above\",\"be"
    "low\",\"unlimited\",null],\"type\":[\"string\",\"null\"]},\"top\":{\"description\":\"associated,"
    " above, below or unlimited.\",\"enum\":[\"associated\",\"above\",\"below\",\"unlimited\",null],"
    "\"type\":[\"string\",\"null\"]}},\"type\":[\"object\",\"null\"]},\"name\":{\"description\":\"The"
    " new name, not blank.\",\"type\":[\"string\",\"null\"]},\"origin\":{\"description\":\"A section'"
    "s cut point, world mm [x, y].\",\"items\":{\"format\":\"double\",\"type\":\"number\"},\"maxItems"
    "\":2,\"minItems\":2,\"type\":[\"array\",\"null\"]},\"visibility\":{\"description\":\"What the vi"
    "ew hides: each list given replaces the view's (sorted, unique).\",\"properties\":{\"hidden_class"
    "es\":{\"description\":\"IFC classes, e.g. IfcSlab.\",\"items\":{\"type\":\"string\"},\"type\":["
    "\"array\",\"null\"]},\"hidden_elements\":{\"description\":\"GlobalIds hidden in the view.\",\"it"
    "ems\":{\"type\":\"string\"},\"type\":[\"array\",\"null\"]},\"hidden_families\":{\"description\":"
    "\"Material families.\",\"items\":{\"type\":\"string\"},\"type\":[\"array\",\"null\"]},\"hidden_l"
    "inks\":{\"description\":\"Linked models by name.\",\"items\":{\"type\":\"string\"},\"type\":[\"a"
    "rray\",\"null\"]},\"hidden_materials\":{\"description\":\"Material names.\",\"items\":{\"type\":"
    "\"string\"},\"type\":[\"array\",\"null\"]},\"hidden_storeys\":{\"description\":\"Storey names.\""
    ",\"items\":{\"type\":\"string\"},\"type\":[\"array\",\"null\"]},\"isolated_elements\":{\"descrip"
    "tion\":\"GlobalIds isolated in the view.\",\"items\":{\"type\":\"string\"},\"type\":[\"array\","
    "\"null\"]}},\"type\":[\"object\",\"null\"]}},\"required\":[\"id\"],\"title\":\"ViewEditArgs\",\""
    "type\":\"object\"}";
static const char SUMMARY_VIEW_EDIT[] =
    "Change a view's name, a section's cut (origin, facing, depth), a plan's range levels or what the view hides (a C addition).";
int cc_vh_view_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_view_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_view_edit(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
/* ---- end SEC1 ---- */
/* ---- MAT1: the materials (verbs_materials.h), C additions ---- */
static const char SCHEMA_MATERIAL_ADD[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `material_add`: a library grade, or a blank material by name.\",\"pr"
    "operties\":{\"category\":{\"description\":\"The material family (default: the grade's, or other)"
    ".\",\"enum\":[\"concrete\",\"steel\",\"timber\",\"masonry\",\"insulation\",\"earth\",\"glass\","
    "\"other\",null],\"type\":[\"string\",\"null\"]},\"library\":{\"description\":\"A grade of the li"
    "brary (query material_library), e.g. \\\"C30/37\\\".\",\"type\":[\"string\",\"null\"]},\"name\":"
    "{\"description\":\"The material's name (default: the grade's); required without `library`.\",\"t"
    "ype\":[\"string\",\"null\"]}},\"title\":\"MaterialAddArgs\",\"type\":\"object\"}";
static const char SUMMARY_MATERIAL_ADD[] =
    "Add a material: a grade of the library with its values, or a blank one by name (a C addition).";
static const char SCHEMA_MATERIAL_SET[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `material_set`: the fields to change, others kept.\",\"properties\":"
    "{\"category\":{\"description\":\"The material family.\",\"enum\":[\"concrete\",\"steel\",\"timbe"
    "r\",\"masonry\",\"insulation\",\"earth\",\"glass\",\"other\",null],\"type\":[\"string\",\"null\""
    "]},\"id\":{\"description\":\"GlobalId of the material.\",\"type\":\"string\"},\"name\":{\"descri"
    "ption\":\"The new name, not blank, unique.\",\"type\":[\"string\",\"null\"]},\"props\":{\"additi"
    "onalProperties\":false,\"description\":\"The values (all four replaced; {} clears them).\",\"pro"
    "perties\":{\"density_kg_m3\":{\"description\":\"kg/m³, up to 30000.\",\"format\":\"double\",\"ty"
    "pe\":[\"number\",\"null\"]},\"e_mpa\":{\"description\":\"Modulus of elasticity, MPa, up to 10000"
    "00.\",\"format\":\"double\",\"type\":[\"number\",\"null\"]},\"source\":{\"description\":\"Where "
    "the values come from, at most 200 characters.\",\"type\":[\"string\",\"null\"]},\"strength_mpa\""
    ":{\"description\":\"Characteristic strength, MPa, up to 10000.\",\"format\":\"double\",\"type\":"
    "[\"number\",\"null\"]}},\"type\":[\"object\",\"null\"]}},\"required\":[\"id\"],\"title\":\"Mater"
    "ialSetArgs\",\"type\":\"object\"}";
static const char SUMMARY_MATERIAL_SET[] =
    "Change a material's name, family or values (a C addition; the command set_material).";
static const char SCHEMA_MATERIAL_REMOVE[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"additionalProperties\":false,\"d"
    "escription\":\"Arguments of `material_remove`.\",\"properties\":{\"id\":{\"description\":\"Globa"
    "lId of the material.\",\"type\":\"string\"}},\"required\":[\"id\"],\"title\":\"MaterialRemoveArg"
    "s\",\"type\":\"object\"}";
static const char SUMMARY_MATERIAL_REMOVE[] =
    "Delete a material; refused while elements are made of it (a C addition).";
/* ---- end MAT1 ---- */
#define V(n, N) {#n, SUMMARY_##N, SCHEMA_##N, cc_vh_##n, 0}
const cc_verb_def cc_verb_table[] = {
    V(place, PLACE),
    V(move, MOVE),
    V(copy, COPY),
    V(delete, DELETE),
    V(set_generator, SET_GENERATOR),
    V(set_view, SET_VIEW),
    V(set_schedule, SET_SCHEDULE),
    V(set_material, SET_MATERIAL),
    V(set_note, SET_NOTE),
    V(set_project_field, SET_PROJECT_FIELD),
    V(rotate, ROTATE),
    V(mirror, MIRROR),
    V(add_sheet, ADD_SHEET),
    V(set_sheet, SET_SHEET),
};
const cc_verb_def cc_verb_c_additions[] = {
    V(rename, RENAME),
    V(add_dimension, ADD_DIMENSION),
    V(add_text, ADD_TEXT),
    V(move_annotation, MOVE_ANNOTATION),
    V(delete_annotation, DELETE_ANNOTATION),
    V(level_add, LEVEL_ADD),
    V(level_set, LEVEL_SET),
    V(level_remove, LEVEL_REMOVE),
    V(grid_add, GRID_ADD),
    V(grid_axis_set, GRID_AXIS_SET),
    V(grid_remove, GRID_REMOVE),
    V(grid_axis_add, GRID_AXIS_ADD),       /* SLAB1 */
    V(grid_axis_remove, GRID_AXIS_REMOVE), /* SLAB1 */
    /* ticket SH1 */
    V(sheet_place, SHEET_PLACE),
    V(sheet_move, SHEET_MOVE),
    V(sheet_unplace, SHEET_UNPLACE),
    V(sheet_remove, SHEET_REMOVE),
    V(georef_set, GEOREF_SET),
    /* ---- SEC1 ---- */
    V(view_add, VIEW_ADD),
    V(view_remove, VIEW_REMOVE),
    V(view_edit, VIEW_EDIT),
    /* ---- end SEC1 ---- */
    /* ---- MAT1 ---- */
    V(material_add, MATERIAL_ADD),
    V(material_set, MATERIAL_SET),
    V(material_remove, MATERIAL_REMOVE),
    /* ---- end MAT1 ---- */
};
#undef V
const size_t cc_verb_count = sizeof cc_verb_table / sizeof cc_verb_table[0];
const size_t cc_verb_c_addition_count = sizeof cc_verb_c_additions / sizeof cc_verb_c_additions[0];

/* strcmp(a, b) == 0 (freestanding wasm has no strcmp). */
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

const cc_verb_def *cc_verb_find(const char *name) {
    for (size_t i = 0; i < cc_verb_count; i++)
        if (streq(cc_verb_table[i].name, name)) return &cc_verb_table[i];
    for (size_t i = 0; i < cc_verb_c_addition_count; i++)
        if (streq(cc_verb_c_additions[i].name, name)) return &cc_verb_c_additions[i];
    return NULL;
}

void cc_verbs_list_json(Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    for (size_t i = 0; i < cc_verb_count; i++) {
        jw_obj_begin(&w);
        jw_key(&w, "name");
        jw_str(&w, cc_verb_table[i].name);
        jw_key(&w, "summary");
        jw_str(&w, cc_verb_table[i].summary);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_finish(&w);
}
