# evx_dedup.s - global event_id dedup, winner collection, person grouping.
# (GNU as, Intel syntax)
#
# Dedup: the parser writes every Cand into its event_id-hash partition
#   (4 KB chunks, see evx_parse.s); evx_dedup_part dedups one partition at a
#   time in a private, cache-resident table and marks every losing copy dead.
#
# Person table: 16-byte slots holding the canonical name as a Key16, claimed
#   with `lock cmpxchg16b`; a match is two word compares, no pointer chase.
#   The slot index *is* the person id. It is sized from an estimate (people are
#   a few % of events); every claim bumps a shared counter, and past
#   claim_limit (half the table) evx_collect returns UINT64_MAX so the C side
#   can enlarge the table and rerun the phase. Correct for any input; small and
#   cache-friendly for real data.

        .intel_syntax noprefix
        .text

        .set    PTRMASK, 0x0000FFFFFFFFFFFF
        .set    WCHUNK_WINS, 255        # Win chunk: 16-byte header + 255 Wins
        .set    CAND,    56

# ---------------------------------------------------------------------------
# void evx_dedup_part(uint64_t *table /*rdi*/, uint64_t tsize /*rsi*/,
#                     const char *arena /*rdx*/, uint32_t first /*ecx*/,
#                     uint32_t pbits /*r8d*/)
# Dedup of one worker's chunk list of ONE partition into a table private to
# the calling worker (zeroed by the caller for the first list of the
# partition, sized ~1.5x its rows so it stays in L1/L2). Plain loads and
# stores: no lock prefix, no fences, no coherence traffic. Slots hold Cand*.
# Home slot = ((eid_h32 << pbits) mod 2^32 * tsize) >> 32: the hash bits
# below the partition bits. Per event_id the smallest ingest stays; ties keep
# the resident copy (equal-ingest copies are identical). The losing copy is
# marked dead: user.k1 = 0, ts_off = -1.
# Chunk: [0] u32 count, [4] u32 next chunk (0xFFFFFFFF = end), [8] Cands.
# ---------------------------------------------------------------------------
        .globl  evx_dedup_part
        .type   evx_dedup_part, @function
        .p2align 5
evx_dedup_part:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        mov     rbx, rdi                # table
        mov     r12, rsi                # tsize
        mov     r13, rdx                # arena
        mov     r14d, ecx               # chunk index
        mov     ebp, r8d                # pbits
.Ldp_chunk:
        cmp     r14d, -1
        je      .Ldp_done
        mov     r15, r14
        shl     r15, 12
        add     r15, r13                # chunk
        mov     eax, dword ptr [r15 + 4]
        mov     r14d, eax               # next
        cmp     eax, -1
        je      .Ldp_nopf
        shl     rax, 12
        prefetcht0 [r13 + rax]          # next chunk's header + first Cand
.Ldp_nopf:
        mov     r10d, dword ptr [r15]   # count
        imul    r10, r10, CAND
        lea     r11, [r15 + 8]
        add     r10, r11                # Cand end
.Ldp_next:
        cmp     r11, r10
        jae     .Ldp_chunk
        mov     rdx, r11                # new Cand
        add     r11, CAND
        mov     eax, dword ptr [rdx + 52]
        mov     ecx, ebp
        shl     eax, cl
        imul    rax, r12
        shr     rax, 32                 # home slot
.Ldp_probe:
        mov     r9, qword ptr [rbx + rax*8]
        test    r9, r9
        jnz     .Ldp_occ
        mov     qword ptr [rbx + rax*8], rdx
        jmp     .Ldp_next
.Ldp_occ:
        mov     r8, qword ptr [rdx + 8]
        cmp     qword ptr [r9 + 8], r8  # k1: length + bytes 8..14 (or long tag)
        jne     .Ldp_pnext
        mov     rcx, r8
        shr     rcx, 56
        cmp     ecx, 0xFF
        je      .Ldp_long
        mov     rcx, qword ptr [rdx]
        cmp     qword ptr [r9], rcx     # short key: k0 decides
        jne     .Ldp_pnext
.Ldp_same:
        mov     rcx, qword ptr [rdx + 16]
        cmp     rcx, qword ptr [r9 + 16]
        jge     .Ldp_lose
        mov     qword ptr [rbx + rax*8], rdx
        mov     qword ptr [r9 + 32], 0  # the replaced copy is dead
        mov     qword ptr [r9 + 40], -1
        jmp     .Ldp_next
.Ldp_lose:
        mov     qword ptr [rdx + 32], 0 # this copy is dead
        mov     qword ptr [rdx + 40], -1
        jmp     .Ldp_next
.Ldp_long:
        push    rax
        push    rdx
        push    r10
        push    r11
        shr     r8, 32
        and     r8d, 0xFFFFFF
        mov     rdi, qword ptr [r9]
        mov     rsi, qword ptr [rdx]
        mov     rdx, r8
        call    evx_memeq               # keeps every GPR but rax / r11
        mov     ecx, eax
        pop     r11
        pop     r10
        pop     rdx
        pop     rax
        test    ecx, ecx
        jnz     .Ldp_same
.Ldp_pnext:
        inc     rax
        cmp     rax, r12
        jb      .Ldp_probe
        xor     eax, eax
        jmp     .Ldp_probe
.Ldp_done:
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   evx_dedup_part, .-evx_dedup_part

# ---------------------------------------------------------------------------
# uint64_t evx_collect(const CollectCtx *ctx /*rdi*/, char *chunks /*rsi*/,
#                      uint64_t nchunks /*rdx*/)
# Walks the Cands of nchunks consecutive arena chunks SEQUENTIALLY (dedup
# marked every losing copy dead, so the live ones are exactly the winners). Every winner that passed validation gets its person
# resolved (link table, else the raw id), interned in the person table,
# counted (`lock inc`) and emitted as a 16-byte Win. Returns the Win count, or
# UINT64_MAX on person-table overflow. Each Win goes into the calling
# worker's open 4 KB Win chunk (16-byte header + 255 Wins) of its person
# range (slot >> rshift; ranges are contiguous in dense-id order), so the
# grouping pass can run one range at a time in cache, without atomics.
# ctx is per worker (wheads is the worker's WinHead[R]).
#
# Software-pipelined in batches of up to 64 winners, so the random misses of
# many events are in flight together instead of one dependent chain per event:
#   stage 1  scan Cands (sequential; hardware prefetch), keep the live+valid
#   stage 2  read the user key, prefetch its link-table entry
#   stage 3  resolve the person key, prefetch the person slot
#   stage 4  intern (CAS claim / compare), emit (no per-person counter: the
#            counts are built range-locally after P4, without atomics)
#
# Frame: [0] Cand cursor, [8] arena end, [16] out base, [24] chunk's Cand end,
#   [32] next chunk, [40] batch size, [48] saved rbx, [56] loop index,
#   [64 + 8i] Cand*, [576 + 8i] key k0, [1088 + 8i] key k1, [1600 + 8i] slot idx
# ---------------------------------------------------------------------------
.ifndef COLLECT_BATCH
        .set    COLLECT_BATCH, 64
.endif
        .set    BATCH,  COLLECT_BATCH
        .set    B_C,    64
        .set    B_K0,   B_C + 8*BATCH
        .set    B_K1,   B_K0 + 8*BATCH
        .set    B_H,    B_K1 + 8*BATCH
        .set    B_PN,   B_H + 8*BATCH           # consumed Cand chunks pending recycling
        .set    B_PEND, B_PN + 8                # up to PEND_MAX chunk pointers
        .set    PEND_MAX, 8
        .set    CFRAME, B_PEND + 8*PEND_MAX + 8 # keeps rsp 16-byte aligned at calls

        .globl  evx_collect
        .type   evx_collect, @function
        .p2align 5
evx_collect:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, CFRAME
        mov     rbp, rdi                    # ctx
        mov     r12, qword ptr [rdi + 8]    # person table
        mov     r13, qword ptr [rdi + 16]   # person mask
        mov     r14, qword ptr [rdi + 24]   # counts
        xor     r15d, r15d                  # Wins emitted
        shl     rdx, 12
        add     rdx, rsi
        mov     qword ptr [rsp + 8], rdx    # end of the chunk range
        mov     qword ptr [rsp + 32], rsi   # next chunk
        mov     qword ptr [rsp], 0          # cursor = chunk end: load a chunk
        mov     qword ptr [rsp + 24], 0
        mov     qword ptr [rsp + 16], 0     # current Cand chunk (none yet)
        mov     qword ptr [rsp + B_PN], 0

.Lco_batch:
        # ---- stage 1: gather up to 64 live, valid Cands (sequential) -------
        xor     ecx, ecx                    # n
        mov     rax, qword ptr [rsp]
        mov     rdx, qword ptr [rsp + 24]
.Lco_s1:
        cmp     ecx, BATCH
        jae     .Lco_s1_done
        cmp     rax, rdx
        jb      .Lco_s1_have
        mov     r10, qword ptr [rsp + 32]   # next chunk
        cmp     r10, qword ptr [rsp + 8]
        jae     .Lco_s1_done                # range exhausted
        mov     r11, qword ptr [rsp + 16]   # the chunk being left
        test    r11, r11
        jz      .Lco_s1_load
        mov     r8, qword ptr [rsp + B_PN]
        cmp     r8, PEND_MAX
        jb      .Lco_s1_push
        test    ecx, ecx
        jnz     .Lco_s1_done                # pending list full: run this batch
        # nothing gathered yet, so every pending chunk is consumed: free now
        mov     r9, qword ptr [rbp + 96]
.Lco_s1_fl:
        dec     r8
        mov     r10, qword ptr [rsp + B_PEND + r8*8]
        mov     qword ptr [r10], r9
        mov     r9, r10
        test    r8, r8
        jnz     .Lco_s1_fl
        mov     qword ptr [rbp + 96], r9
        mov     qword ptr [rsp + B_PN], 0
        mov     r10, qword ptr [rsp + 32]   # reload the next chunk
.Lco_s1_push:
        mov     qword ptr [rsp + B_PEND + r8*8], r11
        inc     qword ptr [rsp + B_PN]
.Lco_s1_load:
        mov     qword ptr [rsp + 16], r10
        mov     r11d, dword ptr [r10]       # count
        imul    r11, r11, CAND
        lea     rax, [r10 + 8]
        lea     rdx, [rax + r11]
        add     r10, 4096
        mov     qword ptr [rsp + 32], r10
        jmp     .Lco_s1
.Lco_s1_have:
        mov     r9, rax
        add     rax, CAND
        cmp     qword ptr [r9 + 32], 0      # invalid or dead copy
        je      .Lco_s1
        mov     qword ptr [rsp + B_C + rcx*8], r9
        inc     ecx
        jmp     .Lco_s1
.Lco_s1_done:
        mov     qword ptr [rsp], rax
        mov     qword ptr [rsp + 24], rdx
        mov     qword ptr [rsp + 40], rcx
        test    ecx, ecx
        jz      .Lco_last

        # ---- stage 2: user key; prefetch its link entry (or person slot) ---
        mov     qword ptr [rsp + 56], 0
.Lco_s2:
        mov     rcx, qword ptr [rsp + 56]
        cmp     rcx, qword ptr [rsp + 40]
        jae     .Lco_s2_done
        mov     r9, qword ptr [rsp + B_C + rcx*8]
        mov     rsi, qword ptr [r9 + 32]
        test    rsi, rsi
        jnz     .Lco_s2_valid
        mov     qword ptr [rsp + B_C + rcx*8], 0    # kept copy invalid: dropped
        jmp     .Lco_s2_next
.Lco_s2_valid:
        mov     rdi, qword ptr [r9 + 24]
        mov     qword ptr [rsp + B_K0 + rcx*8], rdi
        mov     qword ptr [rsp + B_K1 + rcx*8], rsi
        call    evx_khash                   # clobbers rax, r11
        mov     rcx, qword ptr [rsp + 56]
        mov     rdx, qword ptr [rbp + 32]   # link slots
        test    rdx, rdx
        jz      .Lco_s2_nolink
        and     rax, qword ptr [rbp + 40]
        shl     rax, 5
        prefetcht0 [rdx + rax]
        jmp     .Lco_s2_next
.Lco_s2_nolink:
        and     rax, r13                    # person key = raw id
        mov     qword ptr [rsp + B_H + rcx*8], rax
        shl     rax, 4
        prefetchw [r12 + rax]
.Lco_s2_next:
        inc     qword ptr [rsp + 56]
        jmp     .Lco_s2
.Lco_s2_done:

        # ---- stage 3: canonical person key; prefetch person slot + counter -
        cmp     qword ptr [rbp + 32], 0
        je      .Lco_s3_done
        mov     qword ptr [rsp + 56], 0
.Lco_s3:
        mov     rcx, qword ptr [rsp + 56]
        cmp     rcx, qword ptr [rsp + 40]
        jae     .Lco_s3_done
        cmp     qword ptr [rsp + B_C + rcx*8], 0
        je      .Lco_s3_next
        mov     rdi, qword ptr [rsp + B_K0 + rcx*8]
        mov     rsi, qword ptr [rsp + B_K1 + rcx*8]
        mov     rdx, qword ptr [rbp + 32]
        mov     rcx, qword ptr [rbp + 40]
        call    evx_canon                   # entry prefetched in stage 2
        mov     rcx, qword ptr [rsp + 56]
        mov     qword ptr [rsp + B_K0 + rcx*8], rax
        mov     qword ptr [rsp + B_K1 + rcx*8], rdx
        mov     rdi, rax
        mov     rsi, rdx
        call    evx_khash
        mov     rcx, qword ptr [rsp + 56]
        and     rax, r13
        mov     qword ptr [rsp + B_H + rcx*8], rax
        shl     rax, 4
        prefetchw [r12 + rax]
.Lco_s3_next:
        inc     qword ptr [rsp + 56]
        jmp     .Lco_s3
.Lco_s3_done:

        # ---- stage 4: intern, count, emit ---------------------------------
        mov     qword ptr [rsp + 56], 0
.Lco_s4:
        mov     rcx, qword ptr [rsp + 56]
        cmp     rcx, qword ptr [rsp + 40]
        jae     .Lco_recycle
        cmp     qword ptr [rsp + B_C + rcx*8], 0
        je      .Lco_s4_next
        mov     rsi, qword ptr [rsp + B_K0 + rcx*8]     # person key k0
        mov     r10, qword ptr [rsp + B_K1 + rcx*8]     # person key k1
        mov     r9, qword ptr [rsp + B_H + rcx*8]       # home slot
.Lco_probe:
        mov     rax, r9
        shl     rax, 4
        add     rax, r12
        mov     rdi, qword ptr [rax + 8]
        test    rdi, rdi
        jnz     .Lco_cmp
        # claim the empty slot: {0,0} -> {k0, k1}
        mov     r8, rax
        mov     qword ptr [rsp + 48], rbx
        mov     rbx, rsi
        mov     rcx, r10
        xor     eax, eax
        xor     edx, edx
        lock cmpxchg16b xmmword ptr [r8]
        mov     rbx, qword ptr [rsp + 48]
        jz      .Lco_claimed
        jmp     .Lco_probe                  # lost the race: re-read this slot
.Lco_cmp:
        cmp     rdi, r10
        jne     .Lco_pnext
        mov     rdi, qword ptr [rax]
        mov     rdx, rsi
        mov     rcx, r10
        push    rsi
        mov     rsi, r10
        call    evx_keq                     # short key: one word compare
        pop     rsi
        test    eax, eax
        jnz     .Lco_found
.Lco_pnext:
        inc     r9
        and     r9, r13
        jmp     .Lco_probe
.Lco_claimed:
        mov     rax, qword ptr [rbp + 48]
        mov     ecx, 1
        lock xadd qword ptr [rax], rcx
        inc     rcx
        cmp     rcx, qword ptr [rbp + 56]
        ja      .Lco_overflow
.Lco_found:
        mov     rcx, qword ptr [rsp + 56]
        mov     rcx, qword ptr [rsp + B_C + rcx*8]
        # emit into this worker's open Win chunk of the slot's person range
        mov     eax, dword ptr [rbp + 88]   # rshift
        shrx    r8, r9, rax                 # range
        shl     r8, 5
        add     r8, qword ptr [rbp + 80]    # its WinHead
        mov     r10, qword ptr [r8]         # cur
        cmp     r10, qword ptr [r8 + 8]     # end (0/0 = no chunk yet)
        jb      .Lco_wslot
        mov     rdx, qword ptr [r8 + 8]
        test    rdx, rdx
        jz      .Lco_wfirst
        mov     dword ptr [rdx - 4096], WCHUNK_WINS   # the old chunk is full
.Lco_wfirst:
        mov     r10, qword ptr [rbp + 96]   # recycled Cand chunk (cache-warm)?
        test    r10, r10
        jz      .Lco_wfresh
        mov     rax, qword ptr [r10]
        mov     qword ptr [rbp + 96], rax   # pop
        mov     rdx, r10
        sub     rdx, qword ptr [rbp + 64]
        shr     rdx, 12                     # its chunk index
        jmp     .Lco_wlink
.Lco_wfresh:
        mov     rax, qword ptr [rbp + 72]   # &arena_next
        mov     edx, 1
        lock xadd qword ptr [rax], rdx      # new chunk index
        mov     r10, rdx
        shl     r10, 12
        add     r10, qword ptr [rbp + 64]   # chunk
.Lco_wlink:
        mov     eax, dword ptr [r8 + 16]
        mov     dword ptr [r10 + 4], eax    # next = old list head
        mov     dword ptr [r8 + 16], edx    # list head = new chunk
        lea     rax, [r10 + 4096]
        mov     qword ptr [r8 + 8], rax
        add     r10, 16
.Lco_wslot:
        lea     rax, [r10 + 16]
        mov     qword ptr [r8], rax
        inc     dword ptr [r8 + 20]         # Wins of this range (this worker)
        mov     rax, qword ptr [rcx + 40]   # ts_off
        mov     qword ptr [r10], rax
        mov     dword ptr [r10 + 8], r9d
        mov     eax, dword ptr [rcx + 48]   # price_cat
        mov     dword ptr [r10 + 12], eax
        inc     r15
.Lco_s4_next:
        inc     qword ptr [rsp + 56]
        jmp     .Lco_s4

        # ---- after stage 4: the pending Cand chunks are fully consumed ------
.Lco_recycle:
        lea     rax, [rip + .Lco_batch]
        jmp     .Lco_flush
.Lco_last:
        lea     rax, [rip + .Lco_done]
.Lco_flush:
        mov     rcx, qword ptr [rsp + B_PN]
        mov     rdx, qword ptr [rbp + 96]
.Lco_fl:
        test    rcx, rcx
        jz      .Lco_fl_done
        dec     rcx
        mov     r8, qword ptr [rsp + B_PEND + rcx*8]
        mov     qword ptr [r8], rdx         # push on the worker's free list
        mov     rdx, r8
        jmp     .Lco_fl
.Lco_fl_done:
        mov     qword ptr [rbp + 96], rdx
        mov     qword ptr [rsp + B_PN], 0
        jmp     rax

.Lco_overflow:
        mov     r15, -1
.Lco_done:
        mov     rax, r15
        add     rsp, CFRAME
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   evx_collect, .-evx_collect

# ---------------------------------------------------------------------------
# void evx_count_range(const char *warena /*rdi*/, uint32_t first /*esi*/,
#                      const uint32_t *remap /*rdx*/, uint64_t *cnt /*rcx*/)
# Events per dense person for one worker's Win list of one person range:
# cnt[remap[slot]]++ (the range owns those counters; plain increments).
# ---------------------------------------------------------------------------
        .globl  evx_count_range
        .type   evx_count_range, @function
        .p2align 5
evx_count_range:
.Lcr_chunk:
        cmp     esi, -1
        je      .Lcr_done
        mov     r9, rsi
        shl     r9, 12
        add     r9, rdi                 # chunk
        mov     esi, dword ptr [r9 + 4] # next
        mov     r10d, dword ptr [r9]    # count
        shl     r10, 4
        lea     r11, [r9 + 16]
        add     r10, r11                # Win end
.Lcr_next:
        cmp     r11, r10
        jae     .Lcr_chunk
        mov     eax, dword ptr [r11 + 8]        # person slot
        mov     eax, dword ptr [rdx + rax*4]    # dense id
        inc     qword ptr [rcx + rax*8]
        add     r11, 16
        jmp     .Lcr_next
.Lcr_done:
        ret
        .size   evx_count_range, .-evx_count_range

# ---------------------------------------------------------------------------
# void evx_group_range(const char *warena /*rdi*/, uint32_t first /*esi*/,
#                      const uint32_t *remap /*rdx*/, uint64_t *cursor /*rcx*/,
#                      Ev *dst /*r8*/)
# Counting-sort placement of one worker's Win chunk list of ONE person range:
# k = remap[slot], dst[cursor[k]++] = event. The range owns its persons'
# cursors and Ev slice, so plain increments suffice, and the cursors, the
# remap slice and the destination all stay in L1/L2.
# Win chunk: [0] u32 count, [4] u32 next (0xFFFFFFFF = end), [16] Wins.
# ---------------------------------------------------------------------------
        .globl  evx_group_range
        .type   evx_group_range, @function
        .p2align 5
evx_group_range:
.Lgr_chunk:
        cmp     esi, -1
        je      .Lgr_done
        mov     r9, rsi
        shl     r9, 12
        add     r9, rdi                 # chunk
        mov     esi, dword ptr [r9 + 4] # next
        mov     r10d, dword ptr [r9]    # count
        shl     r10, 4
        lea     r11, [r9 + 16]
        add     r10, r11                # Win end
        cmp     esi, -1
        je      .Lgr_next
        mov     eax, esi
        shl     rax, 12
        prefetcht0 [rdi + rax]          # next chunk's header
.Lgr_next:
        cmp     r11, r10
        jae     .Lgr_chunk
        mov     eax, dword ptr [r11 + 8]        # person slot
        mov     eax, dword ptr [rdx + rax*4]    # dense id
        mov     r9, qword ptr [rcx + rax*8]
        lea     rax, [rcx + rax*8]
        inc     qword ptr [rax]
        shl     r9, 4
        add     r9, r8
        mov     rax, qword ptr [r11]            # Ev: ts_off, price_cat, pad 0
        mov     qword ptr [r9], rax
        mov     eax, dword ptr [r11 + 12]
        mov     qword ptr [r9 + 8], rax
        add     r11, 16
        jmp     .Lgr_next
.Lgr_done:
        ret
        .size   evx_group_range, .-evx_group_range

        .section .note.GNU-stack,"",@progbits
