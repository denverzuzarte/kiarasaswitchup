# evx_click.s - click engine and attribution (GNU as, Intel syntax).
#
#   evx_canon           raw user Key16 -> canonical person Key16 (shared with
#                       evx_collect so events and clicks agree bit for bit)
#   evx_memcmp          byte-wise order (click_id tie-break)
#   evx_sort16          16-byte records by u64 key (insertion / LSD radix)
#   evx_parse_clicks    batch click parse + validation + person resolution
#   evx_scatter_clicks  counting-sort placement by dense person
#   evx_attribute       per person: sessions, credit, assists, campaign/day
#
# Only zmm16+ are used (no vzeroupper needed), as in the rest of the engine.

        .intel_syntax noprefix
        .text

        .set    W7, 604800000           # 7 days in ms
        .set    GAP, 1800000            # 30 minutes in ms

# ---------------------------------------------------------------------------
# evx_canon (internal ABI)
#   in : rdi:rsi = raw user id as Key16, rdx = link slots (NULL = no links),
#        rcx = link mask
#   out: rax:rdx = canonical person name as Key16 (the link component's
#        smallest id, or the raw id itself)
#   clobbers rcx, rsi, rdi, r8 - r11 and what evx_lookup clobbers
# ---------------------------------------------------------------------------
        .globl  evx_canon
        .type   evx_canon, @function
        .p2align 5
evx_canon:
        test    rdx, rdx
        jz      .Lcn_self
        push    rdi
        push    rsi
        mov     r8, rdx
        mov     r9, rcx
        mov     rdx, rdi
        mov     rcx, rsi
        mov     rdi, r8
        mov     rsi, r9
        call    evx_lookup
        pop     rsi
        pop     rdi
        test    rax, rax
        jz      .Lcn_self
        mov     rdx, qword ptr [rax + 24]
        mov     rax, qword ptr [rax + 16]
        ret
.Lcn_self:
        mov     rax, rdi
        mov     rdx, rsi
        ret
        .size   evx_canon, .-evx_canon

# ---------------------------------------------------------------------------
# int evx_memcmp(const void *a /*rdi*/, size_t alen /*rsi*/,
#                const void *b /*rdx*/, size_t blen /*rcx*/)
#   64-byte masked compares; the first differing byte is found with tzcnt of
#   the not-equal mask and compared unsigned. Preserves every GPR except
#   rax / r11 (clobbers k1, k2, zmm16, zmm17).
# ---------------------------------------------------------------------------
        .globl  evx_memcmp
        .type   evx_memcmp, @function
        .p2align 5
evx_memcmp:
        push    rdi
        push    rdx
        push    r8
        push    r9
        mov     r8, rsi
        cmp     rcx, r8
        cmovb   r8, rcx                 # common length
.Lmc_loop:
        cmp     r8, 64
        jb      .Lmc_tail
        vmovdqu8 zmm16, zmmword ptr [rdi]
        vmovdqu8 zmm17, zmmword ptr [rdx]
        vpcmpb  k2, zmm16, zmm17, 4
        kmovq   r11, k2
        test    r11, r11
        jnz     .Lmc_diff
        add     rdi, 64
        add     rdx, 64
        sub     r8, 64
        jmp     .Lmc_loop
.Lmc_tail:
        mov     r11, -1
        bzhi    r11, r11, r8
        kmovq   k1, r11
        vmovdqu8 zmm16{k1}{z}, zmmword ptr [rdi]
        vmovdqu8 zmm17{k1}{z}, zmmword ptr [rdx]
        vpcmpb  k2, zmm16, zmm17, 4
        kmovq   r11, k2
        test    r11, r11
        jnz     .Lmc_diff
        xor     eax, eax
        cmp     rsi, rcx
        je      .Lmc_out
        mov     eax, 1
        mov     r11d, -1
        cmovb   eax, r11d               # a is a proper prefix of b
        jmp     .Lmc_out
.Lmc_diff:
        tzcnt   r11, r11
        movzx   eax, byte ptr [rdi + r11]
        movzx   r9d, byte ptr [rdx + r11]
        sub     eax, r9d
.Lmc_out:
        pop     r9
        pop     r8
        pop     rdx
        pop     rdi
        ret
        .size   evx_memcmp, .-evx_memcmp

# ---------------------------------------------------------------------------
# void evx_sort16(void *a /*rdi*/, uint64_t n /*rsi*/, void *scratch /*rdx*/,
#                 uint32_t passes /*ecx*/, uint32_t *hist /*r8*/)
#   Same algorithm as the events sort: insertion sort for n <= 32, else LSD
#   radix on 8-bit digits with every histogram built in one read, skipping
#   digits that are constant across the keys.
# ---------------------------------------------------------------------------
        .globl  evx_sort16
        .type   evx_sort16, @function
        .p2align 5
evx_sort16:
        cmp     rsi, 32
        ja      .Ls_radix
        mov     r8d, 1
.Ls_outer:
        cmp     r8, rsi
        jae     .Ls_ret
        mov     r9, r8
        shl     r9, 4
        add     r9, rdi
        vmovdqu64 xmm16, xmmword ptr [r9]
        mov     r10, qword ptr [r9]
.Ls_inner:
        cmp     r9, rdi
        jbe     .Ls_place
        cmp     qword ptr [r9 - 16], r10
        jbe     .Ls_place
        vmovdqu64 xmm17, xmmword ptr [r9 - 16]
        vmovdqu64 xmmword ptr [r9], xmm17
        sub     r9, 16
        jmp     .Ls_inner
.Ls_place:
        vmovdqu64 xmmword ptr [r9], xmm16
        inc     r8
        jmp     .Ls_outer
.Ls_ret:
        ret
.Ls_radix:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        mov     r12, rdi                # src
        mov     r13, rdx                # dst
        mov     r14, rsi                # n
        mov     ebp, ecx                # passes
        mov     r15, r8                 # hist
        mov     rbx, rdi                # original array
        mov     rdi, r15
        mov     ecx, ebp
        shl     ecx, 8
        xor     eax, eax
        rep stosd
        mov     rdi, r12
        mov     rsi, r14
        shl     rsi, 4
        add     rsi, rdi
.Ls_hist:
        mov     rax, qword ptr [rdi]
        xor     ecx, ecx
.Ls_hd:
        movzx   edx, al
        mov     r8d, ecx
        shl     r8d, 8
        add     edx, r8d
        inc     dword ptr [r15 + rdx*4]
        shr     rax, 8
        inc     ecx
        cmp     ecx, ebp
        jb      .Ls_hd
        add     rdi, 16
        cmp     rdi, rsi
        jb      .Ls_hist
        xor     ecx, ecx
.Ls_pass:
        cmp     ecx, ebp
        jae     .Ls_fin
        mov     r9d, ecx
        shl     r9d, 10
        add     r9, r15
        mov     rax, qword ptr [r12]
        mov     edx, ecx
        shl     edx, 3
        push    rcx
        mov     ecx, edx
        shr     rax, cl
        pop     rcx
        movzx   eax, al
        cmp     dword ptr [r9 + rax*4], r14d
        je      .Ls_skip
        xor     eax, eax
        xor     r8d, r8d
.Ls_pref:
        mov     r10d, dword ptr [r9 + r8*4]
        mov     dword ptr [r9 + r8*4], eax
        add     eax, r10d
        inc     r8d
        cmp     r8d, 256
        jb      .Ls_pref
        mov     rdi, r12
        mov     rsi, r14
        shl     rsi, 4
        add     rsi, rdi
        push    rcx
        shl     ecx, 3
.Ls_sc:
        mov     rax, qword ptr [rdi]
        shr     rax, cl
        movzx   edx, al
        mov     r8d, dword ptr [r9 + rdx*4]
        inc     dword ptr [r9 + rdx*4]
        shl     r8, 4
        vmovdqu64 xmm16, xmmword ptr [rdi]
        vmovdqu64 xmmword ptr [r13 + r8], xmm16
        add     rdi, 16
        cmp     rdi, rsi
        jb      .Ls_sc
        pop     rcx
        xchg    r12, r13
.Ls_skip:
        inc     ecx
        jmp     .Ls_pass
.Ls_fin:
        cmp     r12, rbx
        je      .Ls_out
        mov     rsi, r12
        mov     rdi, rbx
        mov     rcx, r14
        shl     rcx, 1
        rep movsq
.Ls_out:
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   evx_sort16, .-evx_sort16

# ---------------------------------------------------------------------------
# uint64_t evx_parse_clicks(const ClickParseCtx *ctx /*rdi*/, const char *begin /*rsi*/,
#                           const char *end /*rdx*/, ClickCand *out /*rcx*/)
# Same 64-byte block scanner and quote rule as evx_parse_events, 4 fields.
# Every failing row is dropped here (copies of a click are identical, so no
# dedup is needed). Checks run cheap-first, so a dropped row never pays for
# the link-table and person-table probes:
#   field count / quotes -> click_id, user_id non-empty -> campaign lookup
#   (small, cache resident) -> ts parse + click window -> canonical person ->
#   person table (no surviving events = no sessions = drop).
# ---------------------------------------------------------------------------
        .set    K_CID,   64
        .set    K_USER,  80
        .set    K_CAMP,  96
        .set    K_TS,   112
        .set    K_TSV,  128
        .set    K_END,  136
        .set    K_N,    240             # rows in the current batch
        .set    K_I,    248             # stage loop index
        .set    K_B0,   256             # [64] key k0 (user, then person)
        .set    K_B1,   768             # [64] key k1
        .set    K_BH,   1280            # [64] person slot (-1 = drop)
        .set    KBATCH, 64
        .set    KFRAME, 1800

        .globl  evx_parse_clicks
        .type   evx_parse_clicks, @function
        .p2align 5
evx_parse_clicks:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, KFRAME
        mov     r12, rdi                # ctx
        mov     r13, rsi                # line start
        mov     r14, rdx                # end
        mov     r15, rcx                # out cursor
        mov     rbx, rcx                # out base
        mov     qword ptr [rsp + K_N], 0
        mov     eax, 0x0A
        vpbroadcastb zmm20, eax
        mov     eax, 0x2C
        vpbroadcastb zmm21, eax
        mov     eax, 0x22
        vpbroadcastb zmm22, eax
.Lk_line:
        cmp     r13, r14
        jae     .Lk_done
        mov     rbp, r13
        mov     r8, r13
        xor     r9d, r9d                # commas
        xor     r10d, r10d              # quotes
.Lk_scan:
        mov     rcx, r14
        sub     rcx, r8
        cmp     rcx, 64
        jb      .Lk_partial
        vmovdqu8 zmm16, zmmword ptr [r8]
        vpcmpeqb k1, zmm16, zmm20
        vpcmpeqb k2, zmm16, zmm21
        vpcmpeqb k3, zmm16, zmm22
        kmovq   rax, k1
        kmovq   rdx, k2
        kmovq   r11, k3
        test    rax, rax
        jnz     .Lk_has_nl
        popcnt  r11, r11
        add     r10, r11
.Lk_cf:
        test    rdx, rdx
        jz      .Lk_cf_done
        cmp     r9d, 4
        jae     .Lk_cf_skip
        tzcnt   rax, rdx
        add     rax, r8
        mov     qword ptr [rsp + r9*8], rax
.Lk_cf_skip:
        inc     r9d
        blsr    rdx, rdx
        jmp     .Lk_cf
.Lk_cf_done:
        add     r8, 64
        jmp     .Lk_scan
.Lk_partial:
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
        bts     rax, rcx
.Lk_has_nl:
        tzcnt   rsi, rax
        mov     rax, -1
        bzhi    rax, rax, rsi
        and     rdx, rax
        and     r11, rax
        popcnt  r11, r11
        add     r10, r11
.Lk_cl:
        test    rdx, rdx
        jz      .Lk_cl_done
        cmp     r9d, 4
        jae     .Lk_cl_skip
        tzcnt   rax, rdx
        add     rax, r8
        mov     qword ptr [rsp + r9*8], rax
.Lk_cl_skip:
        inc     r9d
        blsr    rdx, rdx
        jmp     .Lk_cl
.Lk_cl_done:
        lea     rax, [r8 + rsi]
        mov     qword ptr [rsp + K_END], rax
        lea     r13, [rax + 1]
        cmp     r9d, 3
        jne     .Lk_line
        # cut the 4 fields: branchless quote unwrap (see evx_parse.s)
        mov     rcx, rbp
        xor     r11d, r11d
        .irp    f, 0,1,2,3
        .if     \f == 3
        mov     rdx, qword ptr [rsp + K_END]
        .else
        mov     rdx, qword ptr [rsp + 8*\f]
        .endif
        mov     rax, rdx
        sub     rax, rcx                # len
        xor     esi, esi
        xor     r8d, r8d
        xor     r9d, r9d
        cmp     byte ptr [rcx], 0x22
        sete    sil
        cmp     byte ptr [rdx - 1], 0x22
        sete    r8b
        cmp     rax, 2
        setae   r9b
        and     esi, r8d
        and     esi, r9d                # wrapped
        add     r11d, esi
        sub     rax, rsi
        sub     rax, rsi
        add     rsi, rcx
        mov     qword ptr [rsp + 64 + 16*\f], rsi
        mov     qword ptr [rsp + 72 + 16*\f], rax
        lea     rcx, [rdx + 1]
        .endr
        add     r11, r11
        cmp     r10, r11
        jne     .Lk_line

        # click_id and user_id non-empty
        cmp     qword ptr [rsp + K_CID + 8], 0
        je      .Lk_line
        cmp     qword ptr [rsp + K_USER + 8], 0
        je      .Lk_line
        # campaign must be in campaigns.csv (exact bytes)
        cmp     qword ptr [rsp + K_CAMP + 8], 0
        je      .Lk_line
        mov     rdi, qword ptr [rsp + K_CAMP]
        mov     rsi, qword ptr [rsp + K_CAMP + 8]
        call    evx_key16
        mov     rcx, rdx
        mov     rdx, rax
        mov     rdi, qword ptr [r12 + 32]
        mov     rsi, qword ptr [r12 + 40]
        call    evx_lookup
        test    rax, rax
        jz      .Lk_line
        mov     eax, dword ptr [rax + 16]       # val.k0 = campaign id
        mov     dword ptr [r15 + 12], eax
        # ts valid and in the click window [cw_start, cw_end)
        mov     rdi, qword ptr [rsp + K_TS]
        mov     rsi, qword ptr [rsp + K_TS + 8]
        lea     rdx, [rsp + K_TSV]
        call    evx_parse_ts
        test    eax, eax
        jz      .Lk_line
        mov     rax, qword ptr [rsp + K_TSV]
        cmp     rax, qword ptr [r12]
        jl      .Lk_line
        cmp     rax, qword ptr [r12 + 8]
        jge     .Lk_line
        sub     rax, qword ptr [r12]
        mov     qword ptr [r15], rax            # k
        # ---- valid row: stage it in the batch; resolve the person later ----
        mov     rdi, qword ptr [rsp + K_USER]
        mov     rsi, qword ptr [rsp + K_USER + 8]
        call    evx_key16                       # from the hot line
        mov     rcx, qword ptr [rsp + K_N]
        mov     qword ptr [rsp + K_B0 + rcx*8], rax
        mov     qword ptr [rsp + K_B1 + rcx*8], rdx
        mov     r8, qword ptr [r12 + 16]        # link slots
        test    r8, r8
        jz      .Lk_nolinkpf
        mov     rdi, rax
        mov     rsi, rdx
        call    evx_khash
        and     rax, qword ptr [r12 + 24]
        shl     rax, 5
        add     rax, qword ptr [r12 + 16]
        prefetcht0 [rax]                        # link entry, needed at flush
.Lk_nolinkpf:
        mov     rax, qword ptr [rsp + K_CID]
        mov     qword ptr [r15 + 16], rax
        mov     eax, dword ptr [rsp + K_CID + 8]
        mov     dword ptr [r15 + 24], eax
        mov     dword ptr [r15 + 28], 0
        add     r15, 32
        inc     qword ptr [rsp + K_N]
        cmp     qword ptr [rsp + K_N], KBATCH
        jb      .Lk_line
        call    .Lk_flush
        jmp     .Lk_line
.Lk_done:
        cmp     qword ptr [rsp + K_N], 0
        je      .Lk_done2
        call    .Lk_flush
.Lk_done2:
        mov     rax, r15
        sub     rax, rbx
        shr     rax, 5
        add     rsp, KFRAME
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret

# local: resolve the staged batch [r15 - n*32, r15) in three stages so the
# person-table and remap misses of all rows overlap, then compact the kept
# rows in place. rbp = caller frame (this call pushed a return address).
# Preserves rbx, r12, r13, r14; updates r15 and K_N.
.Lk_flush:
        lea     rbp, [rsp + 8]
        # stage A: canonical person key; prefetch its person-table slot
        mov     qword ptr [rbp + K_I], 0
.Lk_fa:
        mov     rcx, qword ptr [rbp + K_I]
        cmp     rcx, qword ptr [rbp + K_N]
        jae     .Lk_fa_done
        mov     rdi, qword ptr [rbp + K_B0 + rcx*8]
        mov     rsi, qword ptr [rbp + K_B1 + rcx*8]
        mov     rdx, qword ptr [r12 + 16]
        mov     rcx, qword ptr [r12 + 24]
        call    evx_canon
        mov     rcx, qword ptr [rbp + K_I]
        mov     qword ptr [rbp + K_B0 + rcx*8], rax
        mov     qword ptr [rbp + K_B1 + rcx*8], rdx
        mov     rdi, rax
        mov     rsi, rdx
        call    evx_khash
        mov     rcx, qword ptr [rbp + K_I]
        and     rax, qword ptr [r12 + 56]
        mov     qword ptr [rbp + K_BH + rcx*8], rax
        shl     rax, 4
        add     rax, qword ptr [r12 + 48]
        prefetcht0 [rax]
        inc     qword ptr [rbp + K_I]
        jmp     .Lk_fa
.Lk_fa_done:
        # stage B: probe the person table; prefetch the slot's remap entry
        mov     qword ptr [rbp + K_I], 0
.Lk_fb:
        mov     rcx, qword ptr [rbp + K_I]
        cmp     rcx, qword ptr [rbp + K_N]
        jae     .Lk_fb_done
        mov     rsi, qword ptr [rbp + K_B0 + rcx*8]     # person key k0
        mov     r10, qword ptr [rbp + K_B1 + rcx*8]     # person key k1
        mov     r9, qword ptr [rbp + K_BH + rcx*8]
        mov     r8, qword ptr [r12 + 48]
.Lk_pf:
        mov     rax, r9
        shl     rax, 4
        add     rax, r8
        mov     rdi, qword ptr [rax + 8]
        test    rdi, rdi
        jz      .Lk_pmiss                       # no surviving events: drop
        cmp     rdi, r10
        jne     .Lk_pfn
        mov     rdi, qword ptr [rax]
        mov     rdx, rsi
        mov     rcx, r10
        push    rsi
        mov     rsi, r10
        call    evx_keq                         # keeps r8 - r10
        pop     rsi
        test    eax, eax
        jnz     .Lk_pfound
.Lk_pfn:
        inc     r9
        and     r9, qword ptr [r12 + 56]
        jmp     .Lk_pf
.Lk_pmiss:
        mov     r9, -1
        jmp     .Lk_pstore
.Lk_pfound:
        mov     rax, qword ptr [r12 + 64]
        prefetcht0 [rax + r9*4]
.Lk_pstore:
        mov     rcx, qword ptr [rbp + K_I]
        mov     qword ptr [rbp + K_BH + rcx*8], r9
        inc     qword ptr [rbp + K_I]
        jmp     .Lk_fb
.Lk_fb_done:
        # stage C: dense id, count, compact kept rows in place
        mov     rax, qword ptr [rbp + K_N]
        shl     rax, 5
        mov     rsi, r15
        sub     rsi, rax                        # batch start (read)
        mov     rdi, rsi                        # write cursor
        xor     ecx, ecx
.Lk_fc:
        cmp     rcx, qword ptr [rbp + K_N]
        jae     .Lk_fc_done
        mov     r9, qword ptr [rbp + K_BH + rcx*8]
        cmp     r9, -1
        je      .Lk_fc_next
        mov     rax, qword ptr [r12 + 64]
        mov     eax, dword ptr [rax + r9*4]     # dense person id
        mov     r8, qword ptr [r12 + 72]
        lock inc dword ptr [r8 + rax*4]
        vmovdqu64 ymm16, ymmword ptr [rsi]
        vmovdqu64 ymmword ptr [rdi], ymm16
        mov     dword ptr [rdi + 8], eax
        add     rdi, 32
.Lk_fc_next:
        add     rsi, 32
        inc     rcx
        jmp     .Lk_fc
.Lk_fc_done:
        mov     r15, rdi
        mov     qword ptr [rbp + K_N], 0
        ret
        .size   evx_parse_clicks, .-evx_parse_clicks

# ---------------------------------------------------------------------------
# void evx_scatter_clicks(const ClickCand *c /*rdi*/, uint64_t n /*rsi*/,
#                         uint64_t *cursor /*rdx*/, ClickEv *dst /*rcx*/,
#                         const ClickCand *base /*r8*/)
# ---------------------------------------------------------------------------
        .globl  evx_scatter_clicks
        .type   evx_scatter_clicks, @function
        .p2align 5
evx_scatter_clicks:
        shl     rsi, 5
        add     rsi, rdi
.Lks_loop:
        cmp     rdi, rsi
        jae     .Lks_done
        mov     eax, dword ptr [rdi + 8]        # dense person
        mov     r9d, 1
        lock xadd qword ptr [rdx + rax*8], r9
        shl     r9, 4
        mov     r10, qword ptr [rdi]
        mov     qword ptr [rcx + r9], r10       # k
        mov     r10d, dword ptr [rdi + 12]
        mov     dword ptr [rcx + r9 + 8], r10d  # camp
        mov     r10, rdi
        sub     r10, r8
        shr     r10, 5
        mov     dword ptr [rcx + r9 + 12], r10d # cand_idx
        add     rdi, 32
        jmp     .Lks_loop
.Lks_done:
        ret
        .size   evx_scatter_clicks, .-evx_scatter_clicks

# ---------------------------------------------------------------------------
# uint64_t evx_attribute(AttrCtx *ctx /*rdi*/, uint64_t pbegin /*rsi*/, uint64_t pend /*rdx*/)
#
# Per dense person (events already sorted by evx_sessionize):
#   pass 1  re-derive sessions (gap > 30 min) into SessRec scratch
#   no clicks: every session credits `organic`
#   else     sort clicks by k, fill day_first[], then a two-pointer sweep over
#            sessions in start order with the click window [s, s + 7 d]:
#              hi admits clicks with k <= s + W7 (cnt[c]++, remember enter[c])
#              lo retires clicks with k < s (cnt[c]--, at 0 flush [enter, i))
#              credit = latest click in [lo, hi); equal-k ties -> smallest
#              click_id (evx_memcmp), only within the top equal-k group
#            assists are flushed lazily per campaign as session intervals and
#            split per day with day_first[]: O(clicks x days), not
#            O(sessions x active campaigns).
#   aggregates go to the worker's dense (campaign + organic) x day table.
#
# Frame: [0] p, [8] pend, [16] lo, [24] hi, [32] credits, [48] saved S ptr,
#        [56] tie j, [64] tie best.
# Registers in the sweep: rbx ctx, r12 SessRec base, r13 m, r14 clicks,
#        r15 n clicks, rbp session index.
# ---------------------------------------------------------------------------
        .globl  evx_attribute
        .type   evx_attribute, @function
        .p2align 5
evx_attribute:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, 72
        mov     rbx, rdi
        mov     qword ptr [rsp], rsi
        mov     qword ptr [rsp + 8], rdx
        mov     qword ptr [rsp + 32], 0
.La_person:
        mov     rax, qword ptr [rsp]
        cmp     rax, qword ptr [rsp + 8]
        jae     .La_done
        inc     qword ptr [rsp]
        mov     rcx, qword ptr [rbx]            # offsets
        mov     rdi, qword ptr [rcx + rax*8]
        mov     rsi, qword ptr [rcx + rax*8 + 8]
        sub     rsi, rdi
        jz      .La_person
        shl     rdi, 4
        add     rdi, qword ptr [rbx + 8]        # first Ev
        shl     rsi, 4
        add     rsi, rdi                        # end
        mov     rcx, qword ptr [rbx + 16]       # coffsets
        mov     r14, qword ptr [rcx + rax*8]
        mov     r15, qword ptr [rcx + rax*8 + 8]
        sub     r15, r14                        # clicks of this person
        shl     r14, 4
        add     r14, qword ptr [rbx + 24]
        mov     r12, qword ptr [rbx + 48]       # SessRec scratch
        xor     r13d, r13d

        # ---- pass 1: sessions ------------------------------------------------
        mov     r8, qword ptr [rdi]             # prev
        mov     r9, r8                          # session start
        xor     r10d, r10d                      # session revenue
.La_p1:
        mov     rax, qword ptr [rdi]
        mov     rdx, rax
        sub     rdx, r8
        cmp     rdx, GAP
        jle     .La_p1same
        mov     rcx, rax
        call    .La_emit
        mov     r9, rcx
        xor     r10d, r10d
        mov     rax, rcx
.La_p1same:
        mov     r8, rax
        mov     eax, dword ptr [rdi + 8]
        and     eax, 0xFFFFFF
        add     r10, rax
        add     rdi, 16
        cmp     rdi, rsi
        jb      .La_p1
        call    .La_emit

        test    r15, r15
        jnz     .La_clicks
        # ---- no clicks: all organic ----------------------------------------
        xor     ebp, ebp
.La_org:
        cmp     rbp, r13
        jae     .La_person
        lea     r11, [rbp + rbp*2]
        lea     r11, [r12 + r11*8]
        mov     ecx, dword ptr [rbx + 96]
        call    .La_credit
        inc     rbp
        jmp     .La_org

.La_clicks:
        mov     rdi, r14
        mov     rsi, r15
        mov     rdx, qword ptr [rbx + 40]
        mov     ecx, dword ptr [rbx + 104]
        lea     r8, [rbx + 112]
        call    evx_sort16
        # day_first[d] = first session with day_rel >= d, d = 0 .. ndays + 1
        mov     rdi, qword ptr [rbx + 72]
        xor     ecx, ecx
        xor     edx, edx
        mov     r8d, dword ptr [rbx + 100]
        inc     r8d
.La_df:
        cmp     rdx, r13
        jae     .La_dfs
        lea     rax, [rdx + rdx*2]
        cmp     dword ptr [r12 + rax*8 + 8], ecx
        jae     .La_dfs
        inc     rdx
        jmp     .La_df
.La_dfs:
        mov     dword ptr [rdi + rcx*4], edx
        inc     ecx
        cmp     ecx, r8d
        jbe     .La_df

        # ---- pass 2: two-pointer sweep -------------------------------------
        mov     qword ptr [rsp + 16], 0         # lo
        mov     qword ptr [rsp + 24], 0         # hi
        xor     ebp, ebp                        # i
.La_p2:
        cmp     rbp, r13
        jae     .La_p2end
        lea     r11, [rbp + rbp*2]
        lea     r11, [r12 + r11*8]              # S[i]
        mov     r10, qword ptr [r11]
        add     r10, W7                         # s + 7 days
        mov     rax, qword ptr [rsp + 24]
.La_en:
        cmp     rax, r15
        jae     .La_en_done
        mov     rdx, rax
        shl     rdx, 4
        cmp     qword ptr [r14 + rdx], r10
        ja      .La_en_done
        mov     ecx, dword ptr [r14 + rdx + 8]
        mov     r8, qword ptr [rbx + 56]
        mov     r9d, dword ptr [r8 + rcx*4]
        inc     dword ptr [r8 + rcx*4]
        test    r9d, r9d
        jnz     .La_en_next
        mov     r8, qword ptr [rbx + 64]
        mov     dword ptr [r8 + rcx*4], ebp     # enter[c] = i
.La_en_next:
        inc     rax
        jmp     .La_en
.La_en_done:
        mov     qword ptr [rsp + 24], rax
        mov     r10, qword ptr [r11]            # s
        mov     rax, qword ptr [rsp + 16]
.La_lv:
        cmp     rax, qword ptr [rsp + 24]
        jae     .La_lv_done
        mov     rdx, rax
        shl     rdx, 4
        cmp     qword ptr [r14 + rdx], r10
        jae     .La_lv_done
        mov     ecx, dword ptr [r14 + rdx + 8]
        mov     r8, qword ptr [rbx + 56]
        dec     dword ptr [r8 + rcx*4]
        jnz     .La_lv_next
        mov     qword ptr [rsp + 16], rax
        mov     qword ptr [rsp + 48], r11
        mov     r8, qword ptr [rbx + 64]
        mov     eax, dword ptr [r8 + rcx*4]     # a = enter[c]
        mov     rdx, rbp                        # b = i
        call    .La_flush
        mov     rax, qword ptr [rsp + 16]
        mov     r11, qword ptr [rsp + 48]
        mov     r10, qword ptr [r11]
.La_lv_next:
        inc     rax
        jmp     .La_lv
.La_lv_done:
        mov     qword ptr [rsp + 16], rax
        # credit: latest click in [lo, hi), ties -> smallest click_id
        mov     ecx, dword ptr [rbx + 96]       # organic
        mov     rax, qword ptr [rsp + 24]
        cmp     rax, qword ptr [rsp + 16]
        jbe     .La_cred
        dec     rax
        mov     qword ptr [rsp + 56], rax       # j
        mov     qword ptr [rsp + 64], rax       # best
        shl     rax, 4
        mov     r10, qword ptr [r14 + rax]      # t
.La_tie:
        mov     rax, qword ptr [rsp + 56]
        cmp     rax, qword ptr [rsp + 16]
        jbe     .La_tie_done
        lea     rdx, [rax - 1]
        mov     r8, rdx
        shl     r8, 4
        cmp     qword ptr [r14 + r8], r10
        jne     .La_tie_done
        mov     qword ptr [rsp + 56], rdx
        mov     r8d, dword ptr [r14 + r8 + 12]
        shl     r8, 5
        add     r8, qword ptr [rbx + 32]        # ClickCand of j
        mov     r9, qword ptr [rsp + 64]
        shl     r9, 4
        mov     r9d, dword ptr [r14 + r9 + 12]
        shl     r9, 5
        add     r9, qword ptr [rbx + 32]        # ClickCand of best
        mov     rdi, qword ptr [r8 + 16]
        mov     esi, dword ptr [r8 + 24]
        mov     rdx, qword ptr [r9 + 16]
        mov     ecx, dword ptr [r9 + 24]
        call    evx_memcmp                      # keeps r10
        test    eax, eax
        jns     .La_tie
        mov     rax, qword ptr [rsp + 56]
        mov     qword ptr [rsp + 64], rax
        jmp     .La_tie
.La_tie_done:
        mov     rax, qword ptr [rsp + 64]
        shl     rax, 4
        mov     ecx, dword ptr [r14 + rax + 8]
        inc     qword ptr [rsp + 32]
.La_cred:
        lea     r11, [rbp + rbp*2]
        lea     r11, [r12 + r11*8]
        call    .La_credit
        inc     rbp
        jmp     .La_p2
.La_p2end:
        # close every campaign still active; also resets cnt[] for the next person
        mov     rax, qword ptr [rsp + 16]
.La_cl:
        cmp     rax, qword ptr [rsp + 24]
        jae     .La_person
        mov     rdx, rax
        shl     rdx, 4
        mov     ecx, dword ptr [r14 + rdx + 8]
        mov     r8, qword ptr [rbx + 56]
        cmp     dword ptr [r8 + rcx*4], 0
        je      .La_cl_next
        mov     dword ptr [r8 + rcx*4], 0
        mov     qword ptr [rsp + 16], rax
        mov     r8, qword ptr [rbx + 64]
        mov     eax, dword ptr [r8 + rcx*4]
        mov     rdx, r13
        call    .La_flush
        mov     rax, qword ptr [rsp + 16]
.La_cl_next:
        inc     rax
        jmp     .La_cl

.La_done:
        mov     rax, qword ptr [rsp + 32]
        add     rsp, 72
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret

# local: append SessRec {start r9, revenue r10}; clobbers rax, rdx, r11.
.La_emit:
        lea     r11, [r13 + r13*2]
        lea     r11, [r12 + r11*8]
        mov     qword ptr [r11], r9
        mov     qword ptr [r11 + 16], r10
        mov     rax, r9
        add     rax, qword ptr [rbx + 88]
        xor     edx, edx
        div     qword ptr [rip + .La_day_ms]
        mov     dword ptr [r11 + 8], eax
        mov     dword ptr [r11 + 12], 0
        inc     r13
        ret

# local: credit session r11 to campaign ecx; clobbers rax, rdx.
.La_credit:
        mov     eax, ecx
        mov     edx, dword ptr [rbx + 100]
        imul    rax, rdx
        mov     edx, dword ptr [r11 + 8]
        add     rax, rdx
        lea     rax, [rax + rax*2]
        shl     rax, 3
        add     rax, qword ptr [rbx + 80]
        inc     dword ptr [rax]
        mov     rdx, qword ptr [r11 + 16]
        add     qword ptr [rax + 16], rdx
        test    rdx, rdx
        jz      .La_cr_out
        inc     dword ptr [rax + 4]
.La_cr_out:
        ret

# local: campaign ecx assisted sessions [eax, edx); split per day with
# day_first. Clobbers rax, rcx, rdx, rsi, rdi, r8 - r11.
.La_flush:
        cmp     eax, edx
        jae     .La_fl_out
        mov     esi, eax                        # a
        mov     edi, edx                        # b
        lea     r8, [rsi + rsi*2]
        mov     r8d, dword ptr [r12 + r8*8 + 8] # first day
        lea     r9, [rdi - 1]
        lea     r9, [r9 + r9*2]
        mov     r9d, dword ptr [r12 + r9*8 + 8] # last day
        mov     eax, ecx
        mov     edx, dword ptr [rbx + 100]
        imul    rax, rdx
        lea     rax, [rax + rax*2]
        shl     rax, 3
        add     rax, qword ptr [rbx + 80]
        mov     r10, rax                        # cells of campaign c
        mov     r11, qword ptr [rbx + 72]       # day_first
.La_fl_day:
        cmp     r8d, r9d
        ja      .La_fl_out
        mov     eax, dword ptr [r11 + r8*4]
        cmp     eax, esi
        cmovb   eax, esi                        # max(a, day_first[d])
        mov     edx, dword ptr [r11 + r8*4 + 4]
        cmp     edx, edi
        cmova   edx, edi                        # min(b, day_first[d + 1])
        cmp     edx, eax
        jbe     .La_fl_next
        sub     edx, eax
        lea     rcx, [r8 + r8*2]
        add     dword ptr [r10 + rcx*8 + 8], edx
.La_fl_next:
        inc     r8d
        jmp     .La_fl_day
.La_fl_out:
        ret
        .size   evx_attribute, .-evx_attribute

        .section .rodata
        .p2align 3
.La_day_ms:
        .quad   86400000

        .section .note.GNU-stack,"",@progbits
