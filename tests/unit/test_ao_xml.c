/* tests/unit/test_ao_xml.c — OF7's bounded XML reader and escaper. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "userspace/office/libaodoc/aoxml.h"

static unsigned checks;
static unsigned failures;
#define CHECK(expr) do { checks++; if (expr) printf("PASS: %s\n", #expr); \
    else { failures++; printf("FAIL: %s (line %d)\n", #expr, __LINE__); } } while (0)

static char huge_xml[1024u * 1024u + 32u];
static char deep_xml[10u * 7u * 10000u + 1u];
static char attrs_xml[4096];

typedef struct { char *out; size_t cap, len; } xml_buffer_t;
static int xml_buffer_write(void *context, const void *bytes, size_t length) {
    xml_buffer_t *b = (xml_buffer_t *)context;
    if (!b || length > b->cap - b->len) return -1;
    if (length) memcpy(b->out + b->len, bytes, length);
    b->len += length;
    return 0;
}
static int xml_write_fail(void *context, const void *bytes, size_t length) {
    (void)context; (void)bytes; (void)length;
    return -1;
}

static void test_escape_decode(void) {
    static const char source[] = "& < > \" '";
    static const char expected[] = "&amp; &lt; &gt; &quot; &apos;";
    char out[128]; size_t n = 0;
    int rc = ao_xml_escape(source, sizeof(source) - 1u, 1,
                           out, sizeof(out), &n);
    CHECK(rc == AO_XML_OK && n == sizeof(expected) - 1u &&
          memcmp(out, expected, sizeof(expected) - 1u) == 0);
    CHECK(ao_xml_escape(source, sizeof(source) - 1u, 0, NULL, 0, &n) == AO_XML_OK &&
          n == sizeof(expected) - 1u);
    xml_buffer_t streamed = { out, sizeof(out), 0 };
    CHECK(ao_xml_escape_stream(source, sizeof(source) - 1u, 0,
                               xml_buffer_write, &streamed, &n) == AO_XML_OK &&
          n == sizeof(expected) - 1u && streamed.len == n &&
          memcmp(out, expected, n) == 0);
    CHECK(ao_xml_escape_stream(source, sizeof(source) - 1u, 0,
                               xml_write_fail, NULL, &n) == AO_XML_E_WRITE);
    CHECK(ao_xml_escape(source, sizeof(source) - 1u, 0, out, 5, &n) == AO_XML_E_FULL);

    static const char encoded[] = "A&amp;&lt;&#x1F642;&#65;";
    static const char decoded[] = "A&<\xF0\x9F\x99\x82" "A";
    rc = ao_xml_decode(encoded, sizeof(encoded) - 1u, out, sizeof(out), &n);
    CHECK(rc == AO_XML_OK && n == sizeof(decoded) - 1u &&
          memcmp(out, decoded, sizeof(decoded) - 1u) == 0);
    CHECK(ao_xml_decode("&external;", 10, out, sizeof(out), &n) == AO_XML_E_FORMAT);
    CHECK(ao_xml_decode("&#0;", 4, out, sizeof(out), &n) == AO_XML_E_FORMAT);
    CHECK(ao_xml_decode("&#x110000;", 10, out, sizeof(out), &n) == AO_XML_E_FORMAT);
    CHECK(ao_xml_decode_native("&#1;", 4, out, sizeof(out), &n) == AO_XML_OK &&
          n == 1 && (uint8_t)out[0] == 1u);
    CHECK(ao_xml_decode("&#1;", 4, out, sizeof(out), &n) == AO_XML_E_FORMAT);
}

static void test_pull_and_bom(void) {
    static const char xml[] =
        "\xEF\xBB\xBF<?xml version=\"1.0\"?><!--before-->"
        "<root a='A&amp;B'>left&amp;<![CDATA[<raw>]]><!--inside-->right</root>"
        "<!--after-->";
    ao_xml_reader_t r;
    ao_xml_token_t t;
    CHECK(ao_xml_reader_init(&r, xml, sizeof(xml) - 1u) == AO_XML_OK);
    CHECK(ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_START &&
          t.name_len == 4 && memcmp(t.name, "root", 4) == 0 && t.col > 1);
    CHECK(t.attr_count == 1 && t.attrs[0].value_len == 7);
    char attr[32]; size_t n = 0;
    CHECK(ao_xml_decode(t.attrs[0].value, t.attrs[0].value_len,
                        attr, sizeof(attr), &n) == AO_XML_OK && n == 3 &&
          memcmp(attr, "A&B", 3) == 0);
    CHECK(ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_TEXT);
    CHECK(ao_xml_decode(t.text, t.text_len, attr, sizeof(attr), &n) == AO_XML_OK &&
          n == 5 && memcmp(attr, "left&", 5) == 0);
    CHECK(ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_CDATA &&
          t.text_len == 5 && memcmp(t.text, "<raw>", 5) == 0);
    CHECK(ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_TEXT &&
          t.text_len == 5 && memcmp(t.text, "right", 5) == 0);
    CHECK(ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_END &&
          t.name_len == 4 && memcmp(t.name, "root", 4) == 0);
    CHECK(ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_EOF);
    CHECK(ao_xml_validate(xml, sizeof(xml) - 1u) == AO_XML_OK);
    {
        static const char bom_only[] = "\xEF\xBB\xBF<r/>";
        CHECK(ao_xml_reader_init(&r, bom_only, sizeof(bom_only) - 1u) == AO_XML_OK &&
              ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_START &&
              t.col == 1);
    }
    CHECK(ao_xml_validate_native("<a>&#1;</a>", 11) == AO_XML_OK);
    CHECK(ao_xml_validate("<a>&#1;</a>", 11) == AO_XML_E_FORMAT);
    {
        const char raw_control[] = { '<', 'a', '>', 1, '<', '/', 'a', '>' };
        CHECK(ao_xml_validate_native(raw_control, sizeof(raw_control)) == AO_XML_E_FORMAT);
    }
}

static void test_size_and_depth(void) {
    const size_t payload = 1024u * 1024u;
    memcpy(huge_xml, "<r>", 3);
    memset(huge_xml + 3, 'x', payload);
    memcpy(huge_xml + 3 + payload, "</r>", 4);
    size_t length = payload + 7u;
    ao_xml_reader_t r; ao_xml_token_t t;
    CHECK(ao_xml_validate(huge_xml, length) == AO_XML_OK);
    CHECK(ao_xml_reader_init(&r, huge_xml, length) == AO_XML_OK);
    CHECK(ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_START);
    CHECK(ao_xml_next(&r, &t) == AO_XML_OK && t.type == AO_XML_T_TEXT &&
          t.text_len == payload);

    size_t at = 0;
    for (unsigned i = 0; i < 10000u; i++) {
        memcpy(deep_xml + at, "<x>", 3); at += 3;
    }
    for (unsigned i = 0; i < 10000u; i++) {
        memcpy(deep_xml + at, "</x>", 4); at += 4;
    }
    CHECK(ao_xml_validate(deep_xml, at) == AO_XML_E_LIMIT);
    CHECK(ao_xml_reader_init(&r, "<r/>", AO_XML_MAX_INPUT_BYTES + 1u) == AO_XML_E_LIMIT);
}

static void test_refusals_and_limits(void) {
    CHECK(ao_xml_validate("<a><b></a>", 11) == AO_XML_E_FORMAT);
    CHECK(ao_xml_validate("<a x=\"1\" x=\"2\"/>", sizeof("<a x=\"1\" x=\"2\"/>") - 1u) == AO_XML_E_FORMAT);
    CHECK(ao_xml_validate("<!DOCTYPE a><a/>", sizeof("<!DOCTYPE a><a/>") - 1u) == AO_XML_E_FORMAT);
    CHECK(ao_xml_validate("<a>&bogus;</a>", sizeof("<a>&bogus;</a>") - 1u) == AO_XML_E_FORMAT);
    CHECK(ao_xml_validate("<a/><b/>", 8) == AO_XML_E_FORMAT);
    CHECK(ao_xml_validate("text<a/>", 8) == AO_XML_E_FORMAT);
    CHECK(ao_xml_validate("<a>bad]]>text</a>", 17) == AO_XML_E_FORMAT);
    CHECK(ao_xml_validate("<a><!-- bad -- comment --></a>",
                          sizeof("<a><!-- bad -- comment --></a>") - 1u) == AO_XML_E_FORMAT);
    {
        char *p = attrs_xml; size_t left = sizeof(attrs_xml);
        int n = snprintf(p, left, "<r");
        p += n; left -= (size_t)n;
        for (unsigned i = 0; i < AO_XML_MAX_ATTRS + 1u; i++) {
            n = snprintf(p, left, " a%u=\"v\"", i);
            p += n; left -= (size_t)n;
        }
        n = snprintf(p, left, "/>");
        p += n;
        CHECK(ao_xml_validate(attrs_xml, (size_t)(p - attrs_xml)) == AO_XML_E_LIMIT);
    }
    {
        const char bad_utf8[] = { '<','a','>',(char)0xC0,(char)0x80,'<','/','a','>' };
        CHECK(ao_xml_validate(bad_utf8, sizeof(bad_utf8)) == AO_XML_E_UTF8);
    }
    {
        const char nul_xml[] = { '<','a','>',0,'<','/','a','>' };
        CHECK(ao_xml_validate(nul_xml, sizeof(nul_xml)) == AO_XML_E_FORMAT);
    }
}

int main(void) {
    test_escape_decode();
    test_pull_and_bom();
    test_size_and_depth();
    test_refusals_and_limits();
    printf("checks=%u fails=%u\n", checks, failures);
    return failures ? 1 : 0;
}
