# evx_dedup.s - global event_id dedup + winner emission, person grouping.
# (GNU as, Intel syntax)
#
# The parser writes every 32-byte Rec into its event_id-hash partition (4 KB
# chunks, see evx_parse.s) and has already resolved each valid copy's raw
# user id to a rid; P1.5 turned rids into dense person ids (remap[]).
# evx_dedup_emit then handles one partition at a time in one call:
#   insert  every Rec of every worker's chunk list into a private,
#           cache-resident table (no atomics); per event_id the smallest
#           ingest stays, ties keep the resident copy (equal-ingest copies are
#           identical). Equal 40-bit hashes are confirmed on the event_id
#           bytes in the input (evx_eid_eq).
#   emit    walk the table: every valid winner becomes a 16-byte Win
#           {ts_off, dense pid, price_cat} in the calling worker's open Win
#           chunk of its person range (pid >> dshift); Win chunks are the
#           worker's consumed Rec chunks (cache-warm) or fresh slab chunks.
#   recycle the partition's Rec chunks go on the worker's free list.

        .intel_syntax noprefix
        .text

        .set    WCHUNK_WINS, 255        # Win chunk: 16-byte header + 255 Wins

# ---------------------------------------------------------------------------
# local .Leq: rsi, rdi = two event_id values (each ends at ',' or '"');
# r11d = 1 if equal. Clobbers rcx, rsi, rdi. Byte loop: only copies of
# one event (2-4% of rows) and 40-bit hash collisions get here.
# ---------------------------------------------------------------------------
        .p2align 4
.Leq:
        movzx   ecx, byte ptr [rsi]
        cmp     cl, byte ptr [rdi]
        jne     .Leq_d
        cmp     ecx, 0x2C
        je      .Leq_yes
        cmp     ecx, 0x22
        je      .Leq_yes
        inc     rsi
        inc     rdi
        jmp     .Leq
.Leq_d:
        cmp     ecx, 0x2C
        je      .Leq_ta
        cmp     ecx, 0x22
        jne     .Leq_no
.Leq_ta:
        movzx   ecx, byte ptr [rdi]
        cmp     ecx, 0x2C
        je      .Leq_yes
        cmp     ecx, 0x22
        je      .Leq_yes
.Leq_no:
        xor     r11d, r11d
        ret
.Leq_yes:
        mov     r11d, 1
        ret

# int evx_eid_eq(const char *a /*rdi*/, const char *b /*rsi*/)
        .globl  evx_eid_eq
        .type   evx_eid_eq, @function
        .p2align 4
evx_eid_eq:
        call    .Leq
        mov     eax, r11d
        ret
        .size   evx_eid_eq, .-evx_eid_eq

# ---------------------------------------------------------------------------
# uint64_t evx_dedup_emit(DedupCtx *ctx /*rdi*/, uint32_t part /*esi*/)
# The table (ctx->table, tsize + 64 zeroed slots) holds tagged Rec pointers:
# Rec* | hash bits 24..39 << 48, so most probe mismatches need no Rec load.
# Home slot = ((h32 << pbits) mod 2^32 * tsize) >> 32 (the hash bits below
# the partition bits).
# Frame: [0] part, [8] worker w, [16] Wins, [24] next chunk index.
# ---------------------------------------------------------------------------
        .set    D_TABLE, 0
        .set    D_TSIZE, 8
        .set    D_ARENA, 16
        .set    D_HEADS, 24
        .set    D_NW,    32
        .set    D_P,     36
        .set    D_PBITS, 40
        .set    D_DSH,   44
        .set    D_REMAP, 48
        .set    D_WHEADS, 56
        .set    D_FREE,  64
        .set    D_SLAB,  72
        .set    DFRAME,  40             # == 8 mod 16: calls aligned
.ifndef EM_PF1
        .set    EM_PF1, 48              # table is zeroed 64 slots past tsize
.endif
.ifndef EM_PF2
        .set    EM_PF2, 24
.endif

        .globl  evx_dedup_emit
        .type   evx_dedup_emit, @function
        .p2align 5
evx_dedup_emit:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, DFRAME
        mov     rbp, rdi
        mov     dword ptr [rsp], esi
        mov     qword ptr [rsp + 16], 0
        mov     rbx, qword ptr [rbp + D_TABLE]
        mov     r12, qword ptr [rbp + D_TSIZE]
        mov     r13, qword ptr [rbp + D_ARENA]
        mov     r10d, dword ptr [rbp + D_PBITS]

        # ---- insert -----------------------------------------------------------
        mov     qword ptr [rsp + 8], 0
.Lin_w:
        mov     eax, dword ptr [rsp + 8]
        cmp     eax, dword ptr [rbp + D_NW]
        jae     .Lemit
        imul    eax, dword ptr [rbp + D_P]
        add     eax, dword ptr [rsp]
        shl     rax, 5
        add     rax, qword ptr [rbp + D_HEADS]
        mov     eax, dword ptr [rax + 16]       # list head
.Lin_chunk:
        cmp     eax, -1
        je      .Lin_wnext
        mov     r14, rax
        shl     r14, 12
        add     r14, r13                        # chunk
        mov     ecx, dword ptr [r14 + 4]
        mov     qword ptr [rsp + 24], rcx       # next
        cmp     ecx, -1
        je      .Lin_nopf
        shl     rcx, 12
        prefetcht0 [r13 + rcx]
        prefetcht0 [r13 + rcx + 64]
.Lin_nopf:
        mov     r15d, dword ptr [r14]           # count
        shl     r15, 5
        add     r14, 32
        add     r15, r14                        # Rec end
.Lin_rec:
        cmp     r14, r15
        jae     .Lin_cnext
        mov     r8, qword ptr [r14]             # ki
        mov     r9, qword ptr [r14 + 8]         # eptr
        mov     eax, r8d
        and     eax, 0xFFFFFF
        mov     rcx, r9
        shr     rcx, 48
        shl     ecx, 24
        or      eax, ecx                        # h32
        shlx    eax, eax, r10d
        imul    rax, r12
        shr     rax, 32                         # home slot
        mov     rdx, r9
        shr     rdx, 48
        shl     rdx, 48
        or      rdx, r14                        # tagged Rec*
.Lin_probe:
        mov     rcx, qword ptr [rbx + rax*8]
        test    rcx, rcx
        jnz     .Lin_occ
        mov     qword ptr [rbx + rax*8], rdx
        add     r14, 32
        jmp     .Lin_rec
.Lin_occ:
        mov     rsi, rcx
        xor     rsi, rdx
        shr     rsi, 48
        jnz     .Lin_pnext                      # tag differs
        shl     rcx, 16
        shr     rcx, 16                         # resident Rec
        mov     esi, dword ptr [rcx]
        xor     esi, r8d
        and     esi, 0xFFFFFF
        jnz     .Lin_pnext                      # low hash bits differ
        mov     rsi, qword ptr [rcx + 8]
        shl     rsi, 16
        shr     rsi, 16
        mov     rdi, r9
        shl     rdi, 16
        shr     rdi, 16
        call    .Leq
        test    r11d, r11d
        jz      .Lin_pnext                      # a different event_id
        mov     rcx, qword ptr [rbx + rax*8]
        shl     rcx, 16
        shr     rcx, 16
        cmp     r8, qword ptr [rcx]
        jae     .Lin_keep                       # resident is earlier (or tie)
        mov     qword ptr [rbx + rax*8], rdx
.Lin_keep:
        add     r14, 32
        jmp     .Lin_rec
.Lin_pnext:
        inc     rax
        cmp     rax, r12
        jb      .Lin_probe
        xor     eax, eax
        jmp     .Lin_probe
.Lin_cnext:
        mov     rax, qword ptr [rsp + 24]
        jmp     .Lin_chunk
.Lin_wnext:
        inc     dword ptr [rsp + 8]
        jmp     .Lin_w

        # ---- emit: walk the table ---------------------------------------------
        # lookahead: Rec of slot i+EM_PF1 prefetched; remap entry of slot i+EM_PF2
.Lemit:
        xor     r14d, r14d
        mov     r13, qword ptr [rbp + D_REMAP]
.Lem:
        cmp     r14, r12
        jae     .Lrecycle
        mov     rcx, qword ptr [rbx + r14*8 + 8*EM_PF1]
        shl     rcx, 16
        shr     rcx, 16
        prefetcht0 [rcx]
        mov     rcx, qword ptr [rbx + r14*8 + 8*EM_PF2]
        test    rcx, rcx
        jz      .Lem_nopf
        shl     rcx, 16
        shr     rcx, 16
        mov     ecx, dword ptr [rcx + 24]
        prefetcht0 [r13 + rcx*4]
.Lem_nopf:
        mov     rcx, qword ptr [rbx + r14*8]
        inc     r14
        test    rcx, rcx
        jz      .Lem
        shl     rcx, 16
        shr     rcx, 16                         # winner Rec
        mov     eax, dword ptr [rcx + 24]       # rid
        cmp     eax, -1
        je      .Lem                            # kept copy invalid: dropped
        mov     eax, dword ptr [r13 + rax*4]    # dense pid
        mov     edx, dword ptr [rbp + D_DSH]
        shrx    r8d, eax, edx                   # person range
        shl     r8, 5
        add     r8, qword ptr [rbp + D_WHEADS]  # its WinHead
        mov     r10, qword ptr [r8]             # cur
        cmp     r10, qword ptr [r8 + 8]         # end (0/0 = no chunk yet)
        jb      .Lem_slot
        mov     rdx, qword ptr [r8 + 8]
        test    rdx, rdx
        jz      .Lem_first
        mov     dword ptr [rdx - 4096], WCHUNK_WINS   # the old chunk is full
.Lem_first:
        mov     r10, qword ptr [rbp + D_FREE]   # recycled Rec chunk?
        test    r10, r10
        jz      .Lem_fresh
        mov     rdx, qword ptr [r10]
        mov     qword ptr [rbp + D_FREE], rdx   # pop
        jmp     .Lem_link
.Lem_fresh:
        push    rax
        push    rcx
        push    r8
        push    r8
        mov     rdi, qword ptr [rbp + D_SLAB]
        call    evx_chunk_alloc
        mov     r10, rax
        pop     r8
        pop     r8
        pop     rcx
        pop     rax
.Lem_link:
        mov     rdx, r10
        sub     rdx, qword ptr [rbp + D_ARENA]
        shr     rdx, 12                         # chunk index
        mov     esi, dword ptr [r8 + 16]
        mov     dword ptr [r10 + 4], esi        # next = old list head
        mov     dword ptr [r8 + 16], edx        # list head = new chunk
        lea     rsi, [r10 + 4096]
        mov     qword ptr [r8 + 8], rsi
        add     r10, 16
.Lem_slot:
        lea     rsi, [r10 + 16]
        mov     qword ptr [r8], rsi
        inc     dword ptr [r8 + 20]             # Wins of this range (this worker)
        mov     rsi, qword ptr [rcx + 16]       # ts_off
        mov     qword ptr [r10], rsi
        mov     dword ptr [r10 + 8], eax        # dense pid
        mov     esi, dword ptr [rcx + 28]       # price_cat
        mov     dword ptr [r10 + 12], esi
        inc     qword ptr [rsp + 16]
        jmp     .Lem

        # ---- recycle the partition's Rec chunks --------------------------------
.Lrecycle:
        mov     r13, qword ptr [rbp + D_ARENA]
        mov     rdx, qword ptr [rbp + D_FREE]
        xor     r9d, r9d
.Lrc_w:
        cmp     r9d, dword ptr [rbp + D_NW]
        jae     .Lrc_done
        mov     eax, r9d
        imul    eax, dword ptr [rbp + D_P]
        add     eax, dword ptr [rsp]
        shl     rax, 5
        add     rax, qword ptr [rbp + D_HEADS]
        mov     eax, dword ptr [rax + 16]
.Lrc_c:
        cmp     eax, -1
        je      .Lrc_wnext
        mov     rcx, rax
        shl     rcx, 12
        add     rcx, r13
        mov     eax, dword ptr [rcx + 4]        # next (read before the push)
        mov     qword ptr [rcx], rdx
        mov     rdx, rcx
        jmp     .Lrc_c
.Lrc_wnext:
        inc     r9d
        jmp     .Lrc_w
.Lrc_done:
        mov     qword ptr [rbp + D_FREE], rdx
        mov     rax, qword ptr [rsp + 16]
        add     rsp, DFRAME
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   evx_dedup_emit, .-evx_dedup_emit

# ---------------------------------------------------------------------------
# void evx_count_range(const char *warena /*rdi*/, uint32_t first /*esi*/,
#                      uint64_t *cnt /*rdx*/)
# Events per dense person for one worker's Win list of one person range:
# cnt[pid]++ (the range owns those counters; plain increments).
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
        mov     eax, dword ptr [r11 + 8]        # dense id
        inc     qword ptr [rdx + rax*8]
        add     r11, 16
        jmp     .Lcr_next
.Lcr_done:
        ret
        .size   evx_count_range, .-evx_count_range

# ---------------------------------------------------------------------------
# void evx_group_range(const char *warena /*rdi*/, uint32_t first /*esi*/,
#                      uint64_t *cursor /*rdx*/, Ev *dst /*rcx*/)
# Counting-sort placement of one worker's Win chunk list of ONE person range:
# dst[cursor[pid]++] = event. The range owns its persons' cursors and Ev
# slice, so plain increments suffice, and everything stays in L1/L2.
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
        mov     eax, dword ptr [r11 + 8]        # dense id
        mov     r9, qword ptr [rdx + rax*8]
        inc     qword ptr [rdx + rax*8]
        shl     r9, 4
        add     r9, rcx
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
