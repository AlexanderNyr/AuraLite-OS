/* userspace/office/libaodoc/aoxml.h — bounded pull XML reader and escaper.
 *
 * Tokens are slices of the caller's source; parsing never allocates or copies
 * a text node.  The explicit element stack has a hard depth cap, DTD/entity
 * declarations are refused, and every token is length-bounded by the input.
 */
#ifndef AURALITE_OFFICE_AOXML_H
#define AURALITE_OFFICE_AOXML_H

#include <stddef.h>
#include <stdint.h>

#define AO_XML_MAX_INPUT_BYTES (4u * 1024u * 1024u)
#define AO_XML_MAX_DEPTH       64u
#define AO_XML_MAX_NAME        127u
#define AO_XML_MAX_ATTRS       32u
#define AO_XML_MAX_ATTR_BYTES  4096u

typedef enum {
    AO_XML_T_EOF = 0,
    AO_XML_T_START,
    AO_XML_T_END,
    AO_XML_T_TEXT,
    AO_XML_T_CDATA
} ao_xml_type_t;

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;       /* entity-escaped source slice */
    size_t value_len;
} ao_xml_attr_t;

typedef struct {
    ao_xml_type_t type;
    const char *name;
    size_t name_len;
    const char *text;
    size_t text_len;
    uint32_t attr_count;
    ao_xml_attr_t attrs[AO_XML_MAX_ATTRS];
    uint32_t empty;          /* START token ended in '/>' */
    uint32_t line, col;      /* 1-based token start */
} ao_xml_token_t;

typedef struct {
    const char *src;
    size_t len, pos;
    uint32_t line, col;
    uint32_t depth;
    uint16_t stack_len[AO_XML_MAX_DEPTH];
    char stack_names[AO_XML_MAX_DEPTH][AO_XML_MAX_NAME + 1u];
    uint8_t root_seen, root_closed;
    uint8_t failed, eof_seen;
    uint8_t allow_legacy_merge_mark; /* accepts AO's historical &#1; marker */
} ao_xml_reader_t;

#define AO_XML_OK             0
#define AO_XML_E_ARG         -1
#define AO_XML_E_FORMAT      -2
#define AO_XML_E_LIMIT       -3
#define AO_XML_E_UTF8        -4
#define AO_XML_E_FULL        -5
#define AO_XML_E_WRITE       -6

/* Strict XML 1.0 reader. `len` is authoritative; source need not be NUL-ended. */
int ao_xml_reader_init(ao_xml_reader_t *r, const char *src, size_t len);
/* Native-Office compatibility mode accepts the historical numeric &#1;
 * encoding of the table-cell merge marker; all other XML 1.0 rules remain. */
int ao_xml_reader_init_native(ao_xml_reader_t *r, const char *src, size_t len);
/* Returns AO_XML_OK and fills token (including EOF), or a negative AO_XML_E_*. */
int ao_xml_next(ao_xml_reader_t *r, ao_xml_token_t *token);

/* Decode one text/attribute slice, entity-aware. `out == NULL` validates and
 * counts decoded bytes without copying. `cap` includes no implicit NUL. */
int ao_xml_decode(const char *src, size_t len, char *out, size_t cap,
                  size_t *out_len);
int ao_xml_decode_native(const char *src, size_t len, char *out, size_t cap,
                         size_t *out_len);

/* Escape UTF-8 XML character data or an attribute value. Quotes and apostrophes
 * are always escaped for deterministic output. No NUL terminator is appended;
 * out == NULL, cap == 0 validates and counts the escaped byte length. */
int ao_xml_escape(const char *src, size_t len, int attribute,
                  char *out, size_t cap, size_t *out_len);

typedef int (*ao_xml_write_fn)(void *context, const void *bytes, size_t length);
/* Same escaping operation, emitting bounded pieces to a sink instead of
 * requiring a second full-size copy of the XML document. A sink failure
 * returns AO_XML_E_WRITE; the sink owner can retain its underlying error. */
int ao_xml_escape_stream(const char *src, size_t len, int attribute,
                         ao_xml_write_fn write, void *context,
                         size_t *out_len);

/* Consume the complete document and validate nesting, entities and UTF-8. */
int ao_xml_validate(const char *src, size_t len);
int ao_xml_validate_native(const char *src, size_t len);

#endif
