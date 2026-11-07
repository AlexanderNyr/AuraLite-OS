/* test_nvme_proto.c — host pin for drivers/nvme/nvme_proto.h (RESIDUE3 C7b).
 *
 * The NVMe driver's silent-death modes all live in the encoding: a CDW12 that
 * says "zero blocks" for a one-block read, a doorbell computed with the wrong
 * stride, an AQA that forgets its fields are 0-based, a completion whose
 * phase bit is accepted without checking the entry is not unwritten queue
 * memory.  None of those produce a crash; they produce a queue that never
 * completes.  That is why they are pinned here.
 *
 * Build: cc -std=c11 -Wall -Wextra -Werror -I . tests/unit/test_nvme_proto.c
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "drivers/nvme/nvme_proto.h"

static int checks = 0, fails = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fails++;                                                       \
            printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                    \
        }                                                                  \
    } while (0)

static void t_cc_aqa(void) {
    printf("registers: CC and AQA packing\n");
    uint32_t cc = nvme_cc_value(1);
    CHECK((cc & NVME_CC_EN) != 0, "CC.EN set when enabling");
    CHECK(nvme_cc_value(0) == (cc & ~NVME_CC_EN), "CC.EN clear when disabling");
    CHECK(((cc >> NVME_CC_IOSQES_SHIFT) & 0xFu) == NVME_SQ_ENTRY_SHIFT,
          "CC.IOSQES = 6 (64-byte entries)");
    CHECK(((cc >> NVME_CC_IOCQES_SHIFT) & 0xFu) == NVME_CQ_ENTRY_SHIFT,
          "CC.IOCQES = 4 (16-byte entries)");
    CHECK(((cc >> 4) & 0x7u) == 0u, "CC.CSS = NVM command set");

    /* AQA is 0-based in BOTH fields -- the classic off-by-one is a queue of
     * 8 entries declared as 8, which asks for 9. */
    uint32_t aqa = nvme_aqa_value(8, 8);
    CHECK((aqa & 0xFFFu) == 7u, "AQA.ASQS = entries-1");
    CHECK(((aqa >> 16) & 0xFFFu) == 7u, "AQA.ACQS = entries-1");
    CHECK(nvme_aqa_value(1, 1) == 0u, "a one-entry queue is 0");
    /* Wider than the field: saturate, never truncate into a smaller queue. */
    CHECK((nvme_aqa_value(5000, 5000) & 0xFFFu) == 0xFFFu, "ASQS saturates");
    CHECK((nvme_aqa_value(0, 0) & 0xFFFu) == 0u, "a zero size does not wrap");
}

static void t_cap(void) {
    printf("registers: CAP decode\n");
    /* A plausible QEMU CAP: MQES=0x3FF, DSTRD=0, MPSMIN=0, CSS=1 (NVM). */
    uint64_t cap = (uint64_t)0x3FFu
                 | ((uint64_t)1u << NVME_CAP_CSS_SHIFT)
                 | ((uint64_t)0u << NVME_CAP_DSTRD_SHIFT)
                 | ((uint64_t)0u << NVME_CAP_MPSMIN_SHIFT);
    CHECK(nvme_cap_mqes(cap) == 0x3FFu, "MQES");
    CHECK(nvme_cap_dstrd(cap) == 0u, "DSTRD");
    CHECK(nvme_cap_mpsmin(cap) == 0u, "MPSMIN = 4 KiB pages");

    uint64_t cap2 = cap | ((uint64_t)2u << NVME_CAP_DSTRD_SHIFT);
    CHECK(nvme_cap_dstrd(cap2) == 2u, "DSTRD 2 decoded");
}

static void t_doorbells(void) {
    printf("registers: doorbell geometry\n");
    /* With stride 0, queue 0's SQ tail is 0x1000 and its CQ head 0x1004. */
    CHECK(nvme_doorbell_sq(0, 0) == 0x1000u, "SQ0TDBL");
    CHECK(nvme_doorbell_cq(0, 0) == 0x1004u, "CQ0HDBL");
    CHECK(nvme_doorbell_sq(0, 1) == 0x1008u, "SQ1TDBL");
    CHECK(nvme_doorbell_cq(0, 1) == 0x100Cu, "CQ1HDBL");
    /* DSTRD=2 => 16-byte stride: queue 1's SQ tail is 0x1000 + 2*16. */
    CHECK(nvme_doorbell_sq(2, 1) == 0x1020u, "SQ1TDBL with DSTRD=2");
    CHECK(nvme_doorbell_cq(2, 1) == 0x1030u, "CQ1HDBL with DSTRD=2");
    /* The two doorbells of a pair must never collide. */
    for (uint32_t d = 0; d < 4; d++)
        for (uint32_t q = 0; q < 4; q++)
            CHECK(nvme_doorbell_sq(d, q) != nvme_doorbell_cq(d, q),
                  "doorbells distinct (dstrd=%u qid=%u)", d, q);
}

static void t_status(void) {
    printf("completions: status word and plausibility\n");
    /* A real Success completion for CID 5: DW3 = CID(5) | status(0x0001)
     * << 16 — the status lives in the HIGH half. */
    nvme_status_t st = nvme_cqe_status(0x00010005u);
    CHECK(nvme_cqe_cid(0x00010005u) == 5, "CID comes from the low half");
    CHECK(st.phase == 1 && st.status_code == 0 && st.status_type == 0,
          "phase 1, status 0");
    /* The same words with the halves swapped is the bug this pin exists for:
     * CID 1 + status 0x0005 would read as success in the wrong layout. */
    CHECK(nvme_cqe_status(0x00050001u).status_code == 0x02,
          "swapped halves decode as an error, not as success");

    /* A real error: SC=0x02 (invalid field), SCT=0 (generic), phase 1.
     * CDW3 = phase | (SC << 1) | (SCT << 9). */
    uint32_t bad = (uint32_t)(1u | (0x02u << NVME_CQE_SC_SHIFT)) << NVME_CQE_STATUS_SHIFT;
    st = nvme_cqe_status(bad | 7u);
    CHECK(st.status_code == 0x02, "SC decoded");
    CHECK(st.status_type == 0, "SCT decoded");
    st = nvme_cqe_status(((uint32_t)(1u | (0x81u << NVME_CQE_SC_SHIFT)
                           | (2u << NVME_CQE_SCT_SHIFT)) << NVME_CQE_STATUS_SHIFT) | 3u);
    CHECK(st.status_code == 0x81 && st.status_type == 2, "media error decodes");
    st = nvme_cqe_status(((uint32_t)(1u | (1u << NVME_CQE_DNR_SHIFT))
                          << NVME_CQE_STATUS_SHIFT) | 4u);
    CHECK(st.do_not_retry == 1, "DNR bit decoded");

    /* The trap: unwritten queue memory is all ones, and its phase bit is 1 --
     * the same value a driver waiting for phase 1 expects. */
    uint32_t garbage_cdw2 = 0xFFFFFFFFu, garbage_cdw3 = 0xFFFFFFFFu;
    CHECK(nvme_cqe_plausible(garbage_cdw2, garbage_cdw3, 1, 9) == 0,
          "an all-ones entry is rejected even though its phase matches");
    /* A real entry: SQHD=0, SQID=1, CID=9, phase 1, status 0. */
    uint32_t good_cdw2 = 0u | (1u << 16);
    uint32_t good_cdw3 = (0x0001u << NVME_CQE_STATUS_SHIFT) | 9u;
    CHECK(nvme_cqe_plausible(good_cdw2, good_cdw3, 1, 9) == 1,
          "a real entry is accepted");
    CHECK(nvme_cqe_plausible(good_cdw2, good_cdw3, 0, 9) == 0,
          "wrong phase rejected");
    CHECK(nvme_cqe_plausible(good_cdw2, good_cdw3, 1, 10) == 0,
          "a completion for another command is rejected");
}

static void t_ring(void) {
    printf("queues: ring advance\n");
    CHECK(nvme_ring_next(0, 8) == 1, "0 -> 1");
    CHECK(nvme_ring_next(6, 8) == 7, "6 -> 7");
    CHECK(nvme_ring_next(7, 8) == 0, "7 wraps to 0");
    CHECK(nvme_ring_next(0, 1) == 0, "a one-entry ring stays at 0");
}

static void t_cdws(void) {
    printf("commands: CDW packing\n");
    CHECK(nvme_cdw0(NVME_IO_READ, 1) == (0x02u | (1u << 16)), "opcode + CID");
    /* NLB is blocks-1: one block must be 0, not 1.  Getting this backwards
     * reads 512 bytes too many and the next command's data is shifted. */
    CHECK(nvme_cdw12_nlb(1) == 0u, "one block -> NLB 0");
    CHECK(nvme_cdw12_nlb(8) == 7u, "eight blocks -> NLB 7");
    CHECK(nvme_cdw12_nlb(0) == 0u, "zero blocks does not wrap to 0xFFFF");
    CHECK(nvme_cdw12_nlb(70000) == 0xFFFFu, "NLB saturates at the field width");

    CHECK(nvme_create_cq_cdw10(1, 8) == (1u | (7u << 16)), "Create CQ qid/size");
    CHECK(nvme_create_cq_cdw11(1, 0) == 1u, "Create CQ: physically contiguous");
    CHECK(nvme_create_sq_cdw10(1, 8) == (1u | (7u << 16)), "Create SQ qid/size");
    uint32_t csq = nvme_create_sq_cdw11(1, 1, 0);
    CHECK((csq & 1u) == 1u, "Create SQ: physically contiguous");
    CHECK(((csq >> 16) & 0xFFFFu) == 1u, "Create SQ: bound to CQ 1");

    uint32_t nq = nvme_setfeat_numq(1, 1);
    CHECK(nq == 0u, "asking for one queue pair is 0/0");
    CHECK(nvme_numq_sq_granted(nq) == 1u && nvme_numq_cq_granted(nq) == 1u,
          "granted counts decode back");
    CHECK(nvme_numq_sq_granted(0xFFFF0000u) == 1u,
          "a controller granting 0 SQs decodes as 1 (0-based, as the spec says)");

    CHECK(nvme_identify_cdw10(NVME_IDENTIFY_CTRL) == 1u, "Identify CNS=01h");
    CHECK(nvme_identify_cdw10(NVME_IDENTIFY_NSID_0) == 0u, "Identify CNS=00h");
}

static void t_identify(void) {
    printf("identify: string and LBA-format decoding\n");
    uint8_t data[4096];
    memset(data, ' ', sizeof data);
    memcpy(data + NVME_ID_CTRL_SN, "AURA0001            ", 20);
    memcpy(data + NVME_ID_CTRL_MN, "QEMU NVMe Ctrl                          ", 40);
    memcpy(data + NVME_ID_CTRL_FR, "1.3     ", 8);

    char out[64];
    CHECK(nvme_id_string(data, NVME_ID_CTRL_SN, 20, out, sizeof out) == 8,
          "serial length after trimming");
    CHECK(strcmp(out, "AURA0001") == 0, "serial trimmed: '%s'", out);
    nvme_id_string(data, NVME_ID_CTRL_MN, 40, out, sizeof out);
    CHECK(strcmp(out, "QEMU NVMe Ctrl") == 0, "model trimmed: '%s'", out);

    /* Control bytes must not reach the console. */
    uint8_t junk[8] = { 'A', 0x00, 0x7F, 'B', ' ', ' ', ' ', ' ' };
    nvme_id_string(junk, 0, 8, out, sizeof out);
    CHECK(strcmp(out, "A  B") == 0, "non-printable bytes become spaces: '%s'", out);

    /* LBA format: LBADS 9 => 512 bytes, 12 => 4096. */
    CHECK(nvme_lbaf_bytes(9u << 16) == 512u, "LBADS 9 is 512 bytes");
    CHECK(nvme_lbaf_bytes(12u << 16) == 4096u, "LBADS 12 is 4096 bytes");
    CHECK(nvme_lbaf_bytes(0u) == 0u, "LBADS 0 is refused, not divided by");
    CHECK(nvme_lbaf_bytes(17u << 16) == 0u, "LBADS 17 is refused");

    /* A namespace whose active format is index 1, 4096-byte LBAs. */
    uint8_t ns[4096];
    memset(ns, 0, sizeof ns);
    ns[NVME_ID_NS_FLBAS] = 1;
    ns[NVME_ID_NS_LBAF + 0] = 0x00; ns[NVME_ID_NS_LBAF + 2] = 9;   /* 512 */
    ns[NVME_ID_NS_LBAF + 4] = 0x00; ns[NVME_ID_NS_LBAF + 6] = 12;  /* 4096 */
    CHECK(nvme_flbas_index(ns) == 1u, "FLBAS index");
    CHECK(nvme_ns_lba_bytes(ns) == 4096u, "4096-byte LBA resolved");
    ns[NVME_ID_NS_FLBAS] = 0;
    CHECK(nvme_ns_lba_bytes(ns) == 512u, "512-byte LBA resolved");

    /* NSZE, little endian 64-bit. */
    uint8_t size[8] = { 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    CHECK(nvme_le64(size) == 8192u, "NSZE = 8192 blocks");
}

int main(void) {
    printf("=== AuraLite OS NVMe protocol tests (RESIDUE3 C7b) ===\n\n");
    t_cc_aqa();
    t_cap();
    t_doorbells();
    t_status();
    t_ring();
    t_cdws();
    t_identify();
    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
