#ifndef AURALITE_DRIVERS_NVME_NVME_PROTO_H
#define AURALITE_DRIVERS_NVME_NVME_PROTO_H

#include <stdint.h>

/*
 * NVMe protocol encoding — the pure half of the driver (RESIDUE3 C7b).
 *
 * Everything here is arithmetic over bytes that a controller will read or
 * that a controller wrote: register field packing, the 64-byte submission
 * entry, the 16-byte completion entry, and the doorbell geometry.  It is kept
 * out of nvme.c for the same reason kernel/net/tcp_wscale.h is kept out of
 * tcp.c: the parts that can be wrong silently are the parts worth pinning on
 * the host, where a mistake costs a test run instead of a wedged queue.
 *
 * Register and structure layouts: NVM Express Base Specification 2.0,
 * figures "Offset 00h: Controller Capabilities", "Submission Queue Entry",
 * "Completion Queue Entry".  Byte offsets are named rather than assumed.
 *
 * NOTHING HERE TOUCHES HARDWARE.  nvme.c owns the MMIO, the DMA and the
 * polling; this header only says what the bytes mean.
 */

/* ---- Controller registers (BAR0 offsets) ----------------------------- */
#define NVME_REG_CAP    0x00u   /* 64-bit controller capabilities */
#define NVME_REG_VS     0x08u   /* 32-bit version */
#define NVME_REG_INTMS  0x0Cu
#define NVME_REG_INTMC  0x10u
#define NVME_REG_CC     0x14u   /* 32-bit controller configuration */
#define NVME_REG_CSTS   0x1Cu   /* 32-bit controller status */
#define NVME_REG_AQA    0x24u   /* 32-bit admin queue attributes */
#define NVME_REG_ASQ    0x28u   /* 64-bit admin submission queue base */
#define NVME_REG_ACQ    0x30u   /* 64-bit admin completion queue base */

/* CC bits (NVMe 2.0 figure 40). */
#define NVME_CC_EN       (1u << 0)
#define NVME_CC_CSS_NVM  (0u << 4)     /* command set: NVM */
#define NVME_CC_SHN_NONE (0u << 14)
#define NVME_CC_IOSQES_SHIFT 16u       /* 2^n bytes per SQ entry */
#define NVME_CC_IOCQES_SHIFT 20u
#define NVME_SQ_ENTRY_SHIFT  6u        /* 64-byte entries => n = 6 */
#define NVME_CQ_ENTRY_SHIFT  4u        /* 16-byte entries => n = 4 */

/* CSTS bits. */
#define NVME_CSTS_RDY   (1u << 0)
#define NVME_CSTS_CFS   (1u << 1)      /* controller fatal status */

/* CAP fields (NVMe 2.0 figure 39). */
#define NVME_CAP_MQES_SHIFT   0u
#define NVME_CAP_CQR_SHIFT    16u
#define NVME_CAP_TO_SHIFT     24u
#define NVME_CAP_DSTRD_SHIFT  32u
#define NVME_CAP_CSS_SHIFT    37u
#define NVME_CAP_MPSMIN_SHIFT 48u

/* ---- Admin / I/O opcodes --------------------------------------------- */
#define NVME_ADMIN_DELETE_SQ  0x00u
#define NVME_ADMIN_CREATE_SQ  0x01u
#define NVME_IO_WRITE         0x01u
#define NVME_IO_READ          0x02u
#define NVME_ADMIN_CREATE_CQ  0x05u
#define NVME_ADMIN_IDENTIFY   0x06u
#define NVME_ADMIN_SET_FEAT   0x09u
#define NVME_ADMIN_GET_FEAT   0x0Au

#define NVME_FEAT_NUM_QUEUES  0x07u
#define NVME_IDENTIFY_NSID_0  0x00u   /* CNS 0: namespace */
#define NVME_IDENTIFY_CTRL    0x01u   /* CNS 1: controller */

/* Completion Queue Entry, DW3 — and this is the field this driver got wrong
 * the first time: DW3 is NOT "reserved then status".  Its LOW half is the
 * command identifier the submission carried, and its HIGH half is the status
 * (NVMe 2.0 figure 96).  Reading the status out of the low half makes a
 * perfectly good completion look like a phase-0 error, and — worse — makes
 * some genuine errors look like successes, because the CID's low bit reads
 * as a phase tag.  Measured once, in this driver, at C7b. */
#define NVME_CQE_CID_MASK    0x0000FFFFu
#define NVME_CQE_STATUS_SHIFT 16u
#define NVME_CQE_PHASE         (1u << 0)   /* inside the status half */
#define NVME_CQE_STATUS_MASK   0xFFFEu     /* SC, bits 15:1 of the status half */
#define NVME_CQE_SCT_SHIFT     9u
#define NVME_CQE_SC_SHIFT      1u
#define NVME_CQE_DNR_SHIFT     15u

/* A completed command that the controller rejected carries a non-zero
 * status code; a good completion is 0. */
typedef struct {
    uint8_t  phase;        /* the phase bit the controller wrote */
    uint8_t  status_code;  /* SC, bits 8:1 */
    uint8_t  status_type;  /* SCT, bits 11:9: 0 generic, 1 command, 2 media */
    uint8_t  do_not_retry;
} nvme_status_t;

/* The command identifier a completion answers. */
static inline uint16_t nvme_cqe_cid(uint32_t cdw3) {
    return (uint16_t)(cdw3 & NVME_CQE_CID_MASK);
}

/* The 16-bit status half of DW3. */
static inline uint16_t nvme_cqe_status16(uint32_t cdw3) {
    return (uint16_t)(cdw3 >> NVME_CQE_STATUS_SHIFT);
}

static inline nvme_status_t nvme_cqe_status(uint32_t cdw3) {
    uint16_t sw = nvme_cqe_status16(cdw3);
    nvme_status_t st;
    st.phase        = (uint8_t)(sw & NVME_CQE_PHASE);
    st.status_code  = (uint8_t)((sw & NVME_CQE_STATUS_MASK) >> NVME_CQE_SC_SHIFT);
    st.status_type  = (uint8_t)((sw >> NVME_CQE_SCT_SHIFT) & 0x7u);
    st.do_not_retry = (uint8_t)((sw >> NVME_CQE_DNR_SHIFT) & 1u);
    return st;
}

/* An all-ones completion entry is what unwritten queue memory reads as; its
 * phase bit is 1 while the driver's expectation may also be 1, so "phase
 * matches" alone can accept garbage.  A CQE also has to carry a plausible
 * SQ head pointer, which is exactly what makes this check possible. */
static inline int nvme_cqe_plausible(uint32_t cdw2, uint32_t cdw3,
                                     uint8_t expected_phase,
                                     uint16_t expected_cid) {
    nvme_status_t st = nvme_cqe_status(cdw3);
    if (st.phase != expected_phase) return 0;
    /* The CID is the strongest check available: unwritten queue memory has
     * CID 0xFFFF and phase 1, which "phase matches" alone would accept. */
    if (nvme_cqe_cid(cdw3) != expected_cid) return 0;
    uint16_t sqid = (uint16_t)(cdw2 >> 16);
    if (sqid == 0xFFFFu) return 0;
    return 1;
}

/* ---- Register packing ------------------------------------------------ */

static inline uint32_t nvme_cc_value(uint32_t enable) {
    return (enable ? NVME_CC_EN : 0u)
         | NVME_CC_CSS_NVM
         | NVME_CC_SHN_NONE
         | (NVME_SQ_ENTRY_SHIFT << NVME_CC_IOSQES_SHIFT)
         | (NVME_CQ_ENTRY_SHIFT << NVME_CC_IOCQES_SHIFT);
}

/* AQA: ASQS/ACQS are 0-based sizes, 12 and 16 bits wide respectively. */
static inline uint32_t nvme_aqa_value(uint32_t sq_entries, uint32_t cq_entries) {
    uint32_t sqs = (sq_entries > 0) ? (sq_entries - 1u) : 0u;
    uint32_t cqs = (cq_entries > 0) ? (cq_entries - 1u) : 0u;
    if (sqs > 0xFFFu) sqs = 0xFFFu;
    if (cqs > 0xFFFu) cqs = 0xFFFu;
    return (sqs & 0xFFFu) | ((cqs & 0xFFFu) << 16);
}

static inline uint32_t nvme_cap_mqes(uint64_t cap) {
    return (uint32_t)((cap >> NVME_CAP_MQES_SHIFT) & 0xFFFFu);
}
static inline uint32_t nvme_cap_dstrd(uint64_t cap) {
    return (uint32_t)((cap >> NVME_CAP_DSTRD_SHIFT) & 0xFu);
}
static inline uint32_t nvme_cap_mpsmin(uint64_t cap) {
    return (uint32_t)((cap >> NVME_CAP_MPSMIN_SHIFT) & 0xFu);
}

/* Doorbell offsets: SQ0 tail doorbell is at 0x1000; each queue pair owns two
 * consecutive doorbells, separated by 4 << DSTRD bytes. */
static inline uint32_t nvme_doorbell_sq(uint32_t dstrd, uint32_t qid) {
    return 0x1000u + (qid * 2u) * (4u << dstrd);
}
static inline uint32_t nvme_doorbell_cq(uint32_t dstrd, uint32_t qid) {
    return 0x1000u + (qid * 2u + 1u) * (4u << dstrd);
}

/* ---- Completion-queue bookkeeping ------------------------------------ */

/* Head pointer advance with wraparound: the CQ is a ring of `entries`. */
static inline uint32_t nvme_ring_next(uint32_t idx, uint32_t entries) {
    uint32_t n = idx + 1u;
    return (n >= entries) ? 0u : n;
}

/* ---- Submission-queue entries ---------------------------------------- */

/* CDW0: opcode in 7:0, fuse in 9:8, command id in 31:16. */
static inline uint32_t nvme_cdw0(uint32_t opcode, uint32_t cid) {
    return (opcode & 0xFFu) | ((cid & 0xFFFFu) << 16);
}

/* CDW12 for a read/write: NLB is the number of logical blocks MINUS ONE,
 * bits 15:0.  A caller that passes 0 has asked for nothing at all. */
static inline uint32_t nvme_cdw12_nlb(uint32_t blocks) {
    if (blocks == 0) return 0;
    uint32_t nlb = blocks - 1u;
    return (nlb > 0xFFFFu) ? 0xFFFFu : nlb;
}

/* Create I/O Completion Queue, CDW10/CDW11 (NVMe 2.0 figure 114). */
static inline uint32_t nvme_create_cq_cdw10(uint32_t qid, uint32_t entries) {
    uint32_t qs = (entries > 0) ? (entries - 1u) : 0u;
    return (qid & 0xFFFFu) | ((qs & 0xFFFFu) << 16);
}
static inline uint32_t nvme_create_cq_cdw11(int physically_contiguous,
                                            uint32_t vector) {
    return (physically_contiguous ? 1u : 0u) | ((vector & 0xFFFFu) << 16);
}

/* Create I/O Submission Queue, CDW10/CDW11 (NVMe 2.0 figure 116). */
static inline uint32_t nvme_create_sq_cdw10(uint32_t qid, uint32_t entries) {
    uint32_t qs = (entries > 0) ? (entries - 1u) : 0u;
    return (qid & 0xFFFFu) | ((qs & 0xFFFFu) << 16);
}
static inline uint32_t nvme_create_sq_cdw11(int physically_contiguous,
                                            uint32_t cqid, uint32_t priority) {
    return (physically_contiguous ? 1u : 0u)
         | ((priority & 0x3u) << 1)
         | ((cqid & 0xFFFFu) << 16);
}

/* Set Features (Number of Queues): NSQR in 15:0, NCQR in 31:16, both 0-based.
 * The controller answers with what it actually allocated. */
static inline uint32_t nvme_setfeat_numq(uint32_t sq_wanted, uint32_t cq_wanted) {
    uint32_t nsqr = (sq_wanted > 0) ? (sq_wanted - 1u) : 0u;
    uint32_t ncqr = (cq_wanted > 0) ? (cq_wanted - 1u) : 0u;
    return (nsqr & 0xFFFFu) | ((ncqr & 0xFFFFu) << 16);
}
static inline uint32_t nvme_numq_sq_granted(uint32_t dw0) {
    return (dw0 & 0xFFFFu) + 1u;
}
static inline uint32_t nvme_numq_cq_granted(uint32_t dw0) {
    return ((dw0 >> 16) & 0xFFFFu) + 1u;
}

/* Identify: CDW10 carries CNS (7:0); CDW11 carries the namespace id. */
static inline uint32_t nvme_identify_cdw10(uint32_t cns) { return cns & 0xFFu; }

/* ---- Identify payload decoding --------------------------------------- */

/* Byte offsets inside the 4096-byte Identify Controller / Namespace data. */
#define NVME_ID_CTRL_SN   4u     /* 20-byte serial number, space padded */
#define NVME_ID_CTRL_MN   24u    /* 40-byte model number */
#define NVME_ID_CTRL_FR   64u    /* 8-byte firmware revision */
#define NVME_ID_NS_SIZE   0u     /* NSZE, 64-bit, in logical blocks */
#define NVME_ID_NS_CAP    8u     /* NCAP, 64-bit */
#define NVME_ID_NS_FLBAS  26u    /* bits 3:0: LBA format index */
#define NVME_ID_NS_LBAF   128u   /* 16 LBA formats x 4 bytes */
#define NVME_ID_NS_NOFLBAS 0x4u  /* FLBAS bit 2: metadata at end of LBA */

/* Copy a space-padded ASCII field out of an identify payload, trimming the
 * trailing spaces the spec requires and refusing to emit control bytes. */
static inline int nvme_id_string(const uint8_t *data, uint32_t off,
                                 uint32_t len, char *out, uint32_t out_max) {
    if (!data || !out || out_max == 0) return -1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n + 1u < out_max; i++) {
        uint8_t c = data[off + i];
        if (c < 0x20u || c > 0x7Eu) c = (uint8_t)' ';
        out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1u] == ' ') n--;
    out[n] = '\0';
    return (int)n;
}

/* LBA format decode.  `lbaf` is the 32-bit entry; MS(15:0), LBADS(23:16),
 * RP(25:24).  Returns the LBA size in bytes, or 0 for "not a shape this
 * driver can address" — the caller must say so out loud rather than divide
 * by it. */
static inline uint32_t nvme_lbaf_bytes(uint32_t lbaf) {
    uint32_t lbads = (lbaf >> 16) & 0xFFu;
    if (lbads == 0 || lbads > 16) return 0;     /* spec allows 9..16 */
    return 1u << lbads;
}

/* The namespace's active LBA format index from FLBAS, plus the LBA size that
 * index names. */
static inline uint32_t nvme_flbas_index(const uint8_t *ns_data) {
    return (uint32_t)(ns_data[NVME_ID_NS_FLBAS] & 0x0Fu);
}
static inline uint32_t nvme_ns_lba_bytes(const uint8_t *ns_data) {
    uint32_t idx = nvme_flbas_index(ns_data);
    if (idx > 15u) return 0;
    uint32_t lbaf = 0;
    const uint8_t *p = ns_data + NVME_ID_NS_LBAF + idx * 4u;
    lbaf = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return nvme_lbaf_bytes(lbaf);
}

/* Little-endian 64-bit field decode.  Built from two 32-bit halves so the
 * portable header needs no widening casts (this tree ratchets those, and a
 * byte-by-byte OR of eight terms is not more readable than four). */
static inline uint64_t nvme_le64(const uint8_t *p) {
    uint32_t lo = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    uint64_t v = hi;         /* implicit widening: no cast token needed */
    v <<= 32;
    v |= lo;
    return v;
}

#endif /* AURALITE_DRIVERS_NVME_NVME_PROTO_H */
