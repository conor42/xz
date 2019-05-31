###############################################################################
#
## \file       lzma_dec_x86_64.S
## \brief      LZMA assembler-optimized decoder
##
#  Authors:    Igor Pavlov
#              Conor McCarthy (liblzma .S version)
#
#  This file has been put into the public domain.
#  You can do whatever you want with this file.
#
###############################################################################

# lzma_lzma1_decoder and lzma_dict structures, and probability table layout
# must be equal in both versions (C / ASM).


.intel_syntax noprefix


.if MS_x64_CALL
# for WIN64-x64 ABI:
.equ REG_PARAM_0, rcx
.equ REG_PARAM_1, rdx
.equ REG_PARAM_2, r8
.equ REG_PARAM_3, r9

.macro MY_PUSH_PRESERVED_REGS
    push    rbx
    push    rbp
    push    rsi # WIN64
    push    rdi # WIN64
    push    r12
    push    r13
    push    r14
    push    r15
.endm

.macro MY_POP_PRESERVED_REGS
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi # WIN64
    pop     rsi # WIN64
    pop     rbp
    pop     rbx
.endm

.else
# for System V AMD64 ABI:
.equ REG_PARAM_0, rdi
.equ REG_PARAM_1, rsi
.equ REG_PARAM_2, rdx
.equ REG_PARAM_3, rcx

.macro MY_PUSH_PRESERVED_REGS
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
.endm

.macro MY_POP_PRESERVED_REGS
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
.endm

.endif



.macro MY_ALIGN  num:req
        .balign  \num
.endm

.macro MY_ALIGN_16
        MY_ALIGN 16
.endm

.macro MY_ALIGN_64
        MY_ALIGN 64
.endm


# Definitions for sizeof(probability) and loading values

.equ PSHIFT, 1
.macro PLOAD dest, mem
        movzx   \dest, word ptr [\mem]
.endm
.macro PSTORE src, mem
        mov     word ptr [\mem], \src\()_W
.endm

.equ PMULT, (1 SHL PSHIFT)
.equ PMULT_HALF, (1 SHL (PSHIFT - 1))
.equ PMULT_2, (1 SHL (PSHIFT + 1))


#       eax      range
#       ecx      pbPos / (prob) TREE
#       edx      probBranch / prm (MATCHED) / pbPos / cnt
#       ebx      sym
#       ebp      cod
#       esi      t1 NORM_CALC / probs_state / dist
#       edi      t0 NORM_CALC / prob2 IF_BIT_1
#       r8d      state
#       r9d      match (MATCHED) / sym2 / dist2 / lpMask_reg
#       r10d     kBitModelTotal_reg
#       r11      probs
#       r12d     offs (MATCHED) / dic / len_temp
#       r13d     dicPos
#       r14d     bit (MATCHED) / dicDst
#       r15      buf


.equ cod, ebp
.equ cod_L, bpl
.equ range, eax
.equ state, r8d
.equ state_R, r8
.equ buf, r15
.equ dicPos, r13d
.equ dicPos_R, r13
.equ kBitModelTotal_reg, r10d

.equ probBranch, edx
.equ probBranch_R, rdx
.equ probBranch_W, dx

.equ pbPos, ecx
.equ pbPos_R, rcx

.equ cnt, edx
.equ cnt_R, rdx

.equ lpMask_reg, r9d
.equ dicDst, r14

.equ sym, ebx
.equ sym_R, rbx
.equ sym_L, bl

.equ probs, r11
.equ dic, r12

.equ t0, edi
.equ t0_W, di
.equ t0_R, rdi

.equ prob2, t0
.equ prob2_W, t0_W

.equ t1, esi
.equ t1_R, rsi

.equ probs_state, t1
.equ probs_state_R, t1_R

.equ prm, rdx
.equ match, r9d
.equ match_R, r9
.equ offs, r12d
.equ offs_R, r12
.equ bit, r14d
.equ bit_R, r14

.equ sym2, r9d
.equ sym2_R, r9

.equ len_temp, r12d

.equ dist, sym
.equ dist2, r9d


.equ kNumBitModelTotalBits, 11
.equ kBitModelTotal, (1 SHL kNumBitModelTotalBits)
.equ kNumMoveBits, 5
.equ kBitModelOffset, ((1 SHL kNumMoveBits) - 1)
.equ kTopValue, (1 SHL 24)


.macro NORM_2
        shl     cod, 8
        mov     cod_L, BYTE PTR [buf]
        shl     range, 8
        inc     buf
.endm


.macro NORM
        cmp     range, kTopValue
        jae     SHORT 1f
        NORM_2
1:
.endm


# ---------- Branch MACROS ----------

.macro UPDATE_0 probsArray:req, probOffset:req, probDisp:req
        mov     prob2, kBitModelTotal_reg
        sub     prob2, probBranch
        shr     prob2, kNumMoveBits
        add     probBranch, prob2
        PSTORE  probBranch, (\probOffset * 1 + \probsArray + \probDisp * PMULT)
.endm


.macro UPDATE_1 probsArray:req, probOffset:req, probDisp:req
        sub     prob2, range
        sub     cod, range
        mov     range, prob2
        mov     prob2, probBranch
        shr     probBranch, kNumMoveBits
        sub     prob2, probBranch
        PSTORE  prob2, (\probOffset * 1 + \probsArray + \probDisp * PMULT)
.endm


.macro CMP_COD probsArray:req, probOffset:req, probDisp:req
        PLOAD   probBranch, (\probOffset * 1 + \probsArray + \probDisp * PMULT)
        NORM
        mov     prob2, range
        shr     range, kNumBitModelTotalBits
        imul    range, probBranch
        cmp     cod, range
.endm


.macro IF_BIT_1_NOUP probsArray:req, probOffset:req, probDisp:req, toLabel:req
        CMP_COD \probsArray, \probOffset, \probDisp
        jae     \toLabel
.endm


.macro IF_BIT_1 probsArray:req, probOffset:req, probDisp:req, toLabel:req
        IF_BIT_1_NOUP \probsArray, \probOffset, \probDisp, \toLabel
        UPDATE_0 \probsArray, \probOffset, \probDisp
.endm


.macro IF_BIT_0_NOUP probsArray:req, probOffset:req, probDisp:req, toLabel:req
        CMP_COD \probsArray, \probOffset, \probDisp
        jb      \toLabel
.endm


# ---------- CMOV MACROS ----------

.macro NORM_CALC prob:req
        NORM
        mov     t0, range
        shr     range, kNumBitModelTotalBits
        imul    range, \prob
        sub     t0, range
        mov     t1, cod
        sub     cod, range
.endm


.macro PUP prob:req, probPtr:req
        sub     t0, \prob
       # only sar works for both 16/32 bit prob modes
        sar     t0, kNumMoveBits
        add     t0, \prob
        PSTORE  t0, \probPtr
.endm


.macro PUP_SUB prob:req, probPtr:req, symSub:req
        sbb     sym, \symSub
        PUP \prob, \probPtr
.endm


.macro PUP_COD prob:req, probPtr:req, symSub:req
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        mov     t1, sym
        cmovb   t0, kBitModelTotal_reg
        PUP_SUB \prob, \probPtr, \symSub
.endm


.macro BIT_0 prob:req, probNext:req
        PLOAD   \prob, (probs + 1 * PMULT)
        PLOAD   \probNext, (probs + 1 * PMULT_2)

        NORM_CALC \prob
        
        cmovae  range, t0
        PLOAD   t0, (probs + 1 * PMULT_2 + PMULT)
        cmovae  \probNext, t0
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        cmovb   t0, kBitModelTotal_reg
        mov     sym, 2
        PUP_SUB \prob, (probs + 1 * PMULT), (0 - 1)
.endm


.macro BIT_1 prob:req, probNext:req
        PLOAD   \probNext, (probs + sym_R * PMULT_2)
        add     sym, sym
        
        NORM_CALC \prob
        
        cmovae  range, t0
        PLOAD   t0, (probs + sym_R * PMULT + PMULT)
        cmovae  \probNext, t0
        PUP_COD \prob, (probs + t1_R * PMULT_HALF), (0 - 1)
.endm


.macro BIT_2 prob:req, symSub:req
        add     sym, sym

        NORM_CALC \prob
        
        cmovae  range, t0
        PUP_COD \prob, (probs + t1_R * PMULT_HALF), \symSub
.endm


# ---------- MATCHED LITERAL ----------

.macro LITM_0
        mov     offs, 256 * PMULT
        shl     match, (PSHIFT + 1)
        mov     bit, offs
        and     bit, match
        PLOAD   ecx, (probs + 256 * PMULT + bit_R * 1 + 1 * PMULT)
        lea     prm, [probs + 256 * PMULT + bit_R * 1 + 1 * PMULT]
        xor     offs, bit
        add     match, match

        NORM_CALC ecx

        cmovae  offs, bit
        mov     bit, match
        cmovae  range, t0
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        cmovb   t0, kBitModelTotal_reg
        mov     sym, 0
        PUP_SUB ecx, prm, (-2-1)
.endm


.macro LITM
        and     bit, offs
        lea     prm, [probs + offs_R * 1]
        add     prm, bit_R
        PLOAD   ecx, (prm + sym_R * PMULT)
        xor     offs, bit
        add     sym, sym
        add     match, match

        NORM_CALC ecx

        cmovae  offs, bit
        mov     bit, match
        cmovae  range, t0
        PUP_COD ecx, (prm + t1_R * PMULT_HALF), (- 1)
.endm


.macro LITM_2
        and     bit, offs
        lea     prm, [probs + offs_R * 1]
        add     prm, bit_R
        PLOAD   ecx, (prm + sym_R * PMULT)
        add     sym, sym

        NORM_CALC ecx

        cmovae  range, t0
        PUP_COD ecx, (prm + t1_R * PMULT_HALF), (256 - 1)
.endm


# ---------- REVERSE BITS ----------

.macro REV_0 prob:req, probNext:req
        PLOAD   \probNext, sym2_R

        NORM_CALC \prob

        cmovae  range, t0
        PLOAD   t0, (probs + 3 * PMULT)
        cmovae  \probNext, t0
        cmovb   cod, t1
        mov     t0, kBitModelOffset
        cmovb   t0, kBitModelTotal_reg
        lea     t1_R, [probs + 3 * PMULT]
        cmovae  sym2_R, t1_R
        PUP \prob, (probs + 1 * PMULT)
.endm


.macro REV_1 prob:req, probNext:req, step:req
        add     sym2_R, \step * PMULT
        PLOAD   \probNext, sym2_R

        NORM_CALC \prob

        cmovae  range, t0
        PLOAD   t0, (sym2_R + \step * PMULT)
        cmovae  \probNext, t0
        cmovb   cod, t1
        mov     t0, kBitModelOffset
        cmovb   t0, kBitModelTotal_reg
        lea     t1_R, [sym2_R + \step * PMULT]
        cmovae  sym2_R, t1_R
        PUP \prob, (t1_R - \step * PMULT_2)
.endm


.macro REV_2 prob:req, step:req
        sub     sym2_R, probs
        shr     sym2, PSHIFT
        or      sym, sym2

        NORM_CALC \prob

        cmovae  range, t0
        lea     t0, [sym - \step]
        cmovb   sym, t0
        cmovb   cod, t1
        mov     t0, kBitModelOffset
        cmovb   t0, kBitModelTotal_reg
        PUP \prob, (probs + sym2_R * PMULT)
.endm


.macro REV_1_VAR prob:req
        PLOAD   \prob, sym_R
        mov     probs, sym_R
        add     sym_R, sym2_R

        NORM_CALC \prob

        cmovae  range, t0
        lea     t0_R, [sym_R + sym2_R]
        cmovae  sym_R, t0_R
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        cmovb   t0, kBitModelTotal_reg
        add     sym2, sym2
        PUP \prob, probs
.endm


.macro LIT_PROBS lpMaskParam:req
        # prob += (UInt32)3 * ((((processedPos << 8) + dic[(dicPos == 0 ? dicBufSize : dicPos) - 1]) & lpMask) << lc)#
        mov     t0, dicPos
        shl     t0, 8
        add     sym, t0
        and     sym, \lpMaskParam
        add     probs_state_R, pbPos_R
        mov     ecx, [LOC + lc2]
        lea     sym, dword ptr[sym_R + 2 * sym_R]
        add     probs, Literal * PMULT
        shl     sym, cl
        add     probs, sym_R
        UPDATE_0 probs_state_R, 0, IsMatch
        inc     dicPos
.endm


.equ kNumPosBitsMax, 4
.equ kNumPosStatesMax, (1 SHL kNumPosBitsMax)

.equ kLenNumLowBits, 3
.equ kLenNumLowSymbols, (1 SHL kLenNumLowBits)
.equ kLenNumHighBits, 8
.equ kLenNumHighSymbols, (1 SHL kLenNumHighBits)

.equ LenLow, 0
.equ LenChoice, LenLow
.equ LenChoice2, (LenLow + kLenNumLowSymbols)
.equ LenHigh, (LenLow + 2 * kLenNumLowSymbols * kNumPosStatesMax)
.equ kNumLenProbs, (LenHigh + kLenNumHighSymbols)

.equ kLcLpMax, 4
.equ kLiteralCodersMax, (1 SHL kLcLpMax)
.equ kLiteralCoderSize, 0x300

.equ kNumStates, 12
.equ kNumStates2, 16
.equ kNumLitStates, 7

.equ kStartPosModelIndex, 4
.equ kEndPosModelIndex, 14
.equ kNumFullDistances, (1 SHL (kEndPosModelIndex SHR 1))

.equ kNumPosSlotBits, 6
.equ kNumLenToPosStates, 4

.equ kNumAlignBits, 4
.equ kAlignTableSize, (1 SHL kNumAlignBits)

.equ kMatchMinLen, 2
.equ kMatchSpecLenStart, (kMatchMinLen + kLenNumLowSymbols * 2 + kLenNumHighSymbols)


# Memory address offsets between -128 and +127 require 1 byte instead of 8, making smaller
# and faster code. A base address here allows short addresses for commonly used probs.
.equ kAlign, 0
# Tables before position 0
.equ IsMatch, kAlign - (kNumStates2 SHL kNumPosBitsMax)
.equ LenCoder, IsMatch - kNumLenProbs
.equ RepLenCoder, LenCoder - kNumLenProbs
.equ IsRep0Long, RepLenCoder - (kNumStates2 SHL kNumPosBitsMax)
.equ SpecPos, IsRep0Long - kNumFullDistances
# Tables after position 0
.equ IsRep, kAlign + kAlignTableSize
.equ IsRepG0, IsRep + kNumStates
.equ IsRepG1, IsRepG0 + kNumStates
.equ IsRepG2, IsRepG1 + kNumStates
.equ PosSlot, IsRepG2 + kNumStates
.equ Literal, PosSlot + (kNumLenToPosStates SHL kNumPosSlotBits)
.equ TOTAL_PROB_COUNT, Literal + kLiteralCodersMax * kLiteralCoderSize - SpecPos


# lzma_dict:
    .equ dict_buf, 0
    .equ dict_pos, 8
    .equ dict_full, 16
    .equ dict_limit, 24
    .equ dict_size, 32


# lzma_lzma1_decoder:
    .equ range_Spec, 0
    .equ code_Spec, 4
    .equ init_count, 8
    .equ state_Spec, 12
    .equ rep0, 16
    .equ rep1, 20
    .equ rep2, 24
    .equ rep3, 28
    .equ pbMask_Spec, 32
    .equ lc, 36
    .equ lpMask_Spec, 40
    .equ sequence, 44
    .equ remainLen, 48


# lzma_dec_local:
    .equ Old_RSP, 0
    .equ lzmaPtr, 8
    .equ buf_Loc, 16
    .equ bufPosPtr, 24
    .equ dict, 32
    .equ dicBufSize, 40
    .equ probs_Spec, 48
    .equ dic_Spec, 56
        
    .equ limit, 64
    .equ bufLimit, 72
    .equ full, 80
    .equ lc2, 84
    .equ lpMask, 88
    .equ pbMask, 92

    .equ dicDst_Spec, 96
    .equ remainLen_Loc, 104
    .equ rep0_Loc, 108
    .equ rep1_Loc, 112
    .equ rep2_Loc, 116
    .equ rep3_Loc, 120

    .equ sizeof_lzma_dec_local, 128

.equ GLOB_2,  sym_R
.equ GLOB,    rcx
.equ LOC_0,   rax
.equ LOC,     rsp


.macro IsMatchBranch_Pre reg
        # prob = probs + IsMatch + (state << kNumPosBitsMax) + posState#
        mov     pbPos, [LOC + pbMask]
        and     pbPos, dicPos
        shl     pbPos, (kLenNumLowBits + 1 + PSHIFT)
        lea     probs_state_R, [probs + state_R]
.endm


.macro IsMatchBranch reg
        IsMatchBranch_Pre
        IF_BIT_1 probs_state_R, pbPos_R, IsMatch, IsMatch_label
.endm
        

.macro CheckLimits reg
        cmp     buf, [LOC + bufLimit]
        jae     fin_OK
        cmp     dicDst, [LOC + limit]
        jae     fin_OK
.endm



# RSP is (16x + 8) bytes aligned in WIN64-x64
# .equ LocalSize, ((((SIZEOF CLzmaDec_Asm_Loc) + 7) / 16 * 16) + 8)

.equ PARAM_lzma, REG_PARAM_0
.equ PARAM_dict, REG_PARAM_1
.equ PARAM_buf, REG_PARAM_2
.equ PARAM_bufPosPtr, REG_PARAM_3

        .text

# MY_ALIGN_64
        .balign 16, 0x90
        .global lzma_decode_asm_5
lzma_decode_asm_5:
        MY_PUSH_PRESERVED_REGS

        lea     rax, [RSP - sizeof_lzma_dec_local]
        and     rax, -128
        mov     rbp, RSP
        mov     RSP, rax
        mov     [LOC_0 + Old_RSP], rbp
.if MS_x64_CALL
        mov     rbx, [rbp + 0x68]
.else
        mov     rbx, r8
.endif
        add     rbx, PARAM_buf
        mov     [LOC_0 + bufLimit], rbx
        mov     [LOC_0 + dict], PARAM_dict
        mov     [LOC_0 + bufPosPtr], PARAM_bufPosPtr
        mov     [LOC_0 + buf_Loc], PARAM_buf
        mov     buf, [PARAM_bufPosPtr]
        add     buf, PARAM_buf
        mov     dic, [PARAM_dict + dict_buf]
        mov     dicDst, [PARAM_dict + dict_pos]
        mov     dicPos_R, [PARAM_dict + dict_pos]

        add     dicDst, dic
        mov     [LOC_0 + dicDst_Spec], dicDst
        mov     [LOC_0 + dic_Spec], dic
        
        mov     sym_R, PARAM_lzma  #  lzma_dec_local pointer for GLOB_2

        mov     t0_R, [PARAM_dict + dict_limit]
        add     t0_R, dic
        mov     [LOC_0 + limit], t0_R

        mov     t0_R, [PARAM_dict + dict_size]
        mov     [LOC_0 + dicBufSize], t0_R
        mov     t0_R, [PARAM_dict + dict_full]
        mov     [LOC_0 + full], t0

        mov     dword ptr [LOC_0 + remainLen_Loc], 0  # remainLen must be ZERO

        mov     probs, sym_R
        sub     probs, SpecPos * PMULT
        mov     [LOC_0 + probs_Spec], probs

        add     sym_R, TOTAL_PROB_COUNT * PMULT  # sizeof probs
        mov     [LOC_0 + lzmaPtr], sym_R      

        mov     t0, [GLOB_2 + rep0]
        mov     [LOC_0 + rep0_Loc], t0
        mov     t0, [GLOB_2 + rep1]
        mov     [LOC_0 + rep1_Loc], t0
        mov     t0, [GLOB_2 + rep2]
        mov     [LOC_0 + rep2_Loc], t0
        mov     t0, [GLOB_2 + rep3]
        mov     [LOC_0 + rep3_Loc], t0
        mov     t0, [GLOB_2 + pbMask_Spec]
        mov     [LOC_0 + pbMask], t0

        # unsigned pbMask = ((unsigned)1 << (p->prop.pb)) - 1#
        # unsigned lc = p->prop.lc#
        # unsigned lpMask = ((unsigned)0x100 << p->prop.lp) - ((unsigned)0x100 >> lc)#

        mov     ecx, [GLOB_2 + lc]
        add     ecx, PSHIFT
        mov     [LOC_0 + lc2], ecx
        mov     t0, [GLOB_2 + lpMask_Spec]
        mov     [LOC_0 + lpMask], t0
        mov     lpMask_reg, t0
        
        mov     state, [GLOB_2 + state_Spec]
        shl     state, PSHIFT

        mov     range, [GLOB_2 + range_Spec]
        mov     cod,   [GLOB_2 + code_Spec]
        mov     kBitModelTotal_reg, kBitModelTotal
        xor     sym_R, sym_R

        ## if (processedPos != 0)
        cmp     dword ptr [LOC + full], 0
        je      1f
        
        mov     t0_R, [LOC + dicBufSize]
        add     t0_R, dic
        cmp     dicDst, dic
        cmovnz  t0_R, dicDst
        movzx   sym, byte ptr[t0_R - 1]

1:
        IsMatchBranch_Pre
        cmp     state, 4 * PMULT
        jb      lit_end
        cmp     state, kNumLitStates * PMULT
        jb      lit_matched_end
        jmp     lz_end
        

# ---------- LITERAL ----------
MY_ALIGN_64
lit_start:
        xor     state, state
lit_start_2:
        LIT_PROBS lpMask_reg

        BIT_0   ecx, edx
        BIT_1   edx, ecx
        BIT_1   ecx, edx
        BIT_1   edx, ecx
        BIT_1   ecx, edx
        BIT_1   edx, ecx
        BIT_1   ecx, edx
        
        BIT_2   edx, (256 - 1)
        
        mov     probs, [LOC + probs_Spec]
        IsMatchBranch_Pre
        mov     byte ptr[dicDst], sym_L
        inc     dicDst
        cmp     [LOC + full], dicPos
        jae     1f
        mov     [LOC + full], dicPos
1:
        CheckLimits
lit_end:
        IF_BIT_0_NOUP probs_state_R, pbPos_R, IsMatch, lit_start

# ---------- MATCHES ----------
# MY_ALIGN_16
IsMatch_label:
        UPDATE_1 probs_state_R, pbPos_R, IsMatch
        IF_BIT_1 probs_state_R, 0, IsRep, IsRep_label

        add     probs, LenCoder * PMULT
        add     state, kNumStates * PMULT

# ---------- LEN DECODE ----------
len_decode:
        mov     len_temp, 8 - 1 - kMatchMinLen
        IF_BIT_0_NOUP probs, 0, 0, len_mid_0
        UPDATE_1 probs, 0, 0
        add     probs, (1 SHL (kLenNumLowBits + PSHIFT))
        mov     len_temp, -1 - kMatchMinLen
        IF_BIT_0_NOUP probs, 0, 0, len_mid_0
        UPDATE_1 probs, 0, 0
        add     probs, LenHigh * PMULT - (1 SHL (kLenNumLowBits + PSHIFT))
        mov     sym, 1
        PLOAD   ecx, (probs + 1 * PMULT)

MY_ALIGN_16
len8_loop:
        BIT_1   ecx, edx
        mov     ecx, edx
        cmp     sym, 64
        jb      len8_loop
        
        mov     len_temp, (kLenNumHighSymbols - kLenNumLowSymbols * 2) - 1 - kMatchMinLen
        jmp     len_mid_2
        
MY_ALIGN_16
len_mid_0:
        UPDATE_0 probs, 0, 0
        add     probs, pbPos_R
        BIT_0   edx, ecx
len_mid_2:
        BIT_1   ecx, edx
        BIT_2   edx, len_temp
        mov     probs, [LOC + probs_Spec]
        cmp     state, kNumStates * PMULT
        jb      copy_match
        

# ---------- DECODE DISTANCE ----------
        # probs + PosSlot + ((len < kNumLenToPosStates ? len : kNumLenToPosStates - 1) << kNumPosSlotBits)#

        mov     t0, 3 + kMatchMinLen
        cmp     sym, 3 + kMatchMinLen
        cmovb   t0, sym
        add     probs, PosSlot * PMULT - (kMatchMinLen SHL (kNumPosSlotBits + PSHIFT))
        shl     t0, (kNumPosSlotBits + PSHIFT)
        add     probs, t0_R
        
        # sym = Len
        mov     len_temp, sym

        BIT_0   ecx, edx
        BIT_1   edx, ecx
        BIT_1   ecx, edx
        BIT_1   edx, ecx
        BIT_1   ecx, edx

        mov     ecx, sym
        BIT_2   edx, 64-1

        and     sym, 3
        mov     probs, [LOC + probs_Spec]
        cmp     ecx, 32 + kEndPosModelIndex / 2
        jb      short_dist

        #  unsigned numDirectBits = (unsigned)(((distance >> 1) - 1))#
        sub     ecx, (32 + 1 + kNumAlignBits)
        #  distance = (2 | (distance & 1))#
        or      sym, 2
        PLOAD   edx, (probs + 1 * PMULT)
        shl     sym, kNumAlignBits + 1
        lea     sym2_R, [probs + 2 * PMULT]
        
        jmp     direct_norm
        
# ---------- DIRECT DISTANCE ----------
MY_ALIGN_16
direct_loop:
        shr     range, 1
        mov     t0, cod
        sub     cod, range
        cmovs   cod, t0
        cmovns  sym, t1
        
        dec     ecx
        je      direct_end

        add     sym, sym
direct_norm:
        lea     t1, [sym_R + (1 SHL kNumAlignBits)]
        cmp     range, kTopValue
        jae     near ptr direct_loop
        # we align for 16 here with "near ptr" command above
        NORM_2
        jmp     direct_loop

MY_ALIGN_16
direct_end:
        #  prob =  + kAlign#
        #  distance <<= kNumAlignBits#
        REV_0   edx, ecx
        REV_1   ecx, edx, 2
        REV_1   edx, ecx, 4
        REV_2   ecx, 8

decode_dist_end:

        ## if (distance >= (checkDicSize == 0 ? processedPos: checkDicSize))

        cmp     sym, [LOC + full]
        jae     end_of_payload
        
        # rep3 = rep2#
        # rep2 = rep1#
        # rep1 = rep0#
        # rep0 = distance + 1#

        inc     sym
        mov     t0, [LOC + rep0_Loc]
        mov     t1, [LOC + rep1_Loc]
        mov     ecx, [LOC + rep2_Loc]
        mov     [LOC + rep0_Loc], sym
        mov     sym, len_temp
        mov     [LOC + rep1_Loc], t0
        mov     [LOC + rep2_Loc], t1
        mov     [LOC + rep3_Loc], ecx
        
        # state = (state < kNumStates + kNumLitStates) ? kNumLitStates : kNumLitStates + 3#
        cmp     state, (kNumStates + kNumLitStates) * PMULT
        mov     state, kNumLitStates * PMULT
        mov     t0, (kNumLitStates + 3) * PMULT
        cmovae  state, t0


# ---------- COPY MATCH ----------
copy_match:

        ## if ((rem = limit - dicPos) == 0)
        # {
        #   p->dicPos = dicPos#
        #   return SZ_ERROR_DATA#
        # }
        mov     cnt_R, [LOC + limit]
        sub     cnt_R, dicDst
        jz      fin_ERROR

        # curLen = ((rem < len) ? (unsigned)rem : len)#
        cmp     cnt_R, sym_R
        cmovae  cnt, sym

        mov     dic, [LOC + dic_Spec]

        mov     t0_R, dicDst
        add     dicDst, cnt_R
        mov     ecx, [LOC + rep0_Loc]
        # processedPos += curLen#
        add     dicPos, cnt
        cmp [LOC + full], dicPos
        jae 1f
            mov [LOC + full], dicPos
1:
        # len -= curLen#
        sub     sym, cnt
        mov     [LOC + remainLen_Loc], sym

        sub     t0_R, dic
        
        # pos = dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0)#
        sub     t0_R, rcx
        jae     1f

        mov     rcx, [LOC + dicBufSize]
        add     t0_R, rcx
        sub     rcx, t0_R
        cmp     cnt_R, rcx
        ja      copy_match_cross
1:
        ## if (curLen <= dicBufSize - pos)

# ---------- COPY MATCH FAST ----------
        add     t0_R, dic
        movzx   sym, byte ptr[t0_R]
        add     t0_R, cnt_R
        neg     cnt_R
copy_common:
        dec     dicDst

        IsMatchBranch_Pre
        inc     cnt_R
        jz      copy_end
MY_ALIGN_16
1:
        mov     byte ptr[cnt_R * 1 + dicDst], sym_L
        movzx   sym, byte ptr[cnt_R * 1 + t0_R]
        inc     cnt_R
        jnz     1b

copy_end:
lz_end_match:
        mov     byte ptr[dicDst], sym_L
        inc     dicDst
  
        CheckLimits
lz_end:
        IF_BIT_1_NOUP probs_state_R, pbPos_R, IsMatch, IsMatch_label



# ---------- LITERAL MATCHED ----------
                
        LIT_PROBS [LOC + lpMask]
        
        # matchByte = dic[dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0)]#
        mov     ecx, [LOC + rep0_Loc]
        # mov     dic, [LOC + dic_Spec]
        mov     [LOC + dicDst_Spec], dicDst
        
        # state -= (state < 10) ? 3 : 6#
        lea     t0, [state_R - 6 * PMULT]
        sub     state, 3 * PMULT
        cmp     state, 7 * PMULT
        cmovae  state, t0
        
        sub     dicDst, dic
        sub     dicDst, rcx
        jae     1f
        add     dicDst, [LOC + dicBufSize]
1:
        movzx   match, byte ptr[dic + dicDst * 1]

        LITM_0
        LITM
        LITM
        LITM
        LITM
        LITM
        LITM
        LITM_2

        mov     probs, [LOC + probs_Spec]
        IsMatchBranch_Pre
        mov     dicDst, [LOC + dicDst_Spec]
        mov     byte ptr[dicDst], sym_L
        inc     dicDst
        cmp [LOC + full], dicPos
        jae 1f
        mov [LOC + full], dicPos
1:
        CheckLimits
lit_matched_end:
        IF_BIT_1_NOUP probs_state_R, pbPos_R, IsMatch, IsMatch_label
        mov     lpMask_reg, [LOC + lpMask]
        sub     state, 3 * PMULT
        jmp     lit_start_2
        

# ---------- REP 0 LITERAL ----------
MY_ALIGN_16
IsRep0Short_label:
        UPDATE_0 probs_state_R, pbPos_R, IsRep0Long

        # dic[dicPos] = dic[dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0)]#
        mov     dic, [LOC + dic_Spec]
        mov     t0_R, dicDst
        mov     probBranch, [LOC + rep0_Loc]
        sub     t0_R, dic
        
        sub     probs, RepLenCoder * PMULT
        inc     dicPos
        # state = state < kNumLitStates ? 9 : 11#
        or      state, 1 * PMULT
        IsMatchBranch_Pre
       
        sub     t0_R, probBranch_R
        jae     1f
        add     t0_R, [LOC + dicBufSize]
1:
        movzx   sym, byte ptr[dic + t0_R * 1]
        cmp     [LOC + full], dicPos
        jae     lz_end_match
        mov     [LOC + full], dicPos
        jmp     lz_end_match
  
        
MY_ALIGN_16
IsRep_label:
        UPDATE_1 probs_state_R, 0, IsRep

        # The (checkDicSize == 0 && processedPos == 0) case was checked before in LzmaDec.c with kBadRepCode.
        # So we don't check it here.
        
        # state = state < kNumLitStates ? 8 : 11#
        cmp     state, kNumLitStates * PMULT
        mov     state, 8 * PMULT
        mov     probBranch, 11 * PMULT
        cmovae  state, probBranch

        # prob = probs + RepLenCoder#
        add     probs, RepLenCoder * PMULT
        
        IF_BIT_1 probs_state_R, 0, IsRepG0, IsRepG0_label
        IF_BIT_0_NOUP probs_state_R, pbPos_R, IsRep0Long, IsRep0Short_label
        UPDATE_1 probs_state_R, pbPos_R, IsRep0Long
        jmp     len_decode

MY_ALIGN_16
IsRepG0_label:
        UPDATE_1 probs_state_R, 0, IsRepG0
        mov     dist2, [LOC + rep0_Loc]
        mov     dist, [LOC + rep1_Loc]
        mov     [LOC + rep1_Loc], dist2
        
        IF_BIT_1 probs_state_R, 0, IsRepG1, IsRepG1_label
        mov     [LOC + rep0_Loc], dist
        jmp     len_decode
        
# MY_ALIGN_16
IsRepG1_label:
        UPDATE_1 probs_state_R, 0, IsRepG1
        mov     dist2, [LOC + rep2_Loc]
        mov     [LOC + rep2_Loc], dist
        
        IF_BIT_1 probs_state_R, 0, IsRepG2, IsRepG2_label
        mov     [LOC + rep0_Loc], dist2
        jmp     len_decode

# MY_ALIGN_16
IsRepG2_label:
        UPDATE_1 probs_state_R, 0, IsRepG2
        mov     dist, [LOC + rep3_Loc]
        mov     [LOC + rep3_Loc], dist2
        mov     [LOC + rep0_Loc], dist
        jmp     len_decode

        

# ---------- SPEC SHORT DISTANCE ----------

MY_ALIGN_16
short_dist:
        sub     ecx, 32 + 1
        jbe     decode_dist_end
        or      sym, 2
        shl     sym, cl
        lea     sym_R, [probs + sym_R * PMULT + SpecPos * PMULT + 1 * PMULT]
        mov     sym2, PMULT # step
MY_ALIGN_16
spec_loop:
        REV_1_VAR edx
        dec     ecx
        jnz     spec_loop

        mov     probs, [LOC + probs_Spec]
        sub     sym, sym2
        sub     sym, SpecPos * PMULT
        sub     sym_R, probs
        shr     sym, PSHIFT
        
        jmp     decode_dist_end


# ---------- COPY MATCH CROSS ----------
copy_match_cross:
        # t0_R - src pos
        # rcx - len to dicBufSize
        # cnt_R - total copy len

        mov     t1_R, t0_R         # srcPos
        mov     t0_R, dic
        mov     rcx, [LOC + dicBufSize]   #
        neg     cnt_R
1:
        movzx   sym, byte ptr[t1_R * 1 + t0_R]
        inc     t1_R
        mov     byte ptr[cnt_R * 1 + dicDst], sym_L
        inc     cnt_R
        cmp     t1_R, rcx
        jne     1b
        
        movzx   sym, byte ptr[t0_R]
        sub     t0_R, cnt_R
        jmp     copy_common


fin_ERROR:
        mov     [LOC + remainLen_Loc], len_temp
        mov     sym, 1
        jmp     fin

end_of_payload:
        cmp     sym, 0xFFFFFFFF # -1
        je      fin
        jmp     fin_ERROR

fin_OK:
        xor     sym, sym

fin:
        NORM

        mov     rcx, [LOC + lzmaPtr]

        mov     t0_R, [LOC + dict]
        mov     [t0_R + dict_pos], dicPos_R
        xor     t1_R, t1_R
        mov     t1, [LOC + full]
        mov     [t0_R + dict_full], t1_R
        mov     t0_R, [LOC + bufPosPtr]
        sub     buf, [LOC + buf_Loc]
        mov     [t0_R], buf
        mov     [GLOB + range_Spec], range
        mov     [GLOB + code_Spec], cod
        shr     state, PSHIFT
        mov     [GLOB + state_Spec], state

        mov     t0, [LOC + remainLen_Loc]
        mov     [GLOB + remainLen], t0
        mov     t0, [LOC + rep0_Loc]
        mov     [GLOB + rep0], t0
        mov     t0, [LOC + rep1_Loc]
        mov     [GLOB + rep1], t0
        mov     t0, [LOC + rep2_Loc]
        mov     [GLOB + rep2], t0
        mov     t0, [LOC + rep3_Loc]
        mov     [GLOB + rep3], t0

        mov     eax, sym
        
        mov     RSP, [LOC + Old_RSP]

        MY_POP_PRESERVED_REGS

        ret

.end
