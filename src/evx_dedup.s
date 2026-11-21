# evx_dedup.s - global event_id dedup, winner collection, person grouping.
# (GNU as, Intel syntax)
#
# Dedup table: open addressing over an EXACT number of 8-byte slots (about
#   1.5x candidates, not rounded to a power of two): the home slot is
#   (eid_h32 * size) >> 32, a multiply instead of a mask. A slot holds
#   (eid_h32 & 0xFFFF) << 48 | Cand*. x86-64 user pointers fit in 47 bits, so
#   the top 16 bits carry a hash tag taken from the LOW hash bits (the index
#   uses the high ones), and most probe mismatches are rejected from the slot
#   word alone. Insert and replace are single `lock cmpxchg` operations, so all
#   workers insert concurrently without locks. A slot only ever changes from 0
#   to a key, or between copies of the same key, so there is no ABA hazard.
#
# Person table: 16-byte slots {name, len, tag} claimed with `lock cmpxchg16b`.
#   The slot index *is* the person id. It is sized from an estimate (people are
#   a few % of events); every claim bumps a shared counter, and past
#   claim_limit (half the table) evx_collect returns UINT64_MAX so the C side
#   can enlarge the table and rerun the phase. Correct for any input; small and
#   cache-friendly for real data.

        .intel_syntax noprefix
        .text

        .set    PTRMASK, 0x0000FFFFFFFFFFFF
        .set    CAND,    48

# ---------------------------------------------------------------------------
# void evx_dedup_insert(uint64_t *table /*rdi*/, uint64_t size /*rsi*/,
#                       Cand *c /*rdx*/, uint64_t n /*rcx*/)
# Keeps, per event_id, the Cand with the smallest ingest (ties keep the
# resident copy: equal-ingest copies are identical by the rules).
# Software prefetch: the slot of the candidate 8 ahead is prefetched with
# prefetchw (we are about to CAS it) to overlap the random DRAM misses.
# ---------------------------------------------------------------------------
        .globl  evx_dedup_insert
        .type   evx_dedup_insert, @function
        .p2align 5
evx_dedup_insert:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        mov     rbx, rdi                # table
        mov     r12, rsi                # size
        mov     r13, rdx                # cursor
        imul    rcx, rcx, CAND
        lea     r14, [rdx + rcx]        # end
        movabs  rbp, PTRMASK
.Ldi_next:
        cmp     r13, r14
        jae     .Ldi_done
        lea     rax, [r13 + 8*CAND]     # 8 candidates ahead
        cmp     rax, r14
        jae     .Ldi_nopf
        mov     eax, dword ptr [rax + 12]
        imul    rax, r12
        shr     rax, 32
        prefetchw [rbx + rax*8]
.Ldi_nopf:
        mov     eax, dword ptr [r13 + 12]   # eid_h32
        mov     r15, rax
        imul    r15, r12
        shr     r15, 32                 # home slot
        movzx   edx, ax
        shl     rdx, 48
        or      rdx, r13                # tagged new value
.Ldi_probe:
        mov     rax, qword ptr [rbx + r15*8]
        test    rax, rax
        jnz     .Ldi_occ
        lock cmpxchg qword ptr [rbx + r15*8], rdx
        jz      .Ldi_adv                # claimed an empty slot
        # lost the race: rax = what is there now
.Ldi_occ:
        mov     r8, rax
        xor     r8, rdx
        shr     r8, 48
        jnz     .Ldi_pnext              # tag differs: different id
        mov     r9, rax
        and     r9, rbp                 # resident Cand
        mov     r10d, dword ptr [r9 + 8]
        cmp     r10d, dword ptr [r13 + 8]
        jne     .Ldi_pnext
        mov     r8, rax                 # keep slot value (memeq keeps r8)
        push    rdx
        mov     rdi, qword ptr [r9]
        mov     rsi, qword ptr [r13]
        mov     rdx, r10
        call    evx_memeq
        pop     rdx
        test    eax, eax
        mov     rax, r8
        jz      .Ldi_pnext
.Ldi_same:
        # same event_id: replace only with a strictly smaller ingest
        mov     r9, rax
        and     r9, rbp
        mov     r8, qword ptr [r13 + 16]
        cmp     r8, qword ptr [r9 + 16]
        jge     .Ldi_adv
        lock cmpxchg qword ptr [rbx + r15*8], rdx
        jz      .Ldi_adv
        jmp     .Ldi_same               # rax = newer resident of same id
.Ldi_pnext:
        inc     r15
        cmp     r15, r12
        jb      .Ldi_probe
        xor     r15d, r15d
        jmp     .Ldi_probe
.Ldi_adv:
        add     r13, CAND
        jmp     .Ldi_next
.Ldi_done:
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   evx_dedup_insert, .-evx_dedup_insert

# ---------------------------------------------------------------------------
# uint64_t evx_collect(const CollectCtx *ctx /*rdi*/, uint64_t slot_begin /*rsi*/,
#                      uint64_t slot_end /*rdx*/, Win *out /*rcx*/,
#                      uint64_t *occupied /*r8*/)
# Walks dedup slots [begin, end) sequentially (hardware prefetch friendly) and
# counts occupied slots (= unique event ids) into *occupied. For each winner
# that passed validation: resolve the person (link table, else the raw id),
# intern the canonical name in the person table, `lock inc` its count, and
# emit a 16-byte Win. Returns the Win count, or UINT64_MAX on table overflow.
#
# Frame: [rsp] slot idx, [8] slot end, [16] out base, [24] occupied count,
#        [32] occupied ptr, [40] current Cand, [48] saved rbx.
# ---------------------------------------------------------------------------
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
        sub     rsp, 56
        mov     rbp, rdi                    # ctx
        mov     rbx, qword ptr [rdi]        # dedup table
        mov     r12, qword ptr [rdi + 8]    # person table
        mov     r13, qword ptr [rdi + 16]   # person mask
        mov     r14, qword ptr [rdi + 24]   # counts
        mov     r15, rcx                    # out cursor
        mov     qword ptr [rsp], rsi
        mov     qword ptr [rsp + 8], rdx
        mov     qword ptr [rsp + 16], rcx
        mov     qword ptr [rsp + 24], 0
        mov     qword ptr [rsp + 32], r8
.Lco_slot:
        mov     rax, qword ptr [rsp]
        cmp     rax, qword ptr [rsp + 8]
        jae     .Lco_done
        mov     rcx, qword ptr [rbx + rax*8]
        inc     rax
        mov     qword ptr [rsp], rax
        test    rcx, rcx
        jz      .Lco_slot
        inc     qword ptr [rsp + 24]
        movabs  rax, PTRMASK
        and     rcx, rax                    # winner Cand
        mov     qword ptr [rsp + 40], rcx
        mov     rdi, qword ptr [rcx + 24]   # user_ptr
        test    rdi, rdi
        jz      .Lco_slot                   # kept copy is invalid: event dropped
        mov     esi, dword ptr [rcx + 32]
        call    evx_hash                    # rax = hash(user id)
        mov     r8, rax
        cmp     qword ptr [rbp + 32], 0
        je      .Lco_self
        mov     rcx, qword ptr [rsp + 40]
        mov     rdi, qword ptr [rbp + 32]
        mov     rsi, qword ptr [rbp + 40]
        mov     rdx, qword ptr [rcx + 24]
        mov     ecx, dword ptr [rcx + 32]
        call    evx_lookup                  # preserves r8
        test    rax, rax
        jz      .Lco_self
        mov     rsi, qword ptr [rax + 16]   # canonical name
        mov     edx, dword ptr [rax + 24]
        mov     r11d, dword ptr [rax + 28]
        jmp     .Lco_intern
.Lco_self:
        mov     rcx, qword ptr [rsp + 40]
        mov     rsi, qword ptr [rcx + 24]
        mov     edx, dword ptr [rcx + 32]
        mov     r11, r8
        shr     r11, 32
.Lco_intern:
        # rsi = name, edx = len, r11d = tag
        mov     r10, r11
        shl     r10, 32
        or      r10, rdx                    # slot qword 2: tag << 32 | len
        mov     r9d, r11d
        and     r9, r13                     # home slot
.Lco_probe:
        mov     rax, r9
        shl     rax, 4
        add     rax, r12
        mov     rdi, qword ptr [rax]
        test    rdi, rdi
        jnz     .Lco_cmp
        # claim the empty slot: {0,0} -> {name, tag|len}
        mov     r8, rax
        mov     qword ptr [rsp + 48], rbx
        mov     rbx, rsi
        mov     rcx, r10
        xor     eax, eax
        xor     edx, edx
        lock cmpxchg16b xmmword ptr [r8]
        mov     rbx, qword ptr [rsp + 48]
        jz      .Lco_claimed
        mov     edx, r10d                   # lost the race: retry this slot
        jmp     .Lco_probe
.Lco_cmp:
        cmp     qword ptr [rax + 8], r10
        jne     .Lco_pnext
        call    evx_memeq                   # (slot name, name, len): keeps rdx..r10
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
        lock inc dword ptr [r14 + r9*4]
        mov     rcx, qword ptr [rsp + 40]
        mov     rax, qword ptr [rcx + 40]   # ts_off
        mov     qword ptr [r15], rax
        mov     dword ptr [r15 + 8], r9d
        mov     eax, dword ptr [rcx + 36]   # price_cat
        mov     dword ptr [r15 + 12], eax
        add     r15, 16
        jmp     .Lco_slot
.Lco_overflow:
        mov     r15, -1
.Lco_done:
        mov     rax, qword ptr [rsp + 32]
        mov     rcx, qword ptr [rsp + 24]
        mov     qword ptr [rax], rcx
        mov     rax, r15
        cmp     r15, -1
        je      .Lco_ret
        sub     rax, qword ptr [rsp + 16]
        shr     rax, 4
.Lco_ret:
        add     rsp, 56
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   evx_collect, .-evx_collect

# ---------------------------------------------------------------------------
# void evx_scatter(const Win *w /*rdi*/, uint64_t n /*rsi*/, const uint32_t *remap /*rdx*/,
#                  uint64_t *cursor /*rcx*/, Ev *dst /*r8*/)
# Counting-sort scatter: person slot -> dense person id k = remap[pid]; each
# event goes to `lock xadd cursor[k], 1`, which makes every person's events
# one contiguous range of the Ev array.
# ---------------------------------------------------------------------------
        .globl  evx_scatter
        .type   evx_scatter, @function
        .p2align 5
evx_scatter:
        shl     rsi, 4
        add     rsi, rdi                    # end
.Lsc_loop:
        cmp     rdi, rsi
        jae     .Lsc_done
        mov     eax, dword ptr [rdi + 8]    # person slot
        mov     eax, dword ptr [rdx + rax*4]    # dense person id
        mov     r9d, 1
        lock xadd qword ptr [rcx + rax*8], r9
        shl     r9, 4
        mov     r10, qword ptr [rdi]
        mov     qword ptr [r8 + r9], r10
        mov     r10d, dword ptr [rdi + 12]
        mov     dword ptr [r8 + r9 + 8], r10d
        mov     dword ptr [r8 + r9 + 12], 0
        add     rdi, 16
        jmp     .Lsc_loop
.Lsc_done:
        ret
        .size   evx_scatter, .-evx_scatter

        .section .note.GNU-stack,"",@progbits
