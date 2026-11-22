# evx_resolve.s - P1.5: raw user id -> canonical person slot (GNU as, Intel syntax).
#
# uint64_t evx_resolve_raw(const ResolveCtx *ctx /*rdi*/, uint64_t s0 /*rsi*/,
#                          uint64_t s1 /*rdx*/)
# For every claimed raw-ID slot s in [s0, s1): the canonical name (links,
# evx_canon, else the id itself) is interned in the person table (16-byte
# Key16 slots claimed with lock cmpxchg16b) and pslot[s] = its person slot.
# Runs once per DISTINCT raw id (about 1/16 of the rows) instead of once per
# winning row. Returns the claims made, or UINT64_MAX when the shared claim
# count passed claim_limit (the caller enlarges the table and reruns P1.5).
#
# Software-pipelined in batches of RBATCH ids, so the link-table and
# person-table misses of many ids are in flight together:
#   stage 1  scan raw slots (sequential), keep the claimed ones
#   stage 2  khash of the raw key; prefetch its link entry (or person slot)
#   stage 3  canonical key (links), khash, prefetch the person slot
#   stage 4  intern (CAS claim / compare), pslot[s] = slot
#
# Frame: [0] cursor, [8] s1, [16] n, [24] i, [32] claims, [40] saved rbx,
#   then RBATCH x {raw slot, k0, k1, home slot}.

        .intel_syntax noprefix
        .text

.ifndef RBATCH
        .set    RBATCH, 32
.endif
        .set    R_S,    56
        .set    R_K0,   R_S + 8*RBATCH
        .set    R_K1,   R_K0 + 8*RBATCH
        .set    R_H,    R_K1 + 8*RBATCH
        .set    RFRAME, R_H + 8*RBATCH          # 56 + 32*RBATCH == 8 mod 16

        .globl  evx_resolve_raw
        .type   evx_resolve_raw, @function
        .p2align 5
evx_resolve_raw:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, RFRAME
        mov     rbp, rdi
        mov     qword ptr [rsp], rsi
        mov     qword ptr [rsp + 8], rdx
        mov     qword ptr [rsp + 32], 0
        mov     r12, qword ptr [rbp + 16]       # persons
        mov     r13, qword ptr [rbp + 24]       # person mask

.Lrs_batch:
        # ---- stage 1: up to RBATCH claimed raw slots ------------------------
        xor     r15d, r15d
        mov     rax, qword ptr [rsp]
        mov     rdx, qword ptr [rsp + 8]
        mov     rsi, qword ptr [rbp]            # raw table
.Lrs_s1:
        cmp     r15, RBATCH
        jae     .Lrs_s1_done
        cmp     rax, rdx
        jae     .Lrs_s1_done
        mov     rcx, rax
        shl     rcx, 4
        mov     r8, qword ptr [rsi + rcx + 8]
        test    r8, r8
        jz      .Lrs_s1_next
        mov     r9, qword ptr [rsi + rcx]
        mov     qword ptr [rsp + R_S + r15*8], rax
        mov     qword ptr [rsp + R_K0 + r15*8], r9
        mov     qword ptr [rsp + R_K1 + r15*8], r8
        inc     r15
.Lrs_s1_next:
        inc     rax
        jmp     .Lrs_s1
.Lrs_s1_done:
        mov     qword ptr [rsp], rax
        test    r15, r15
        jz      .Lrs_done

        # ---- stage 2: hash; prefetch the link entry (or the person slot) ----
        xor     r14d, r14d
.Lrs_s2:
        cmp     r14, r15
        jae     .Lrs_s2_done
        mov     rdi, qword ptr [rsp + R_K0 + r14*8]
        mov     rsi, qword ptr [rsp + R_K1 + r14*8]
        call    evx_khash                       # clobbers rax, r11
        mov     rdx, qword ptr [rbp + 32]       # link slots
        test    rdx, rdx
        jz      .Lrs_s2_nolink
        and     rax, qword ptr [rbp + 40]
        shl     rax, 5
        prefetcht0 [rdx + rax]
        jmp     .Lrs_s2_next
.Lrs_s2_nolink:
        and     rax, r13
        mov     qword ptr [rsp + R_H + r14*8], rax
        shl     rax, 4
        prefetchw [r12 + rax]
.Lrs_s2_next:
        inc     r14
        jmp     .Lrs_s2
.Lrs_s2_done:

        # ---- stage 3: canonical key; prefetch the person slot ---------------
        cmp     qword ptr [rbp + 32], 0
        je      .Lrs_s3_done
        xor     r14d, r14d
.Lrs_s3:
        cmp     r14, r15
        jae     .Lrs_s3_done
        mov     rdi, qword ptr [rsp + R_K0 + r14*8]
        mov     rsi, qword ptr [rsp + R_K1 + r14*8]
        mov     rdx, qword ptr [rbp + 32]
        mov     rcx, qword ptr [rbp + 40]
        call    evx_canon
        mov     qword ptr [rsp + R_K0 + r14*8], rax
        mov     qword ptr [rsp + R_K1 + r14*8], rdx
        mov     rdi, rax
        mov     rsi, rdx
        call    evx_khash
        and     rax, r13
        mov     qword ptr [rsp + R_H + r14*8], rax
        shl     rax, 4
        prefetchw [r12 + rax]
        inc     r14
        jmp     .Lrs_s3
.Lrs_s3_done:

        # ---- stage 4: intern ---------------------------------------------------
        xor     r14d, r14d
.Lrs_s4:
        cmp     r14, r15
        jae     .Lrs_batch
        mov     rsi, qword ptr [rsp + R_K0 + r14*8]
        mov     r10, qword ptr [rsp + R_K1 + r14*8]
        mov     r9, qword ptr [rsp + R_H + r14*8]
.Lrs_probe:
        mov     rax, r9
        shl     rax, 4
        add     rax, r12
        mov     rdi, qword ptr [rax + 8]
        test    rdi, rdi
        jnz     .Lrs_cmp
        mov     r8, rax
        mov     rbx, rsi
        mov     rcx, r10
        xor     eax, eax
        xor     edx, edx
        lock cmpxchg16b xmmword ptr [r8]
        jnz     .Lrs_probe                      # lost the race: re-read the slot
        inc     qword ptr [rsp + 32]
        mov     rax, qword ptr [rbp + 48]
        mov     ecx, 1
        lock xadd qword ptr [rax], rcx
        inc     rcx
        cmp     rcx, qword ptr [rbp + 56]
        ja      .Lrs_overflow
        jmp     .Lrs_found
.Lrs_cmp:
        cmp     rdi, r10
        jne     .Lrs_pnext
        mov     rdi, qword ptr [rax]
        mov     rdx, rsi
        mov     rcx, r10
        push    rsi
        mov     rsi, r10
        call    evx_keq                         # keeps every GPR but rax / r11
        pop     rsi
        test    eax, eax
        jnz     .Lrs_found
.Lrs_pnext:
        inc     r9
        and     r9, r13
        jmp     .Lrs_probe
.Lrs_found:
        mov     rax, qword ptr [rsp + R_S + r14*8]
        mov     rcx, qword ptr [rbp + 8]        # pslot[]
        mov     dword ptr [rcx + rax*4], r9d
        inc     r14
        jmp     .Lrs_s4

.Lrs_overflow:
        mov     qword ptr [rsp + 32], -1
.Lrs_done:
        mov     rax, qword ptr [rsp + 32]
        add     rsp, RFRAME
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   evx_resolve_raw, .-evx_resolve_raw

        .section .note.GNU-stack,"",@progbits
