# evx_parse.s - batch event-row parser (GNU as, Intel syntax).
#
# uint64_t evx_parse_events(const ParseCtx *ctx /*rdi*/, const char *begin /*rsi*/,
#                           const char *end /*rdx*/, PartHead *heads /*rcx*/,
#                           Slab *slab /*r8*/)
#   returns the number of Recs written, or UINT64_MAX on raw-ID table overflow
#
# One call per newline-aligned byte range (no header). For every line:
#   1. 64-byte blocks: newline / comma / quote k-masks in one pass. The newline
#      bound truncates the comma and quote masks with bzhi; a virtual newline
#      at the range end handles a last line without '\n'. The final partial
#      block is a fault-suppressing masked load, so nothing past `end` is read.
#   2. Exactly 6 commas, quote count == 2 * wrapped fields, else malformed.
#   3. Empty event_id -> dropped (before dedup).
#   4. Every other row becomes a 32-byte Rec (see evx.h): ingest seconds and
#      40 bits of the event_id hash, a pointer to the event_id bytes, ts_off,
#      rid and price_cat. All validation (user, ts, window, type, product,
#      price, late rule) is evaluated here, once; an invalid copy gets
#      rid = RID_NONE and still competes in dedup (rules: dedup first).
#   4b. The Rec goes straight into its dedup partition ((u32)h >> pshift):
#      the calling worker's open 4 KB chunk of that partition (32-byte header
#      + 127 Recs). A full chunk gets count 127; a new chunk comes from the
#      worker's slab (evx_chunk_alloc, C) and is pushed on the worker's list.
#   5. A valid copy interns its raw user_id in the raw-ID table (16-byte
#      Key16 slots claimed with cmpxchg16b; the slot index is the rid). The
#      probe is delayed through a ring of RING rows: the slot is prefetched
#      when the row is parsed and probed RING rows later, so the table misses
#      overlap with parsing. Links are NOT applied here: P1.5 resolves each
#      distinct raw id once (evx_resolve_raw).
#
# Stack frame (rsp-relative):
#    0 ..  55  comma pointers [7]
#   64 .. 175  field slices [7] (ptr, len) x 16 bytes

        .intel_syntax noprefix
        .text

        .set    F_EID,    64
        .set    F_USER,   80
        .set    F_TS,     96
        .set    F_ING,   112
        .set    F_TYPE,  128
        .set    F_PROD,  144
        .set    F_PRICE, 160
        .set    L_TS,    176
        .set    L_ING,   184
        .set    L_END,   192
        .set    L_PRICE, 200
        .set    L_HEADS, 208            # PartHead[P] of this worker
        .set    L_CNT,   216            # Recs written
        .set    L_H,     224            # event_id hash
        .set    L_SLAB,  232
        .set    L_RN,    240            # entries in the ring
        .set    L_RI,    248            # next ring position
.ifndef PARSE_RING
        .set    PARSE_RING, 16
.endif
        .set    RING,    PARSE_RING     # power of two
        .set    L_RING,  256            # RING x {Rec*, k0, k1, slot}
        .set    L_LC,    L_RING + 32*RING # raw claims not yet added to the shared count
        .set    LC_BATCH, 64            # shared count updated once per 64 claims
        .set    FRAME,   L_LC + 8       # == 8 mod 16: calls aligned

        .globl  evx_parse_events
        .type   evx_parse_events, @function
        .p2align 5
evx_parse_events:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, FRAME
        mov     qword ptr [rsp + L_HEADS], rcx
        mov     qword ptr [rsp + L_SLAB], r8
        mov     qword ptr [rsp + L_CNT], 0
        mov     qword ptr [rsp + L_RN], 0
        mov     qword ptr [rsp + L_RI], 0
        mov     qword ptr [rsp + L_LC], 0
        mov     r12, rdi                # ctx
        mov     r13, rsi                # cursor (line start)
        mov     r14, rdx                # end
        mov     eax, 0x0A
        vpbroadcastb zmm20, eax
        mov     eax, 0x2C
        vpbroadcastb zmm21, eax
        mov     eax, 0x22
        vpbroadcastb zmm22, eax

.Lline:
        cmp     r13, r14
        jae     .Ldone
        mov     rbp, r13                # line start
        mov     r8, r13                 # block pointer
        xor     r9d, r9d                # comma count
        xor     r10d, r10d              # quote count

.Lscan:
        mov     rcx, r14
        sub     rcx, r8                 # remaining bytes
        cmp     rcx, 64
        jb      .Lpartial
        vmovdqu8 zmm16, zmmword ptr [r8]
        vpcmpeqb k1, zmm16, zmm20
        vpcmpeqb k2, zmm16, zmm21
        vpcmpeqb k3, zmm16, zmm22
        kmovq   rax, k1
        kmovq   rdx, k2
        kmovq   r11, k3
        test    rax, rax
        jnz     .Lhas_nl
        popcnt  r11, r11
        add     r10, r11
.Lc_full:
        test    rdx, rdx
        jz      .Lc_full_done
        cmp     r9d, 7
        jae     .Lc_full_skip
        tzcnt   rax, rdx
        add     rax, r8
        mov     qword ptr [rsp + r9*8], rax
.Lc_full_skip:
        inc     r9d
        blsr    rdx, rdx
        jmp     .Lc_full
.Lc_full_done:
        add     r8, 64
        jmp     .Lscan

.Lpartial:
        mov     rax, -1
        bzhi    rax, rax, rcx
        kmovq   k4, rax
        vmovdqu8 zmm16{k4}{z}, zmmword ptr [r8]
        vpcmpeqb k1, zmm16, zmm20
        vpcmpeqb k2, zmm16, zmm21
        vpcmpeqb k3, zmm16, zmm22
        kmovq   rax, k1
        kmovq   rdx, k2
        kmovq   r11, k3
        bts     rax, rcx                # virtual newline at the range end
.Lhas_nl:
        tzcnt   rsi, rax                # newline position in block
        mov     rax, -1
        bzhi    rax, rax, rsi
        and     rdx, rax
        and     r11, rax
        popcnt  r11, r11
        add     r10, r11
.Lc_last:
        test    rdx, rdx
        jz      .Lc_last_done
        cmp     r9d, 7
        jae     .Lc_last_skip
        tzcnt   rax, rdx
        add     rax, r8
        mov     qword ptr [rsp + r9*8], rax
.Lc_last_skip:
        inc     r9d
        blsr    rdx, rdx
        jmp     .Lc_last
.Lc_last_done:
        lea     rax, [r8 + rsi]         # line end (the '\n' or `end`)
        mov     qword ptr [rsp + L_END], rax
        lea     r13, [rax + 1]          # next line

        cmp     r9d, 6
        jne     .Lline                  # wrong field count (also: empty line)

        # ---- cut the 7 fields, unwrapping quotes ---------------------------
        # Branchless and unrolled: ~80% of rows have quoted fields, so the
        # quote tests were unpredictable branches. wrapped = len >= 2 and
        # both ends are '"'. The byte reads stay inside the mapping: for an
        # empty field they hit the neighbouring comma / the previous newline.
        mov     rcx, rbp                # field start
        xor     r11d, r11d              # wrapped fields
        .irp    f, 0,1,2,3,4,5,6
        .if     \f == 6
        mov     rdx, qword ptr [rsp + L_END]
        .else
        mov     rdx, qword ptr [rsp + 8*\f]
        .endif
        mov     rax, rdx
        sub     rax, rcx                # len
        xor     esi, esi
        xor     edi, edi
        xor     r8d, r8d
        cmp     byte ptr [rcx], 0x22
        sete    sil
        cmp     byte ptr [rdx - 1], 0x22
        sete    dil
        cmp     rax, 2
        setae   r8b
        and     esi, edi
        and     esi, r8d                # wrapped
        add     r11d, esi
        sub     rax, rsi
        sub     rax, rsi
        add     rsi, rcx                # value start
        mov     qword ptr [rsp + 64 + 16*\f], rsi
        mov     qword ptr [rsp + 72 + 16*\f], rax
        lea     rcx, [rdx + 1]
        .endr
        add     r11, r11
        cmp     r10, r11
        jne     .Lline                  # stray quote -> malformed

        # ---- event_id ------------------------------------------------------
        mov     rsi, qword ptr [rsp + F_EID + 8]
        test    rsi, rsi
        jz      .Lline                  # empty event_id: dropped before dedup
        mov     rdi, qword ptr [rsp + F_EID]
        call    evx_key16                       # inline key from the hot line
        mov     rdi, rax
        mov     rsi, rdx
        call    evx_khash                       # clobbers rax, r11
        mov     qword ptr [rsp + L_H], rax
        mov     ecx, dword ptr [r12 + 72]       # pshift
        shr     eax, cl                         # partition
        shl     rax, 5
        mov     r8, qword ptr [rsp + L_HEADS]
        add     r8, rax                         # its PartHead
        mov     r15, qword ptr [r8]             # cur
        cmp     r15, qword ptr [r8 + 8]         # end (0/0 = no chunk yet)
        jb      .Lhave_slot
        mov     rdx, qword ptr [r8 + 8]
        test    rdx, rdx
        jz      .Lfirst_chunk
        mov     dword ptr [rdx - 4096], 127     # the old chunk is full
.Lfirst_chunk:
        push    r8
        push    r8                              # keeps rsp 16-byte aligned
        mov     rdi, qword ptr [rsp + 16 + L_SLAB]
        call    evx_chunk_alloc                 # C: clobbers caller-saved + zmm
        pop     r8
        pop     r8
        mov     ecx, 0x0A
        vpbroadcastb zmm20, ecx
        mov     ecx, 0x2C
        vpbroadcastb zmm21, ecx
        mov     ecx, 0x22
        vpbroadcastb zmm22, ecx
        mov     r15, rax
        mov     rcx, rax
        sub     rcx, qword ptr [r12 + 56]
        shr     rcx, 12                         # chunk index
        mov     eax, dword ptr [r8 + 16]
        mov     dword ptr [r15 + 4], eax        # next = old list head
        mov     dword ptr [r8 + 16], ecx        # list head = new chunk
        lea     rax, [r15 + 4096]
        mov     qword ptr [r8 + 8], rax
        add     r15, 32
.Lhave_slot:
        lea     rax, [r15 + 32]
        mov     qword ptr [r8], rax
        inc     qword ptr [rsp + L_CNT]
        mov     rax, qword ptr [rsp + L_H]
        shr     rax, 24
        shl     rax, 48                         # hash bits 24..39 on top
        or      rax, qword ptr [rsp + F_EID]
        mov     qword ptr [r15 + 8], rax        # eptr

        # ---- ingest_ts (always F1 per rules; defensive) --------------------
        mov     rdi, qword ptr [rsp + F_ING]
        mov     rsi, qword ptr [rsp + F_ING + 8]
        lea     rdx, [rsp + L_ING]
        call    evx_parse_ts
        test    eax, eax
        jz      .Lbad_ingest
        # ki = (floor((ingest - ws_ms) / 1000) + 2^39, clamped to 40 bits) << 24 | h24
        mov     rax, qword ptr [rsp + L_ING]
        sub     rax, qword ptr [r12 + 80]
        movabs  rcx, -549755813888000           # -2^39 * 1000
        cmp     rax, rcx
        cmovl   rax, rcx
        movabs  rcx, 549755811888000            # (2^39 - 2) * 1000
        cmp     rax, rcx
        cmovg   rax, rcx
        movabs  rcx, 549755813888000
        add     rax, rcx                        # >= 0
        shr     rax, 3
        movabs  rdx, 0x20C49BA5E353F7CF
        mul     rdx
        shr     rdx, 4                          # / 1000
        shl     rdx, 24
        mov     eax, dword ptr [rsp + L_H]
        and     eax, 0xFFFFFF
        or      rdx, rax
        mov     qword ptr [r15], rdx            # ki

        # ---- validation of this copy (applied only if it wins dedup) -------
        cmp     qword ptr [rsp + F_USER + 8], 0
        je      .Linvalid
        cmp     qword ptr [rsp + F_PROD + 8], 0
        je      .Linvalid
        mov     rdi, qword ptr [rsp + F_TS]
        mov     rsi, qword ptr [rsp + F_TS + 8]
        lea     rdx, [rsp + L_TS]
        call    evx_parse_ts
        test    eax, eax
        jz      .Linvalid
        mov     rax, qword ptr [rsp + L_TS]
        cmp     rax, qword ptr [r12]            # window_start <= ts
        jl      .Linvalid
        cmp     rax, qword ptr [r12 + 8]        # ts < window_end
        jge     .Linvalid
        mov     rdx, qword ptr [rsp + L_ING]
        sub     rdx, rax
        cmp     rdx, 86400000                   # late: ingest - ts > 24 h
        jg      .Linvalid
        sub     rax, qword ptr [r12]
        mov     qword ptr [r15 + 16], rax       # ts_off

        # event_type: compare the whole value as one 4- or 8-byte integer
        mov     rdi, qword ptr [rsp + F_TYPE]
        mov     rsi, qword ptr [rsp + F_TYPE + 8]
        cmp     rsi, 4
        jne     .Ltype8
        mov     eax, dword ptr [rdi]
        cmp     eax, 0x77656976                 # "view"
        je      .Lnonpurchase
        cmp     eax, 0x74726163                 # "cart"
        je      .Lnonpurchase
        jmp     .Linvalid
.Ltype8:
        cmp     rsi, 8
        jne     .Linvalid
        movabs  rdx, 0x6573616863727570         # "purchase"
        cmp     qword ptr [rdi], rdx
        jne     .Linvalid

        # purchase price: ASCII digits only, 1 ..= 10,000,000, leading zeros ok
        mov     rdi, qword ptr [rsp + F_PRICE]
        mov     rsi, qword ptr [rsp + F_PRICE + 8]
        test    rsi, rsi
        jz      .Linvalid
        add     rsi, rdi
        xor     eax, eax
.Lprice:
        movzx   ecx, byte ptr [rdi]
        sub     ecx, 0x30
        cmp     ecx, 9
        ja      .Linvalid
        imul    eax, eax, 10
        add     eax, ecx
        cmp     eax, 10000000
        ja      .Linvalid
        inc     rdi
        cmp     rdi, rsi
        jb      .Lprice
        test    eax, eax
        jz      .Linvalid
        mov     dword ptr [rsp + L_PRICE], eax

        # category (only purchases need one)
        mov     eax, dword ptr [r12 + 48]       # unknown_cat
        mov     dword ptr [r15 + 28], eax
        cmp     qword ptr [r12 + 32], 0
        je      .Lcat_done
        mov     rdi, qword ptr [rsp + F_PROD]
        mov     rsi, qword ptr [rsp + F_PROD + 8]
        call    evx_key16
        mov     rcx, rdx
        mov     rdx, rax
        mov     rdi, qword ptr [r12 + 32]
        mov     rsi, qword ptr [r12 + 40]
        call    evx_lookup
        test    rax, rax
        jz      .Lcat_done
        mov     eax, dword ptr [rax + 16]       # val.k0 = category id
        mov     dword ptr [r15 + 28], eax
.Lcat_done:
        mov     eax, dword ptr [r15 + 28]
        shl     eax, 24
        or      eax, dword ptr [rsp + L_PRICE]
        mov     dword ptr [r15 + 28], eax
        jmp     .Lvalid

.Lnonpurchase:
        mov     dword ptr [r15 + 28], 0

        # ---- valid: queue the raw user id for interning (ring) --------------
.Lvalid:
        mov     rdi, qword ptr [rsp + F_USER]
        mov     rsi, qword ptr [rsp + F_USER + 8]
        call    evx_key16                       # never null for a non-empty id
        mov     rcx, qword ptr [rsp + L_RI]
        mov     rbx, rcx
        shl     rbx, 5
        lea     rbx, [rsp + L_RING + rbx]       # ring entry
        mov     qword ptr [rbx], r15
        mov     qword ptr [rbx + 8], rax
        mov     qword ptr [rbx + 16], rdx
        mov     rdi, rax
        mov     rsi, rdx
        call    evx_khash
        and     rax, qword ptr [r12 + 24]       # raw mask
        mov     qword ptr [rbx + 24], rax
        shl     rax, 4
        add     rax, qword ptr [r12 + 16]
        prefetchw [rax]
        mov     rcx, qword ptr [rsp + L_RI]
        inc     rcx
        and     rcx, RING - 1
        mov     qword ptr [rsp + L_RI], rcx
        mov     rax, qword ptr [rsp + L_RN]
        inc     rax
        cmp     rax, RING
        jb      .Lring_keep
        # ring full: intern the oldest entry (the next one to be overwritten)
        dec     rax
        mov     qword ptr [rsp + L_RN], rax
        shl     rcx, 5
        lea     rbx, [rsp + L_RING + rcx]
        call    .Lintern
        test    eax, eax
        jnz     .Loverflow
        jmp     .Lline
.Lring_keep:
        mov     qword ptr [rsp + L_RN], rax
        jmp     .Lline

.Lbad_ingest:
        movabs  rax, 0xFFFFFFFFFF000000         # never beats a real copy
        mov     ecx, dword ptr [rsp + L_H]
        and     ecx, 0xFFFFFF
        or      rax, rcx
        mov     qword ptr [r15], rax
.Linvalid:
        mov     dword ptr [r15 + 24], -1        # rid = RID_NONE: invalid copy
        mov     qword ptr [r15 + 16], 0
        mov     dword ptr [r15 + 28], 0
        jmp     .Lline

.Ldone:
        # drain the ring, oldest first
        mov     rax, qword ptr [rsp + L_RN]
        test    rax, rax
        jz      .Lflush
        dec     rax
        mov     qword ptr [rsp + L_RN], rax
        mov     rcx, qword ptr [rsp + L_RI]
        sub     rcx, rax
        dec     rcx
        and     rcx, RING - 1
        shl     rcx, 5
        lea     rbx, [rsp + L_RING + rcx]
        call    .Lintern
        test    eax, eax
        jnz     .Loverflow
        jmp     .Ldone
.Lflush:
        mov     rcx, qword ptr [rsp + L_LC]     # flush the local claim count
        test    rcx, rcx
        jz      .Lret
        mov     rax, qword ptr [r12 + 64]
        mov     rdx, rcx
        lock xadd qword ptr [rax], rcx
        add     rcx, rdx
        cmp     rcx, qword ptr [r12 + 88]
        jbe     .Lret
.Loverflow:
        mov     qword ptr [rsp + L_CNT], -1
.Lret:
        mov     rax, qword ptr [rsp + L_CNT]
        add     rsp, FRAME
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret

# ---------------------------------------------------------------------------
# local: intern the ring entry at rbx {Rec*, k0, k1, home slot} in the raw-ID
# table and store the slot as the Rec's rid. eax = 0, or 1 on overflow.
# Uses r12 (ctx). Clobbers rax, rcx, rdx, rsi, rdi, r8 - r11, rbx.
# ---------------------------------------------------------------------------
.Lintern:
        mov     r8, qword ptr [rbx]             # Rec*
        mov     rsi, qword ptr [rbx + 8]        # k0
        mov     r10, qword ptr [rbx + 16]       # k1
        mov     r9, qword ptr [rbx + 24]        # slot
        mov     r11, qword ptr [r12 + 16]       # raw table
.Li_probe:
        mov     rdi, r9
        shl     rdi, 4
        add     rdi, r11
        mov     rcx, qword ptr [rdi + 8]
        test    rcx, rcx
        jnz     .Li_cmp
        # claim the empty slot: {0,0} -> {k0, k1}
        mov     rbx, rsi
        mov     rcx, r10
        xor     eax, eax
        xor     edx, edx
        lock cmpxchg16b xmmword ptr [rdi]
        jnz     .Li_probe                       # lost the race: re-read the slot
        # claims are counted locally and added to the shared count in batches
        # (one contended lock xadd per LC_BATCH claims); the limit is half the
        # table, so the slack of nw * LC_BATCH claims stays far from full
        mov     rax, qword ptr [rsp + 8 + L_LC]  # (+8: our return address)
        inc     rax
        mov     qword ptr [rsp + 8 + L_LC], rax
        cmp     rax, LC_BATCH
        jb      .Li_found
        mov     qword ptr [rsp + 8 + L_LC], 0
        mov     rcx, rax
        mov     rax, qword ptr [r12 + 64]       # &raw_claimed
        lock xadd qword ptr [rax], rcx
        add     rcx, LC_BATCH
        cmp     rcx, qword ptr [r12 + 88]       # raw_limit
        ja      .Li_over
        jmp     .Li_found
.Li_cmp:
        cmp     rcx, r10
        jne     .Li_next
        mov     rax, r10
        shr     rax, 56
        cmp     eax, 0xFF
        je      .Li_long
        cmp     qword ptr [rdi], rsi            # short key: k0 decides
        je      .Li_found
        jmp     .Li_next
.Li_long:
        push    rdi
        push    rsi
        mov     rdx, rsi
        mov     rcx, r10
        mov     rsi, r10
        mov     rdi, qword ptr [rdi]
        call    evx_keq                         # keeps every GPR but rax / r11
        pop     rsi
        pop     rdi
        mov     r11, qword ptr [r12 + 16]
        test    eax, eax
        jnz     .Li_found
.Li_next:
        inc     r9
        and     r9, qword ptr [r12 + 24]
        jmp     .Li_probe
.Li_found:
        mov     dword ptr [r8 + 24], r9d        # rid
        xor     eax, eax
        ret
.Li_over:
        mov     eax, 1
        ret
        .size   evx_parse_events, .-evx_parse_events

        .section .note.GNU-stack,"",@progbits
