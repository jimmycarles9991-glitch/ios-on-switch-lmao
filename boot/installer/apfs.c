// SPDX-License-Identifier: GPL-2.0-only
// Layouts: Apple File System Reference (nx_superblock_t, checkpoint_map_phys_t, spaceman_phys_t, chunk_info_block_t,
// btree_node_phys_t). The sizes a container's metadata must have for its block count are newfs_apfs's and the
// kernel's, measured on macOS 27 (fresh containers from 4 GB to 1 TB, and containers it shrank):
//   internal pool blocks           = 3 x (chunks + chunk-info blocks)
//   internal-pool free queue limit = queue_limit(ceil(internal pool blocks / 1136))
//   main free queue limit          = queue_limit(min(512, ceil(blocks x m / (4 x 1136)))), m = 1 below 1 GiB, 2 below
//                                    4 GiB, 4 from 4 GiB
//   queue_limit(n)                 = 1 when n is 1, else at least 3 (a root over two leaves)
//   nx_max_file_systems            = min(100, ceil(bytes / 512 MiB))
// The kernel refuses a container whose free queue limits (sfq_tree_node_limit) are not exactly these for its size,
// though fsck_apfs passes it (diskutil: "Container ERROR -69808", no volume).
#include "apfs.h"

#include <stddef.h>
#include <string.h>

#define BS               SWITCH_APFS_BLOCK
#define NX_BLOCK_COUNT   40
#define NX_DESC_BLOCKS   104
#define NX_SPACEMAN_OID  152
#define NX_MAX_FS        180
#define NX_FS_OIDS       184
#define NX_MAX_VOLUMES   100
#define SM_DEV           48
#define SM_DEV_BYTES     48
#define SM_IP_BLOCKS     152
#define SM_FQ            200
#define SM_FQ_BYTES      40
#define SM_BITMAP_OFFSET 328
#define CI_BYTES         32
#define CI_COUNT_MASK    0xFFFFFu
#define CPM_ENTRIES      256
#define FQ_CHILDREN      256
#define IP_BLOCKS_PER    3u
#define FQ_PER_NODE      1136u
#define FQ_NODE_MAX      512u
#define FQ_LIMIT         24
#define MAX_FS_BYTES     (UINT64_C(512) << 20)

struct work {
    uint8_t zero[BS];
    uint8_t nx[BS];
    uint8_t sm[BS];
    uint8_t ip_bitmap[BS];
    uint8_t node[BS];
    uint8_t scratch[BS];
    uint8_t cib[SWITCH_APFS_EDIT_CIBS][BS];
    uint8_t bitmap[SWITCH_APFS_EDIT_BITMAPS][BS];
    uint64_t cib_addr[SWITCH_APFS_EDIT_CIBS];
    uint64_t bitmap_addr[SWITCH_APFS_EDIT_BITMAPS];
    uint64_t cpm_oid[CPM_ENTRIES];
    uint64_t cpm_addr[CPM_ENTRIES];
    uint64_t fq_children[FQ_CHILDREN];
    uint32_t cibs_edited;
    uint32_t bitmaps_edited;
    uint32_t cpm_count;
    uint64_t nx_addr;
    uint64_t sm_addr;
    uint64_t ip_bitmap_addr;
    uint64_t blocks;
    uint64_t free;
    uint64_t ip_base;
    uint64_t ip_blocks;
    uint64_t areas_end;
    uint64_t main_queue_end;
    uint64_t ip_queue_end;
    uint32_t main_queue_nodes;
    uint32_t ip_queue_nodes;
    uint32_t bpc;
    uint32_t cpc;
    uint32_t cibs;
    uint32_t addr_off;
};

_Static_assert(sizeof(struct work) <= SWITCH_APFS_WORK_BYTES, "SWITCH_APFS_WORK_BYTES holds the work area");

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t max64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

uint64_t switch_apfs_checksum(const uint8_t *block, uint32_t bytes)
{
    const uint64_t mod = 0xFFFFFFFFu;
    uint64_t lo = 0;
    uint64_t hi = 0;
    for (uint32_t at = 8; at < bytes; at += 4) {
        lo = (lo + le32(block + at)) % mod;
        hi = (hi + lo) % mod;
    }
    uint64_t c1 = mod - (lo + hi) % mod;
    uint64_t c2 = mod - (lo + c1) % mod;
    return c2 << 32 | c1;
}

static int object_ok(const uint8_t *block, uint32_t type)
{
    return (le32(block + 24) & 0xFFFFu) == type && switch_apfs_checksum(block, BS) == le64(block);
}

static void seal(uint8_t *block)
{
    put64(block, switch_apfs_checksum(block, BS));
}

static uint64_t div_up(uint64_t n, uint64_t d)
{
    return (n + d - 1) / d;
}

static int bit(const uint8_t *bitmap, uint64_t i)
{
    return (int)((uint32_t)bitmap[i / 8] >> (i % 8) & 1u);
}

static void clear_bit(uint8_t *bitmap, uint64_t i)
{
    bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
}

// One past the highest set bit of the first `bits`, 0 when none is set.
static uint32_t highest_bit(const uint8_t *bitmap, uint32_t bits)
{
    for (uint32_t i = bits; i; i--) {
        if (bit(bitmap, i - 1)) {
            return i;
        }
    }
    return 0;
}

static uint8_t *chunk_info(uint8_t *cib, uint32_t index)
{
    return cib + 40 + (size_t)index * CI_BYTES;
}

static uint64_t ephemeral(const struct work *w, uint64_t oid)
{
    for (uint32_t i = 0; i < w->cpm_count; i++) {
        if (w->cpm_oid[i] == oid) {
            return w->cpm_addr[i];
        }
    }
    return 0;
}

static int read_map(const struct switch_apfs_io *io, struct work *w, uint64_t block)
{
    if (io->read(io->context, block, w->node)) {
        return 1;
    }
    if (!object_ok(w->node, 0xC)) {
        return 0;
    }
    uint32_t count = le32(w->node + 36);
    for (uint32_t k = 0; k < count && 40 + ((size_t)k + 1) * 40 <= BS; k++) {
        const uint8_t *e = w->node + 40 + (size_t)k * 40;
        if (w->cpm_count == CPM_ENTRIES || le32(e + 8) != BS) {
            return 1;
        }
        w->cpm_oid[w->cpm_count] = le64(e + 24);
        w->cpm_addr[w->cpm_count] = le64(e + 32);
        w->cpm_count++;
    }
    return 0;
}

static int latest_checkpoint(const struct switch_apfs_io *io, struct work *w)
{
    if (io->read(io->context, 0, w->zero) || memcmp(w->zero + 32, "NXSB", 4) || !object_ok(w->zero, 1) ||
        le32(w->zero + 36) != BS) {
        return 1;
    }
    uint32_t desc_blocks = le32(w->zero + NX_DESC_BLOCKS);
    uint32_t data_blocks = le32(w->zero + NX_DESC_BLOCKS + 4);
    uint64_t desc_base = le64(w->zero + 112);
    uint64_t data_base = le64(w->zero + 120);
    uint64_t best = 0;
    if ((desc_blocks | data_blocks) & 0x80000000u) {
        return 1;
    }
    for (uint32_t i = 0; i < desc_blocks; i++) {
        if (io->read(io->context, desc_base + i, w->node)) {
            return 1;
        }
        if (!memcmp(w->node + 32, "NXSB", 4) && object_ok(w->node, 1) && le64(w->node + 16) >= best) {
            best = le64(w->node + 16);
            w->nx_addr = desc_base + i;
            memcpy(w->nx, w->node, BS);
        }
    }
    if (!best) {
        return 1;
    }
    uint32_t index = le32(w->nx + 136);
    uint32_t length = le32(w->nx + 140);
    w->cpm_count = 0;
    for (uint32_t j = 0; j < length; j++) {
        if (read_map(io, w, desc_base + (index + j) % desc_blocks)) {
            return 1;
        }
    }
    w->areas_end = max64(desc_base + desc_blocks, data_base + data_blocks);
    return 0;
}

// The entries of a free queue node in `node`: for a leaf, each freed extent (paddr + length), none of which may
// overlap [lo, hi), widening *end; for an index node, each child's oid into `children`.
static int queue_node(const uint8_t *node, uint64_t lo, uint64_t hi, uint64_t *end, uint64_t *children)
{
    uint16_t flags = le16(node + 32);
    uint32_t toc = 56u + le16(node + 40);
    uint32_t keys = toc + le16(node + 42);
    uint32_t values = BS - (flags & 1 ? 40u : 0u);
    uint32_t count = le32(node + 36);
    uint32_t entry = flags & 4 ? 4 : 8;
    if (!(flags & 2) && count > FQ_CHILDREN) {
        return 1;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *t = node + toc + (size_t)i * entry;
        uint32_t k = le16(t);
        uint32_t v = le16(t + (flags & 4 ? 2 : 4));
        if (keys + k + 16 > BS || (v != 0xFFFF && (v < 8 || v > values)) || (!(flags & 2) && v == 0xFFFF)) {
            return 1;
        }
        if (!(flags & 2)) {
            children[i] = le64(node + values - v);
            continue;
        }
        uint64_t paddr = le64(node + keys + k + 8);
        uint64_t length = v == 0xFFFF ? 1 : le64(node + values - v);
        if (paddr < hi && lo < paddr + length) {
            return 1;
        }
        *end = max64(*end, paddr + length);
    }
    return 0;
}

// The largest end (paddr + length) of a free queue's entries, none of which may overlap [lo, hi). The queue is one
// leaf, or a root index node over leaves once APFS splits it: many frees waiting for their checkpoint to retire.
static int queue_end(const struct switch_apfs_io *io, struct work *w, uint32_t q, uint64_t lo, uint64_t hi,
                     uint64_t *end, uint32_t *nodes)
{
    *end = 0;
    *nodes = 0;
    const uint8_t *fq = w->sm + SM_FQ + (size_t)q * SM_FQ_BYTES;
    if (!le64(fq)) {
        return 0;
    }
    uint64_t addr = ephemeral(w, le64(fq + 8));
    if (!addr || io->read(io->context, addr, w->scratch) || !object_ok(w->scratch, 2) || le16(w->scratch + 34) > 1) {
        return 1;
    }
    uint32_t level = le16(w->scratch + 34);
    if (level != !(le16(w->scratch + 32) & 2) || queue_node(w->scratch, lo, hi, end, w->fq_children)) {
        return 1;
    }
    *nodes = 1 + (level ? le32(w->scratch + 36) : 0);
    for (uint32_t i = 0; level && i < le32(w->scratch + 36); i++) {
        addr = ephemeral(w, w->fq_children[i]);
        if (!addr || io->read(io->context, addr, w->node) || !object_ok(w->node, 3) || le16(w->node + 34) ||
            !(le16(w->node + 32) & 2) || queue_node(w->node, lo, hi, end, NULL)) {
            return 1;
        }
    }
    return 0;
}

static int spaceman_ok(const struct work *w)
{
    const uint8_t *sm = w->sm;
    const uint8_t *dev = sm + SM_DEV;
    uint32_t bitmap_offset = le32(sm + SM_BITMAP_OFFSET);
    uint64_t chunks = le64(dev + 8);
    return le32(sm + 32) == BS && w->bpc == (uint32_t)BS * 8 && w->cpc && le64(dev) == w->blocks &&
           chunks == div_up(w->blocks, w->bpc) && w->cibs == div_up(chunks, w->cpc) && !le32(dev + 20) &&
           !le64(dev + SM_DEV_BYTES) && !le32(dev + SM_DEV_BYTES + 16) &&
           le32(dev + SM_DEV_BYTES + 32) == w->addr_off + 8 * w->cibs && w->addr_off + (uint64_t)8 * w->cibs <= BS &&
           le32(sm + 160) == 1 && bitmap_offset + 2 <= BS && le16(sm + bitmap_offset) < le32(sm + 164) &&
           w->ip_blocks == IP_BLOCKS_PER * (chunks + w->cibs) && w->ip_blocks <= (uint64_t)BS * 8 &&
           !le64(sm + SM_FQ + (size_t)2 * SM_FQ_BYTES);
}

static int load(const struct switch_apfs_io *io, struct work *w)
{
    if (latest_checkpoint(io, w)) {
        return 1;
    }
    w->sm_addr = ephemeral(w, le64(w->nx + NX_SPACEMAN_OID));
    if (!w->sm_addr || io->read(io->context, w->sm_addr, w->sm) || !object_ok(w->sm, 5)) {
        return 1;
    }
    const uint8_t *dev = w->sm + SM_DEV;
    w->blocks = le64(w->nx + NX_BLOCK_COUNT);
    w->bpc = le32(w->sm + 36);
    w->cpc = le32(w->sm + 40);
    w->cibs = le32(dev + 16);
    w->free = le64(dev + 24);
    w->addr_off = le32(dev + 32);
    w->ip_blocks = le64(w->sm + SM_IP_BLOCKS);
    w->ip_base = le64(w->sm + 176);
    if (!spaceman_ok(w)) {
        return 1;
    }
    uint64_t ip_bm_base = le64(w->sm + 168);
    w->ip_bitmap_addr = ip_bm_base + le16(w->sm + le32(w->sm + SM_BITMAP_OFFSET));
    if (io->read(io->context, w->ip_bitmap_addr, w->ip_bitmap)) {
        return 1;
    }
    w->areas_end = max64(w->areas_end, max64(ip_bm_base + le32(w->sm + 164), w->ip_base + w->ip_blocks));
    return queue_end(io, w, 0, 0, 0, &w->ip_queue_end, &w->ip_queue_nodes) ||
           queue_end(io, w, 1, 0, 0, &w->main_queue_end, &w->main_queue_nodes);
}

static uint64_t cib_addr(const struct work *w, uint32_t index)
{
    return le64(w->sm + w->addr_off + (size_t)8 * index);
}

static int read_cib(const struct switch_apfs_io *io, const struct work *w, uint32_t index, uint8_t *block)
{
    return io->read(io->context, cib_addr(w, index), block) || !object_ok(block, 7) || le32(block + 32) != index ||
           le32(block + 36) > w->cpc || 40 + (size_t)le32(block + 36) * CI_BYTES > BS;
}

static uint64_t new_ip_blocks(const struct work *w, uint64_t chunks)
{
    return IP_BLOCKS_PER * (chunks + div_up(chunks, w->cpc));
}

static uint16_t queue_limit(uint64_t nodes)
{
    return (uint16_t)(nodes <= 1 ? 1 : max64(nodes, 3));
}

uint16_t switch_apfs_pool_queue_limit(uint64_t pool_blocks)
{
    return queue_limit(div_up(pool_blocks, FQ_PER_NODE));
}

uint16_t switch_apfs_main_queue_limit(uint64_t blocks)
{
    uint64_t m = 4;
    if (blocks < (UINT64_C(1) << 18)) {
        m = 1;
    } else if (blocks < (UINT64_C(1) << 20)) {
        m = 2;
    }
    uint64_t n = div_up(blocks * m, UINT64_C(4) * FQ_PER_NODE);
    return queue_limit(n < FQ_NODE_MAX ? n : FQ_NODE_MAX);
}

// The fewest blocks from `from` whose queue limits hold the queues as they are: the limits grow with the size.
static uint64_t queues_fit(const struct work *w, uint64_t from)
{
    uint64_t lo = from;
    uint64_t hi = w->blocks;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (switch_apfs_main_queue_limit(mid) >= w->main_queue_nodes &&
            switch_apfs_pool_queue_limit(new_ip_blocks(w, div_up(mid, w->bpc))) >= w->ip_queue_nodes) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

// One past the last block the volumes or the container use, from the chunk bitmaps: the last chunk that has one
// stays, and everything after it must be free.
static int used_end(const struct switch_apfs_io *io, struct work *w, uint64_t *end)
{
    for (uint32_t c = w->cibs; c--;) {
        if (read_cib(io, w, c, w->node)) {
            return 1;
        }
        for (uint32_t i = le32(w->node + 36); i--;) {
            const uint8_t *ci = chunk_info(w->node, i);
            uint64_t addr = le64(ci + 8);
            uint64_t bitmap = le64(ci + 24);
            uint32_t count = le32(ci + 16) & CI_COUNT_MASK;
            if (!bitmap && le32(ci + 20) != count) {
                return 1;
            }
            if (bitmap) {
                if (io->read(io->context, bitmap, w->scratch)) {
                    return 1;
                }
                *end = max64(addr + 1, addr + highest_bit(w->scratch, count));
                return 0;
            }
        }
    }
    *end = 0;
    return 0;
}

int switch_apfs_measure(const struct switch_apfs_io *io, void *work, struct switch_apfs_layout *layout)
{
    struct work *w = work;
    uint64_t used = 0;
    if (load(io, w) || used_end(io, w, &used)) {
        return 1;
    }
    uint64_t pool_used = max64(highest_bit(w->ip_bitmap, (uint32_t)w->ip_blocks),
                               w->ip_queue_end > w->ip_base ? w->ip_queue_end - w->ip_base : 0);
    uint64_t chunks = 1;
    while (new_ip_blocks(w, chunks) < pool_used) {
        chunks++;
    }
    layout->minimum = max64(max64(w->areas_end, w->main_queue_end), max64(used, (chunks - 1) * w->bpc + 1));
    layout->minimum = queues_fit(w, layout->minimum);
    layout->blocks = w->blocks;
    layout->pool_base = w->ip_base;
    layout->pool_blocks = w->ip_blocks;
    return layout->minimum > w->blocks;
}

static uint8_t *edited_cib(const struct switch_apfs_io *io, struct work *w, uint32_t index)
{
    for (uint32_t i = 0; i < w->cibs_edited; i++) {
        if (w->cib_addr[i] == cib_addr(w, index)) {
            return w->cib[i];
        }
    }
    if (w->cibs_edited == SWITCH_APFS_EDIT_CIBS || read_cib(io, w, index, w->cib[w->cibs_edited])) {
        return NULL;
    }
    w->cib_addr[w->cibs_edited] = cib_addr(w, index);
    return w->cib[w->cibs_edited++];
}

static uint8_t *edited_bitmap(const struct switch_apfs_io *io, struct work *w, uint64_t addr)
{
    for (uint32_t i = 0; i < w->bitmaps_edited; i++) {
        if (w->bitmap_addr[i] == addr) {
            return w->bitmap[i];
        }
    }
    if (w->bitmaps_edited == SWITCH_APFS_EDIT_BITMAPS || io->read(io->context, addr, w->bitmap[w->bitmaps_edited])) {
        return NULL;
    }
    w->bitmap_addr[w->bitmaps_edited] = addr;
    return w->bitmap[w->bitmaps_edited++];
}

// The chunk `blocks` cuts keeps its first part; what goes must be free. Returns the free blocks that go, or
// UINT64_MAX.
static uint64_t cut_chunk(const struct switch_apfs_io *io, struct work *w, uint8_t *ci, uint64_t blocks)
{
    uint64_t addr = le64(ci + 8);
    uint64_t bitmap = le64(ci + 24);
    uint32_t total = le32(ci + 16) & CI_COUNT_MASK;
    uint32_t free = le32(ci + 20);
    uint32_t cut = (uint32_t)(blocks - addr);
    if (bitmap) {
        if (io->read(io->context, bitmap, w->scratch) || highest_bit(w->scratch, total) > cut) {
            return UINT64_MAX;
        }
    } else if (free != total) {
        return UINT64_MAX;
    }
    put32(ci + 16, (le32(ci + 16) & ~CI_COUNT_MASK) | cut);
    put32(ci + 20, free - (total - cut));
    return total - cut;
}

// A chunk past the cut goes whole: it must be free and own no bitmap. Returns its blocks, or UINT64_MAX.
static uint64_t drop_chunk(uint8_t *ci)
{
    uint32_t total = le32(ci + 16) & CI_COUNT_MASK;
    if (le64(ci + 24) || le32(ci + 20) != total) {
        return UINT64_MAX;
    }
    memset(ci, 0, CI_BYTES);
    return total;
}

// The chunks past `blocks` go and the last kept one is cut at it. Returns the free blocks that go, or UINT64_MAX.
static uint64_t cut_chunks(const struct switch_apfs_io *io, struct work *w, uint64_t blocks, uint64_t chunks)
{
    uint64_t gone = 0;
    uint32_t last = (uint32_t)((chunks - 1) / w->cpc);
    for (uint32_t c = last; c < w->cibs; c++) {
        uint8_t *cib = c == last ? edited_cib(io, w, c) : w->node;
        if (!cib || (c != last && read_cib(io, w, c, cib))) {
            return UINT64_MAX;
        }
        uint32_t count = le32(cib + 36);
        uint32_t keep = c == last ? (uint32_t)(chunks - (uint64_t)c * w->cpc) : 0;
        uint32_t first = keep ? keep - 1 : 0;
        for (uint32_t i = first; i < count; i++) {
            uint64_t n = i + 1 == keep ? cut_chunk(io, w, chunk_info(cib, i), blocks) : drop_chunk(chunk_info(cib, i));
            if (n == UINT64_MAX) {
                return UINT64_MAX;
            }
            gone += n;
        }
        if (c == last) {
            put32(cib + 36, keep);
        }
    }
    return gone;
}

// The internal pool's blocks past its new size go back to the main free space, in the chunk bitmaps that hold them.
static uint64_t release_pool(const struct switch_apfs_io *io, struct work *w, uint64_t new_ip)
{
    uint64_t released = 0;
    uint64_t start = w->ip_base + new_ip;
    uint64_t end = w->ip_base + w->ip_blocks;
    for (uint64_t chunk = start / w->bpc; start < end && chunk * w->bpc < end; chunk++) {
        uint8_t *cib = edited_cib(io, w, (uint32_t)(chunk / w->cpc));
        if (!cib || chunk % w->cpc >= le32(cib + 36)) {
            return UINT64_MAX;
        }
        uint8_t *ci = chunk_info(cib, (uint32_t)(chunk % w->cpc));
        uint64_t addr = le64(ci + 8);
        uint8_t *bitmap = le64(ci + 24) ? edited_bitmap(io, w, le64(ci + 24)) : NULL;
        if (!bitmap || addr != chunk * w->bpc) {
            return UINT64_MAX;
        }
        uint64_t to = end < addr + w->bpc ? end : addr + w->bpc;
        uint64_t n = 0;
        for (uint64_t b = max64(start, addr); b < to; b++, n++) {
            if (!bit(bitmap, b - addr)) {
                return UINT64_MAX;
            }
            clear_bit(bitmap, b - addr);
        }
        put32(ci + 20, le32(ci + 20) + (uint32_t)n);
        released += n;
    }
    return released;
}

// The chunk-info blocks that go are freed in the pool, after which nothing in it may lie past its new size.
static int pool_ok(struct work *w, uint64_t new_ip, uint32_t cibs)
{
    for (uint32_t c = cibs; c < w->cibs; c++) {
        uint64_t at = cib_addr(w, c) - w->ip_base;
        if (cib_addr(w, c) < w->ip_base || at >= w->ip_blocks || !bit(w->ip_bitmap, at)) {
            return 0;
        }
        clear_bit(w->ip_bitmap, at);
    }
    return highest_bit(w->ip_bitmap, (uint32_t)w->ip_blocks) <= new_ip && w->ip_queue_end <= w->ip_base + new_ip;
}

static void edit_superblock(uint8_t *nx, uint64_t blocks, uint32_t max_fs)
{
    put64(nx + NX_BLOCK_COUNT, blocks);
    put32(nx + NX_MAX_FS, max_fs);
    seal(nx);
}

static void edit_spaceman(struct work *w, uint64_t blocks, uint32_t cibs, uint64_t free, uint64_t new_ip)
{
    uint8_t *dev = w->sm + SM_DEV;
    put64(dev, blocks);
    put64(dev + 8, div_up(blocks, w->bpc));
    put32(dev + 16, cibs);
    put64(dev + 24, free);
    memset(w->sm + w->addr_off + (size_t)8 * cibs, 0, (size_t)8 * (w->cibs - cibs));
    put32(dev + SM_DEV_BYTES + 32, w->addr_off + 8 * cibs);
    put64(w->sm + SM_IP_BLOCKS, new_ip);
    put16(w->sm + SM_FQ + FQ_LIMIT, switch_apfs_pool_queue_limit(new_ip));
    put16(w->sm + SM_FQ + SM_FQ_BYTES + FQ_LIMIT, switch_apfs_main_queue_limit(blocks));
    seal(w->sm);
}

// Bitmaps first and the superblocks last, as the kernel commits: every object written before the one naming it.
static int write_all(const struct switch_apfs_io *io, struct work *w)
{
    for (uint32_t i = 0; i < w->bitmaps_edited; i++) {
        if (io->write(io->context, w->bitmap_addr[i], w->bitmap[i])) {
            return 1;
        }
    }
    if (io->write(io->context, w->ip_bitmap_addr, w->ip_bitmap)) {
        return 1;
    }
    for (uint32_t i = 0; i < w->cibs_edited; i++) {
        seal(w->cib[i]);
        if (io->write(io->context, w->cib_addr[i], w->cib[i])) {
            return 1;
        }
    }
    return io->write(io->context, w->sm_addr, w->sm) || io->write(io->context, w->nx_addr, w->nx) ||
           io->write(io->context, 0, w->zero);
}

static uint32_t max_volumes(uint64_t blocks)
{
    uint64_t n = div_up(blocks * BS, MAX_FS_BYTES);
    return (uint32_t)(n > NX_MAX_VOLUMES ? NX_MAX_VOLUMES : n);
}

int switch_apfs_fit(const struct switch_apfs_io *io, void *work, uint64_t blocks)
{
    struct work *w = work;
    struct switch_apfs_layout layout;
    uint64_t end = 0;
    if (switch_apfs_measure(io, w, &layout) || blocks < layout.minimum || blocks > layout.blocks) {
        return 1;
    }
    if (blocks == layout.blocks) {
        return 0;
    }
    w->cibs_edited = 0;
    w->bitmaps_edited = 0;
    uint64_t chunks = div_up(blocks, w->bpc);
    uint64_t new_ip = new_ip_blocks(w, chunks);
    uint32_t cibs = (uint32_t)div_up(chunks, w->cpc);
    uint32_t max_fs = max_volumes(blocks);
    for (uint32_t i = max_fs; i < NX_MAX_VOLUMES; i++) {
        if (le64(w->nx + NX_FS_OIDS + (size_t)8 * i)) {
            return 1;
        }
    }
    uint64_t gone = cut_chunks(io, w, blocks, chunks);
    uint64_t released = gone == UINT64_MAX ? UINT64_MAX : release_pool(io, w, new_ip);
    uint32_t nodes = 0;
    if (released == UINT64_MAX || !pool_ok(w, new_ip, cibs) ||
        queue_end(io, w, 1, w->ip_base + new_ip, w->ip_base + w->ip_blocks, &end, &nodes)) {
        return 1;
    }
    edit_spaceman(w, blocks, cibs, w->free - gone + released, new_ip);
    edit_superblock(w->nx, blocks, max_fs);
    edit_superblock(w->zero, blocks, max_fs);
    return write_all(io, w);
}
