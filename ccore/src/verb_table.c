/* See verb_table.h. The schema and summary texts below are Rust's (crates/bimble-api/src/
 * dispatch.rs), compacted from golden/verbs_schema/<verb>.json and golden/verbs_list.json. */
#include "verb_table.h"
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
