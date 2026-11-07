/*
 * test_usb_msc_csw.c — RESIDUE3 C5b: the USB BOT command-status wrapper.
 *
 * drivers/usb/msc_csw.c is compiled UNMODIFIED and driven with byte arrays.
 * The cases are the ones a working QEMU storage device never produces: a
 * truncated wrapper, an over-long one, a stale tag, and the two status codes
 * BOT defines (failed = 1, phase error = 2).
 *
 * The first case is the bug this pins: before C5 the driver checked the
 * signature of a zeroed struct after a short read, so 4 bytes of nothing
 * produced "bad CSW signature 0x0" -- a message about the wrong layer.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/usb/msc_csw.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a,e) do { long _a=(long)(a), _e=(long)(e); if (_a!=_e) { \
    printf("  FAIL L%d: %s = %ld, want %ld\n", __LINE__, #a, _a, _e); failed++; } } while (0)

#define TAG 0x11223344u

/* A well-formed CSW: "USBS", tag, residue, status. */
static void build_csw(uint8_t *b, uint32_t sig, uint32_t tag, uint32_t res, uint8_t st) {
    b[0] = (uint8_t)(sig);       b[1] = (uint8_t)(sig >> 8);
    b[2] = (uint8_t)(sig >> 16); b[3] = (uint8_t)(sig >> 24);
    b[4] = (uint8_t)(tag);       b[5] = (uint8_t)(tag >> 8);
    b[6] = (uint8_t)(tag >> 16); b[7] = (uint8_t)(tag >> 24);
    b[8] = (uint8_t)(res);       b[9] = (uint8_t)(res >> 8);
    b[10] = (uint8_t)(res >> 16); b[11] = (uint8_t)(res >> 24);
    b[12] = st;
}

static void test_good_csw(void) {
    uint8_t b[MSC_CSW_LEN];
    build_csw(b, MSC_CSW_SIGNATURE, TAG, 0, 0);
    msc_csw_check_t c;
    msc_csw_validate(b, (int)sizeof(b), TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_OK);
    CHECK_EQ(c.signature, 0x53425355u);
    CHECK_EQ(c.tag, TAG);
    CHECK_EQ(c.residue, 0);
    CHECK_EQ(c.status, 0);
    CHECK(strcmp(msc_csw_result_name(c.result), "ok") == 0);
}

/* The C5b case: 4 bytes.  Must be SHORT, not a signature complaint, and no
 * field may be decoded from a buffer that was not fully received. */
static void test_short_csw_is_not_a_signature_error(void) {
    uint8_t b[4] = { 0x55, 0x53, 0x42, 0x53 };   /* "USBS" and nothing else */
    msc_csw_check_t c;
    msc_csw_validate(b, 4, TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_SHORT);
    CHECK_EQ(c.signature, 0);
    CHECK_EQ(c.tag, 0);
    CHECK_EQ(c.residue, 0);
    CHECK(strstr(msc_csw_result_name(c.result), "short") != NULL);
}

/* One byte short of a wrapper: still short, still zero fields. */
static void test_twelve_bytes(void) {
    uint8_t b[MSC_CSW_LEN];
    build_csw(b, MSC_CSW_SIGNATURE, TAG, 0, 0);
    for (int n = 1; n < (int)MSC_CSW_LEN; n++) {
        msc_csw_check_t c;
        msc_csw_validate(b, n, TAG, &c);
        CHECK_EQ(c.result, MSC_CSW_SHORT);
    }
}

/* One byte too many is its own error: the phases disagree. */
static void test_over_long_csw(void) {
    uint8_t b[MSC_CSW_LEN + 1];
    build_csw(b, MSC_CSW_SIGNATURE, TAG, 0, 0);
    b[MSC_CSW_LEN] = 0xAA;
    msc_csw_check_t c;
    msc_csw_validate(b, MSC_CSW_LEN + 1, TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_LONG);
    CHECK(strstr(msc_csw_result_name(c.result), "over-long") != NULL);
}

static void test_bad_signature(void) {
    uint8_t b[MSC_CSW_LEN];
    build_csw(b, 0xDEADBEEF, TAG, 0, 0);
    msc_csw_check_t c;
    msc_csw_validate(b, (int)sizeof(b), TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_BAD_SIGNATURE);
    CHECK_EQ(c.signature, 0xDEADBEEFu);
}

static void test_stale_tag(void) {
    uint8_t b[MSC_CSW_LEN];
    build_csw(b, MSC_CSW_SIGNATURE, TAG + 1, 0, 0);
    msc_csw_check_t c;
    msc_csw_validate(b, (int)sizeof(b), TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_BAD_TAG);
    CHECK_EQ(c.tag, TAG + 1);
}

/* Both failure statuses, with the residue the device reported preserved so the
 * log can say how many bytes never made it. */
static void test_failed_status_keeps_residue(void) {
    uint8_t b[MSC_CSW_LEN];
    build_csw(b, MSC_CSW_SIGNATURE, TAG, 512, 1);
    msc_csw_check_t c;
    msc_csw_validate(b, (int)sizeof(b), TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_BAD_STATUS);
    CHECK_EQ(c.status, 1);
    CHECK_EQ(c.residue, 512);
}

static void test_phase_error_status(void) {
    uint8_t b[MSC_CSW_LEN];
    build_csw(b, MSC_CSW_SIGNATURE, TAG, 0, 2);
    msc_csw_check_t c;
    msc_csw_validate(b, (int)sizeof(b), TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_BAD_STATUS);
    CHECK_EQ(c.status, 2);
}

/* Nothing at all: a failed transfer, reported as short, not as garbage. */
static void test_null_and_zero(void) {
    msc_csw_check_t c;
    msc_csw_validate(NULL, 13, TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_SHORT);
    uint8_t b[MSC_CSW_LEN];
    memset(b, 07, sizeof(b));
    msc_csw_validate(b, 0, TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_SHORT);
    msc_csw_validate(b, -5, TAG, &c);
    CHECK_EQ(c.result, MSC_CSW_SHORT);
    msc_csw_validate(b, (int)sizeof(b), TAG, NULL);   /* must not fault */
}

/* Every result name is distinct and non-empty: the log lines are asserted by
 * pattern in the integration cases, so two names that collapsed would make a
 * grep for one match the other. */
static void test_result_names_distinct(void) {
    msc_csw_result_t all[] = { MSC_CSW_OK, MSC_CSW_SHORT, MSC_CSW_LONG,
                               MSC_CSW_BAD_SIGNATURE, MSC_CSW_BAD_TAG, MSC_CSW_BAD_STATUS };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *a = msc_csw_result_name(all[i]);
        CHECK(a && *a);
        for (unsigned j = i + 1; j < sizeof(all) / sizeof(all[0]); j++)
            CHECK(strcmp(a, msc_csw_result_name(all[j])) != 0);
    }
}

int main(void) {
    printf("=== AuraLite OS USB MSC command-status-wrapper tests ===\n");
    RUN(test_good_csw);
    RUN(test_short_csw_is_not_a_signature_error);
    RUN(test_twelve_bytes);
    RUN(test_over_long_csw);
    RUN(test_bad_signature);
    RUN(test_stale_tag);
    RUN(test_failed_status_keeps_residue);
    RUN(test_phase_error_status);
    RUN(test_null_and_zero);
    RUN(test_result_names_distinct);
    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}
