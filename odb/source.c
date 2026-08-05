#include "git-compat-util.h"
#include "repository.h"
#include "odb/source-vex.h"
#include "object-file.h"
#include "odb/source-files.h"
#include "odb/source.h"
#include "packfile.h"

struct odb_source *odb_source_new(struct object_database *odb,
				  const char *path,
				  bool local)
{
	if (odb->repo->ref_storage_format == REF_STORAGE_FORMAT_VEX)
		return odb_source_vex_new(odb, path, local);
	return &odb_source_files_new(odb, path, local)->base;
}

void odb_source_init(struct odb_source *source,
		     struct object_database *odb,
		     enum odb_source_type type,
		     const char *path,
		     bool local)
{
	source->odb = odb;
	source->type = type;
	source->local = local;
	source->path = xstrdup(path);
}

void odb_source_free(struct odb_source *source)
{
	if (!source)
		return;
	source->free(source);
}

void odb_source_release(struct odb_source *source)
{
	if (!source)
		return;
	free(source->path);
}

struct odb_source_packed *odb_source_get_packed(struct odb_source *source)
{
	if (!source)
		return NULL;
	switch (source->type) {
	case ODB_SOURCE_FILES:
		return odb_source_files_downcast(source)->packed;
	case ODB_SOURCE_VEX:
		return odb_source_vex_packed_store(source);
	default:
		return NULL;
	}
}
