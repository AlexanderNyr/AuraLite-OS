/* userspace/office/libaodoc/aopackage.h — native .aod ZIP package. */
#ifndef AURALITE_OFFICE_AOPACKAGE_H
#define AURALITE_OFFICE_AOPACKAGE_H

#include <stddef.h>
#include <stdint.h>

#include "aoio.h"
#include "aozip.h"

#define AO_PACKAGE_MIMETYPE "application/vnd.auralite.aod"
#define AO_PACKAGE_MIMETYPE_PART "mimetype"
#define AO_PACKAGE_CONTENT_PART "content.xml"
#define AO_PACKAGE_STYLES_PART  "styles.xml"

/* Measure the XML once, then serialize it directly into a deterministic,
 * store-only ZIP stream. No full XML or archive copy is held in RAM. */
int ao_package_export(const ao_model_t *model,
                      ao_zip_write_fn write, void *context,
                      size_t *archive_size, size_t *content_xml_size);

/* Sniff ZIP magic and import a native package; legacy OF2–OF6 plain XML is
 * accepted unchanged. Compressed package parts use caller-owned scratch. */
int ao_package_import(ao_model_t *model, const void *bytes, size_t length,
                      char *scratch, size_t scratch_cap);

#endif /* AURALITE_OFFICE_AOPACKAGE_H */
