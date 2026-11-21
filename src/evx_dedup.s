# evx_dedup.s - global event_id dedup, winner collection, person grouping.
# (GNU as, Intel syntax)
#
# Dedup table: open addressing, 8-byte slots = (eid_hash >> 48) << 48 | Cand*.
#   x86-64 user pointers fit in 47 bits, so the top 16 bits carry a hash tag:
#   most probe mismatches are rejected from the slot word alone, without
#   touching the candidate's cache line. Insert and replace are single
#   `lock cmpxchg` operations, so all workers insert concurrently without locks.
#   A slot only ever changes from 0 to a key, or between copies of the same
#   key, so there is no ABA hazard.
#
# Person table: open addressing, 8-byte slots = Cand* whose pname defines the
#   person. The slot index *is* the person id (no separate counter, no
#   publication race): the CAS that claims an empty slot also names it.

        .intel_syntax noprefix
        .text

        .set    PTRMASK, 0x0000FFFFFFFFFFFF

# ---------------------------------------------------------------------------
# void evx_dedup_insert(uint64_t *table /*rdi*/, uint64_t mask /*rsi*/,
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
        mov     r12, rsi                # mask
        mov     r13, rdx                # cursor
        shl     rcx, 6
        lea     r14, [rdx + rcx]        # end
        movabs  rbp, PTRMASK
.Ldi_next:
        cmp     r13, r14
        jae     .Ldi_done
        lea     rax, [r13 + 512]        # 8 candidates ahead
        cmp     rax, r14
        jae     .Ldi_nopf
        mov     rax, qword ptr [rax]
        and     rax, r12
        prefetchw [rbx + rax*8]
.Ldi_nopf:
        mov     rax, qword ptr [r13]    # hash
        mov     r15, rax
        and     r15, r12                # idx
        shr     rax, 48
        shl     rax, 48
        or      rax, r13
        mov     rdx, rax                # tagged new value
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
        mov     r10d, dword ptr [r9 + 16]
        cmp     r10d, dword ptr [r13 + 16]
        jne     .Ldi_pnext
        mov     r8, rax                 # keep slot value (memeq keeps r8)
        push    rdx
        mov     rdi, qword ptr [r9 + 8]
        mov     rsi, qword ptr [r13 + 8]
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
        mov     r8, qword ptr [r13 + 24]
        cmp     r8, qword ptr [r9 + 24]
        jge     .Ldi_adv
        lock cmpxchg qword ptr [rbx + r15*8], rdx
        jz      .Ldi_adv
        jmp     .Ldi_same               # rax = newer resident of same id
.Ldi_pnext:
        inc     r15
        and     r15, r12
        jmp     .Ldi_probe
.Ldi_adv:
        add     r13, 64
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
# Walks dedup slots [begin, end) sequentially (hardware prefetch friendly),
# counts occupied slots (= unique event ids) into *occupied,
# drops winners whose flag says invalid, interns the canonical person name,
# counts events per person with `lock inc`, and emits 16-byte Win records.
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
        push    r8                          # [rsp + 8] = occupied out ptr
        push    0                           # [rsp]     = occupied count
        mov     rbx, qword ptr [rdi]        # dedup table
        mov     r12, qword ptr [rdi + 8]    # person table
        mov     r13, qword ptr [rdi + 16]   # person mask
        mov     r14, qword ptr [rdi + 24]   # counts
        mov     rbp, rcx                    # out base
        mov     r15, rcx                    # out cursor
        mov     r8, rsi                     # slot index
        mov     r9, rdx                     # slot end
.Lco_slot:
        cmp     r8, r9
        jae     .Lco_done
        mov     rax, qword ptr [rbx + r8*8]
        inc     r8
        test    rax, rax
        jz      .Lco_slot
        inc     qword ptr [rsp]
        movabs  r10, PTRMASK
        and     r10, rax                    # winner Cand
        test    byte ptr [r10 + 20], 1
        jz      .Lco_slot                   # kept copy is invalid: event dropped
        mov     ecx, dword ptr [r10 + 52]   # name tag
        and     rcx, r13                    # person slot
.Lco_probe:
        mov     rdx, qword ptr [r12 + rcx*8]
        test    rdx, rdx
        jnz     .Lco_cmp
        xor     eax, eax
        lock cmpxchg qword ptr [r12 + rcx*8], r10
        jz      .Lco_got
        mov     rdx, rax
.Lco_cmp:
        mov     eax, dword ptr [rdx + 52]
        cmp     eax, dword ptr [r10 + 52]
        jne     .Lco_pnext
        mov     eax, dword ptr [rdx + 48]
        cmp     eax, dword ptr [r10 + 48]
        jne     .Lco_pnext
        mov     rdi, qword ptr [rdx + 40]
        mov     rsi, qword ptr [r10 + 40]
        mov     edx, eax
        call    evx_memeq                   # preserves rcx, r8-r10
        test    eax, eax
        jnz     .Lco_got
.Lco_pnext:
        inc     rcx
        and     rcx, r13
        jmp     .Lco_probe
.Lco_got:
        lock inc dword ptr [r14 + rcx*4]
        mov     rax, qword ptr [r10 + 32]
        mov     qword ptr [r15], rax
        mov     dword ptr [r15 + 8], ecx
        mov     eax, dword ptr [r10 + 56]
        mov     dword ptr [r15 + 12], eax
        add     r15, 16
        jmp     .Lco_slot
.Lco_done:
        pop     rcx
        pop     rdx
        mov     qword ptr [rdx], rcx
        mov     rax, r15
        sub     rax, rbp
        shr     rax, 4
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
