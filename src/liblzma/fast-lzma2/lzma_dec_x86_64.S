# LzmaDecOpt.asm -- ASM version of LZMA_decodeReal_3() function
# 2018-02-06: Igor Pavlov : Public domain
#
# 3 - is the code compatibility version of LZMA_decodeReal_*()
# function for check at link time.
# That code is tightly coupled with LZMA_tryDummy()
# and with another functions in lzma2_dec.c file.
# CLzmaDec structure, (probs) array layout, input and output of
# LZMA_decodeReal_*() must be equal in both versions (C / ASM).

    .intel_syntax noprefix

# 7zAsm.asm -- ASM macros
# 2018-02-03 : Igor Pavlov : Public domain

  .equ REG_SIZE, 8
  .equ REG_LOGAR_SIZE, 3

  .equ x0, EAX
  .equ x1, ECX
  .equ x2, EDX
  .equ x3, EBX
  .equ x4, ESP
  .equ x5, EBP
  .equ x6, ESI
  .equ x7, EDI

  .equ x0_W, AX
  .equ x1_W, CX
  .equ x2_W, DX
  .equ x3_W, BX

  .equ x5_W, BP
  .equ x6_W, SI
  .equ x7_W, DI

  .equ x0_L, AL
  .equ x1_L, CL
  .equ x2_L, DL
  .equ x3_L, BL

  .equ x0_H, AH
  .equ x1_H, CH
  .equ x2_H, DH
  .equ x3_H, BH

  .equ x5_L, BPL
  .equ x6_L, SIL
  .equ x7_L, DIL

  .equ r0, RAX
  .equ r1, RCX
  .equ r2, RDX
  .equ r3, RBX
  .equ r4, RSP
  .equ r5, RBP
  .equ r6, RSI
  .equ r7, RDI
  .equ x8, r8d
  .equ x9, r9d
  .equ x10, r10d
  .equ x11, r11d
  .equ x12, r12d
  .equ x13, r13d
  .equ x14, r14d
  .equ x15, r15d


.if MS_x64_CALL
# for WIN64-x64 ABI:
.equ REG_PARAM_0, r1
.equ REG_PARAM_1, r2
.equ REG_PARAM_2, r8
.equ REG_PARAM_3, r9

.macro MY_PUSH_PRESERVED_REGS
    push    r3
    push    r5
    push    r6 # WIN64
    push    r7 # WIN64
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
    pop     r7 # WIN64
    pop     r6 # WIN64
    pop     r5
    pop     r3
.endm

.else
# for System V AMD64 ABI:
.equ REG_PARAM_0, r7
.equ REG_PARAM_1, r6
.equ REG_PARAM_2, r2
.equ REG_PARAM_3, r1

.macro MY_PUSH_PRESERVED_REGS
    push    r3
    push    r5
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
    pop     r5
    pop     r3
.endm

.endif



.macro MY_ALIGN  num:req
        .balign  \num
.endm

.macro MY_ALIGN_16
        MY_ALIGN 16
.endm

.macro MY_ALIGN_32
        MY_ALIGN 32
.endm

.macro MY_ALIGN_64
        MY_ALIGN 64
.endm


# .equ _LZMA_SIZE_OPT, 1

.ifdef _LZMA_PROB32
    .equ PSHIFT, 2
    .macro PLOAD dest, mem
        mov     \dest, dword ptr [\mem]
    .endm
    .macro PSTORE src, mem
        mov     dword ptr [\mem], \src
    .endm
.else
    .equ PSHIFT, 1
    .macro PLOAD dest, mem
        movzx   \dest, word ptr [\mem]
    .endm
    .macro PSTORE src, mem
        mov     word ptr [\mem], \src\()_W
    .endm
.endif

.equ PMULT, (1 SHL PSHIFT)
.equ PMULT_HALF, (1 SHL (PSHIFT - 1))
.equ PMULT_2, (1 SHL (PSHIFT + 1))


#       x0      range
#       x1      pbPos / (prob) TREE
#       x2      probBranch / prm (MATCHED) / pbPos / cnt
#       x3      sym
#====== r4 ===  RSP
#       x5      cod
#       x6      t1 NORM_CALC / probs_state / dist
#       x7      t0 NORM_CALC / prob2 IF_BIT_1
#       x8      state
#       x9      match (MATCHED) / sym2 / dist2 / lpMask_reg
#       x10     kBitModelTotal_reg
#       r11     probs
#       x12     offs (MATCHED) / dic / len_temp
#       x13     processedPos
#       x14     bit (MATCHED) / dicPos
#       r15     buf


.equ cod, x5
.equ cod_L, x5_L
.equ range, x0
.equ state, x8
.equ state_R, r8
.equ buf, r15
.equ processedPos, x13
.equ kBitModelTotal_reg, x10

.equ probBranch, x2
.equ probBranch_R, r2
.equ probBranch_W, x2_W

.equ pbPos, x1
.equ pbPos_R, r1

.equ cnt, x2
.equ cnt_R, r2

.equ lpMask_reg, x9
.equ dicPos, r14

.equ sym, x3
.equ sym_R, r3
.equ sym_L, x3_L

.equ probs, r11
.equ dic, r12

.equ t0, x7
.equ t0_W, x7_W
.equ t0_R, r7

.equ prob2, t0
.equ prob2_W, t0_W

.equ t1, x6
.equ t1_R, r6

.equ probs_state, t1
.equ probs_state_R, t1_R

.equ prm, r2
.equ match, x9
.equ match_R, r9
.equ offs, x12
.equ offs_R, r12
.equ bit, x14
.equ bit_R, r14

.equ sym2, x9
.equ sym2_R, r9

.equ len_temp, x12

.equ dist, sym
.equ dist2, x9



.equ kNumBitModelTotalBits, 11
.equ kBitModelTotal, (1 SHL kNumBitModelTotalBits)
.equ kNumMoveBits, 5
.equ kBitModelOffset, ((1 SHL kNumMoveBits) - 1)
.equ kTopValue, (1 SHL 24)

.macro NORM_2
        # movzx   t0, BYTE PTR [buf]
        shl     cod, 8
        mov     cod_L, BYTE PTR [buf]
        shl     range, 8
        # or      cod, t0
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
        PLOAD   x1, (probs + 256 * PMULT + bit_R * 1 + 1 * PMULT)
        lea     prm, [probs + 256 * PMULT + bit_R * 1 + 1 * PMULT]
        # lea     prm, [probs + 256 * PMULT + 1 * PMULT]
        # add     prm, bit_R
        xor     offs, bit
        add     match, match

        NORM_CALC x1

        cmovae  offs, bit
        mov     bit, match
        cmovae  range, t0
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        cmovb   t0, kBitModelTotal_reg
        mov     sym, 0
        PUP_SUB x1, prm, (-2-1)
.endm


.macro LITM
        and     bit, offs
        lea     prm, [probs + offs_R * 1]
        add     prm, bit_R
        PLOAD   x1, (prm + sym_R * PMULT)
        xor     offs, bit
        add     sym, sym
        add     match, match

        NORM_CALC x1

        cmovae  offs, bit
        mov     bit, match
        cmovae  range, t0
        PUP_COD x1, (prm + t1_R * PMULT_HALF), (- 1)
.endm


.macro LITM_2
        and     bit, offs
        lea     prm, [probs + offs_R * 1]
        add     prm, bit_R
        PLOAD   x1, (prm + sym_R * PMULT)
        add     sym, sym

        NORM_CALC x1

        cmovae  range, t0
        PUP_COD x1, (prm + t1_R * PMULT_HALF), (256 - 1)
.endm


# ---------- REVERSE BITS ----------

.macro REV_0 prob:req, probNext:req
        # PLOAD   prob, probs + 1 * PMULT
        # lea     sym2_R, [probs + 2 * PMULT]
        # PLOAD   probNext, probs + 2 * PMULT
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
        # mov     t1, kBitModelTotal
        # cmovb   t0, t1
        cmovb   t0, kBitModelTotal_reg
        add     sym2, sym2
        PUP \prob, probs
.endm




.macro LIT_PROBS lpMaskParam:req
        # prob += (UInt32)3 * ((((processedPos << 8) + dic[(dicPos == 0 ? dicBufSize : dicPos) - 1]) & lpMask) << lc)#
        mov     t0, processedPos
        shl     t0, 8
        add     sym, t0
        and     sym, \lpMaskParam
        add     probs_state_R, pbPos_R
        mov     x1, [LOC + lc2]
        lea     sym, dword ptr[sym_R + 2 * sym_R]
        add     probs, Literal * PMULT
        shl     sym, x1_L
        add     probs, sym_R
        UPDATE_0 probs_state_R, 0, IsMatch
        inc     processedPos
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

.equ Literal, 0
.equ IsMatch, (Literal + kLiteralCodersMax * kLiteralCoderSize)
.equ IsRep, (IsMatch + (kNumStates2 SHL kNumPosBitsMax))
.equ IsRepG0, (IsRep + kNumStates)
.equ IsRepG1, (IsRepG0 + kNumStates)
.equ IsRepG2, (IsRepG1 + kNumStates)
.equ IsRep0Long, (IsRepG2 + kNumStates)
.equ PosSlot, (IsRep0Long + (kNumStates2 SHL kNumPosBitsMax))
.equ SpecPos, (PosSlot + (kNumLenToPosStates SHL kNumPosSlotBits))
.equ kAlign, (SpecPos + kNumFullDistances)
.equ LenCoder, (kAlign + kAlignTableSize)
.equ RepLenCoder, (LenCoder + kNumLenProbs)
.equ NUM_PROBS, (RepLenCoder + kNumLenProbs)

# lzma_dict:
    .equ dict_buf, 0
    .equ dict_pos, 8
    .equ dict_full, 16
    .equ dict_limit, 24
    .equ dict_size, 32


# CLzmaDec_Asm:
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


# CLzmaDec_Asm_Loc:
    .equ Old_RSP, 0
    .equ lzmaPtr, 8
    .equ _pad0_, 16
    .equ _pad1_, 24
    .equ _pad2_, 32
    .equ dicBufSize, 40
    .equ probs_Spec, 48
    .equ dic_Spec_Loc, 56
        
    .equ limit, 64
    .equ bufPosPtr, 72
    .equ bufLimit, 80
    .equ lc2, 88
    .equ lpMask, 92
    .equ pbMask, 96

    .equ remainLen_Loc, 100
    .equ dicPos_Spec_Loc, 104
    .equ rep0_Loc, 112
    .equ rep1_Loc, 116
    .equ rep2_Loc, 120
    .equ rep3_Loc, 124
    .equ buf_Loc, 128
    .equ dict, 136
    .equ full, 144

    .equ Sizeof_CLzmaDec_Asm_Loc, 152

.equ GLOB_2,  sym_R
.equ GLOB,    r1
.equ LOC_0,   r0
.equ LOC,     RSP


.macro IsMatchBranch_Pre reg
        # prob = probs + IsMatch + (state << kNumPosBitsMax) + posState#
        mov     pbPos, [LOC + pbMask]
        and     pbPos, processedPos
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
        cmp     dicPos, [LOC + limit]
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
        .global LZMA_decodeReal_3
LZMA_decodeReal_3:
MY_PUSH_PRESERVED_REGS

        lea     r0, [RSP - Sizeof_CLzmaDec_Asm_Loc]
        and     r0, -128
        mov     r5, RSP
        mov     RSP, r0
        mov     [LOC_0 + Old_RSP], r5
        mov     t0_R, [r5 + 8]
        add     t0_R, PARAM_buf
        mov     [LOC_0 + bufLimit], t0_R
        mov     [LOC_0 + lzmaPtr], PARAM_lzma
        mov     [LOC_0 + dict], PARAM_dict
        mov     [LOC_0 + bufPosPtr], PARAM_bufPosPtr
        mov     [LOC_0 + buf_Loc], PARAM_buf
        mov     buf, [PARAM_bufPosPtr]
        add     buf, PARAM_buf
        mov     dic, [PARAM_dict + dict_buf]
        mov     dicPos, [PARAM_dict + dict_pos]
        mov     processedPos, [PARAM_dict + dict_pos]

        add     dicPos, dic
        mov     [LOC_0 + dicPos_Spec_Loc], dicPos
        mov     [LOC_0 + dic_Spec_Loc], dic
        
        mov     t0_R, [PARAM_dict + dict_limit]
        add     t0_R, dic
        mov     [LOC_0 + limit], t0_R

        mov     t0_R, [PARAM_dict + dict_size]
        mov     [LOC_0 + dicBufSize], t0_R
        mov     t0_R, [PARAM_dict + dict_full]
        add     t0_R, dic
        mov     [LOC_0 + full], t0_R

        mov     dword ptr [LOC_0 + remainLen_Loc], 0  # remainLen must be ZERO

        mov     sym_R, PARAM_lzma  #  CLzmaDec_Asm_Loc pointer for GLOB_2
        mov     [LOC_0 + probs_Spec], sym_R
        mov     probs, sym_R

        add     sym_R, (NUM_PROBS SHL PSHIFT)

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

        mov     x1_L, [GLOB_2 + lc]
        add     x1_L, PSHIFT
        mov     [LOC_0 + lc2], x1
        mov     t0, [GLOB_2 + lpMask_Spec]
        mov     [LOC_0 + lpMask], t0
        mov     lpMask_reg, t0
        
        mov     state, [GLOB_2 + state_Spec]
        shl     state, PSHIFT

        mov     range, [GLOB_2 + range_Spec]
        mov     cod,   [GLOB_2 + code_Spec]
        mov     kBitModelTotal_reg, kBitModelTotal
        xor     sym, sym

        ## if (processedPos != 0)
        or      processedPos, processedPos
        jz      1f
        
        mov     t0_R, [LOC + dicBufSize]
        add     t0_R, dic
        cmp     dicPos, dic
        cmovnz  t0_R, dicPos
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

    .ifdef _LZMA_SIZE_OPT

        PLOAD   x1, probs + 1 * PMULT
        mov     sym, 1
MY_ALIGN_16
lit_loop:
        BIT_1   x1, x2
        mov     x1, x2
        cmp     sym, 127
        jbe     lit_loop
        
    .else
        
        BIT_0   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        
    .endif

        BIT_2   x2, (256 - 1)
        
        # mov     dic, [LOC + dic_Spec_Loc]
        mov     probs, [LOC + probs_Spec]
        IsMatchBranch_Pre
        mov     byte ptr[dicPos], sym_L
        inc     dicPos
        mov     t0_R, [LOC + full]
        cmp     dicPos, t0_R
        cmova   t0_R, dicPos
        mov     [LOC + full], t0_R

        CheckLimits
lit_end:
        IF_BIT_0_NOUP probs_state_R, pbPos_R, IsMatch, lit_start

        # jmp     IsMatch_label
        
# ---------- MATCHES ----------
# MY_ALIGN_32
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
        PLOAD   x1, (probs + 1 * PMULT)

MY_ALIGN_32
len8_loop:
        BIT_1   x1, x2
        mov     x1, x2
        cmp     sym, 64
        jb      len8_loop
        
        mov     len_temp, (kLenNumHighSymbols - kLenNumLowSymbols * 2) - 1 - kMatchMinLen
        jmp     len_mid_2
        
MY_ALIGN_32
len_mid_0:
        UPDATE_0 probs, 0, 0
        add     probs, pbPos_R
        BIT_0   x2, x1
len_mid_2:
        BIT_1   x1, x2
        BIT_2   x2, len_temp
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
        # mov     [LOC + remainLen_Loc], sym
        mov     len_temp, sym

    .ifdef _LZMA_SIZE_OPT

        PLOAD   x1, probs + 1 * PMULT
        mov     sym, 1
MY_ALIGN_16
slot_loop:
        BIT_1   x1, x2
        mov     x1, x2
        cmp     sym, 32
        jb      slot_loop
        
    .else
        
        BIT_0   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        
    .endif
        
        mov     x1, sym
        BIT_2   x2, 64-1

        and     sym, 3
        mov     probs, [LOC + probs_Spec]
        cmp     x1, 32 + kEndPosModelIndex / 2
        jb      short_dist

        #  unsigned numDirectBits = (unsigned)(((distance >> 1) - 1))#
        sub     x1, (32 + 1 + kNumAlignBits)
        #  distance = (2 | (distance & 1))#
        or      sym, 2
        PLOAD   x2, (probs + 1 * PMULT)
        shl     sym, kNumAlignBits + 1
        lea     sym2_R, [probs + 2 * PMULT]
        
        jmp     direct_norm
        # lea     t1, [sym_R + (1 SHL kNumAlignBits)]
        # cmp     range, kTopValue
        # jb      direct_norm
        
# ---------- DIRECT DISTANCE ----------
MY_ALIGN_32
direct_loop:
        shr     range, 1
        mov     t0, cod
        sub     cod, range
        cmovs   cod, t0
        cmovns  sym, t1
        
#        sub     cod, range
#        mov     x2, cod
#        sar     x2, 31
#        lea     sym, dword ptr [r2 + sym_R * 2 + 1]
#        and     x2, range
#        add     cod, x2

        dec     x1
        je      direct_end

        add     sym, sym
direct_norm:
        lea     t1, [sym_R + (1 SHL kNumAlignBits)]
        cmp     range, kTopValue
        jae     near ptr direct_loop
        # we align for 32 here with "near ptr" command above
        NORM_2
        jmp     direct_loop

MY_ALIGN_32
direct_end:
        #  prob =  + kAlign#
        #  distance <<= kNumAlignBits#
        REV_0   x2, x1
        REV_1   x1, x2, 2
        REV_1   x2, x1, 4
        REV_2   x1, 8

decode_dist_end:

        ## if (distance >= (checkDicSize == 0 ? processedPos: checkDicSize))

        cmp     sym, processedPos
        jae     end_of_payload
        
        # rep3 = rep2#
        # rep2 = rep1#
        # rep1 = rep0#
        # rep0 = distance + 1#

        inc     sym
        mov     t0, [LOC + rep0_Loc]
        mov     t1, [LOC + rep1_Loc]
        mov     x1, [LOC + rep2_Loc]
        mov     [LOC + rep0_Loc], sym
        # mov     sym, [LOC + remainLen_Loc]
        mov     sym, len_temp
        mov     [LOC + rep1_Loc], t0
        mov     [LOC + rep2_Loc], t1
        mov     [LOC + rep3_Loc], x1
        
        # state = (state < kNumStates + kNumLitStates) ? kNumLitStates : kNumLitStates + 3#
        cmp     state, (kNumStates + kNumLitStates) * PMULT
        mov     state, kNumLitStates * PMULT
        mov     t0, (kNumLitStates + 3) * PMULT
        cmovae  state, t0

        
# ---------- COPY MATCH ----------
copy_match:

        # len += kMatchMinLen#
        # add     sym, kMatchMinLen

        ## if ((rem = limit - dicPos) == 0)
        # {
        #   p->dicPos = dicPos#
        #   return SZ_ERROR_DATA#
        # }
        mov     cnt_R, [LOC + limit]
        sub     cnt_R, dicPos
        jz      fin_ERROR

        # curLen = ((rem < len) ? (unsigned)rem : len)#
        cmp     cnt_R, sym_R
        # cmovae  cnt_R, sym_R # 64-bit
        cmovae  cnt, sym # 32-bit

        mov     dic, [LOC + dic_Spec_Loc]

        mov     t0_R, dicPos
        add     dicPos, cnt_R
        mov     r1, [LOC + full]
        cmp     dicPos, r1
        cmova   r1, dicPos
        mov     [LOC + full], r1
        mov     x1, [LOC + rep0_Loc]
        # processedPos += curLen#
        add     processedPos, cnt
        # len -= curLen#
        sub     sym, cnt
        mov     [LOC + remainLen_Loc], sym

        sub     t0_R, dic
        
        # pos = dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0)#
        sub     t0_R, r1
        jae     1f

        mov     r1, [LOC + dicBufSize]
        add     t0_R, r1
        sub     r1, t0_R
        cmp     cnt_R, r1
        ja      copy_match_cross
1:
        ## if (curLen <= dicBufSize - pos)

# ---------- COPY MATCH FAST ----------
        # Byte *dest = dic + dicPos#
        # mov     r1, dic
        # ptrdiff_t src = (ptrdiff_t)pos - (ptrdiff_t)dicPos#
        # sub   t0_R, dicPos
        # dicPos += curLen#

        # const Byte *lim = dest + curLen#
        add     t0_R, dic
        movzx   sym, byte ptr[t0_R]
        add     t0_R, cnt_R
        neg     cnt_R
        # lea     r1, [dicPos - 1]
copy_common:
        dec     dicPos
        # cmp   [LOC + rep0], 1
        # je    rep0Label

        # t0_R - src_lim
        # r1 - dest_lim - 1
        # cnt_R - (-cnt)

        IsMatchBranch_Pre
        inc     cnt_R
        jz      copy_end
MY_ALIGN_16
1:
        mov     byte ptr[cnt_R * 1 + dicPos], sym_L
        movzx   sym, byte ptr[cnt_R * 1 + t0_R]
        inc     cnt_R
        jnz     1b

copy_end:
lz_end_match:
        mov     byte ptr[dicPos], sym_L
        inc     dicPos
  
        # IsMatchBranch_Pre
        CheckLimits
lz_end:
        IF_BIT_1_NOUP probs_state_R, pbPos_R, IsMatch, IsMatch_label



# ---------- LITERAL MATCHED ----------
                
        LIT_PROBS [LOC + lpMask]
        
        # matchByte = dic[dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0)]#
        mov     x1, [LOC + rep0_Loc]
        # mov     dic, [LOC + dic_Spec_Loc]
        mov     [LOC + dicPos_Spec_Loc], dicPos
        
        # state -= (state < 10) ? 3 : 6#
        lea     t0, [state_R - 6 * PMULT]
        sub     state, 3 * PMULT
        cmp     state, 7 * PMULT
        cmovae  state, t0
        
        sub     dicPos, dic
        sub     dicPos, r1
        jae     1f
        add     dicPos, [LOC + dicBufSize]
1:
#        xor     t0, t0
#        sub     dicPos, r1
#        cmovb   t0_R, [LOC + dicBufSize]
        
        movzx   match, byte ptr[dic + dicPos * 1]

    .ifdef _LZMA_SIZE_OPT

        mov     offs, 256 * PMULT
        shl     match, (PSHIFT + 1)
        mov     bit, match
        mov     sym, 1
MY_ALIGN_16
litm_loop:
        LITM
        cmp     sym, 256
        jb      litm_loop
        sub     sym, 256
        
    .else
        
        LITM_0
        LITM
        LITM
        LITM
        LITM
        LITM
        LITM
        LITM_2
        
    .endif
        
        mov     probs, [LOC + probs_Spec]
        IsMatchBranch_Pre
        # mov     dic, [LOC + dic_Spec_Loc]
        mov     dicPos, [LOC + dicPos_Spec_Loc]
        mov     byte ptr[dicPos], sym_L
        inc     dicPos
        
        CheckLimits
lit_matched_end:
        IF_BIT_1_NOUP probs_state_R, pbPos_R, IsMatch, IsMatch_label
        # IsMatchBranch
        mov     lpMask_reg, [LOC + lpMask]
        sub     state, 3 * PMULT
        jmp     lit_start_2
        


# ---------- REP 0 LITERAL ----------
MY_ALIGN_32
IsRep0Short_label:
        UPDATE_0 probs_state_R, pbPos_R, IsRep0Long

        # dic[dicPos] = dic[dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0)]#
        mov     dic, [LOC + dic_Spec_Loc]
        mov     t0_R, dicPos
        mov     probBranch, [LOC + rep0_Loc]
        sub     t0_R, dic
        
        sub     probs, RepLenCoder * PMULT
        inc     processedPos
        # state = state < kNumLitStates ? 9 : 11#
        or      state, 1 * PMULT
        IsMatchBranch_Pre
       
        sub     t0_R, probBranch_R
        jae     1f
        add     t0_R, [LOC + dicBufSize]
1:
        movzx   sym, byte ptr[dic + t0_R * 1]
        jmp     lz_end_match
  
        
MY_ALIGN_32
IsRep_label:
        UPDATE_1 probs_state_R, 0, IsRep

        # The (checkDicSize == 0 && processedPos == 0) case was checked before in LzmaDec.c with kBadRepCode.
        # So we don't check it here.
        
        # mov     t0, processedPos
        # or      t0, [LOC + checkDicSize]
        # jz      fin_ERROR_2

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

MY_ALIGN_32
IsRepG0_label:
        UPDATE_1 probs_state_R, 0, IsRepG0
        mov     dist2, [LOC + rep0_Loc]
        mov     dist, [LOC + rep1_Loc]
        mov     [LOC + rep1_Loc], dist2
        
        IF_BIT_1 probs_state_R, 0, IsRepG1, IsRepG1_label
        mov     [LOC + rep0_Loc], dist
        jmp     len_decode
        
# MY_ALIGN_32
IsRepG1_label:
        UPDATE_1 probs_state_R, 0, IsRepG1
        mov     dist2, [LOC + rep2_Loc]
        mov     [LOC + rep2_Loc], dist
        
        IF_BIT_1 probs_state_R, 0, IsRepG2, IsRepG2_label
        mov     [LOC + rep0_Loc], dist2
        jmp     len_decode

# MY_ALIGN_32
IsRepG2_label:
        UPDATE_1 probs_state_R, 0, IsRepG2
        mov     dist, [LOC + rep3_Loc]
        mov     [LOC + rep3_Loc], dist2
        mov     [LOC + rep0_Loc], dist
        jmp     len_decode

        

# ---------- SPEC SHORT DISTANCE ----------

MY_ALIGN_32
short_dist:
        sub     x1, 32 + 1
        jbe     decode_dist_end
        or      sym, 2
        shl     sym, x1_L
        lea     sym_R, [probs + sym_R * PMULT + SpecPos * PMULT + 1 * PMULT]
        mov     sym2, PMULT # step
MY_ALIGN_32
spec_loop:
        REV_1_VAR x2
        dec     x1
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
        # r1 - len to dicBufSize
        # cnt_R - total copy len

        mov     t1_R, t0_R         # srcPos
        mov     t0_R, dic
        mov     r1, [LOC + dicBufSize]   #
        neg     cnt_R
1:
        movzx   sym, byte ptr[t1_R * 1 + t0_R]
        inc     t1_R
        mov     byte ptr[cnt_R * 1 + dicPos], sym_L
        inc     cnt_R
        cmp     t1_R, r1
        jne     1b
        
        movzx   sym, byte ptr[t0_R]
        sub     t0_R, cnt_R
        jmp     copy_common




fin_ERROR:
        mov     [LOC + remainLen_Loc], len_temp
# fin_ERROR_2:
        mov     sym, 1
        jmp     fin

end_of_payload:
        cmp     sym, 0xFFFFFFFF # -1
        jne     fin_ERROR

        mov     dword ptr[LOC + remainLen_Loc], kMatchSpecLenStart
        sub     state, kNumStates * PMULT

fin_OK:
        xor     sym, sym

fin:
        NORM

        mov     r1, [LOC + lzmaPtr]

        mov     t0_R, [LOC + dict]
        sub     dicPos, [LOC + dic_Spec_Loc]
        mov     [t0_R + dict_pos], dicPos
        mov     t1_R, [LOC + full]
        sub     t1_R, [LOC + dic_Spec_Loc]
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

        mov     x0, sym
        
        mov     RSP, [LOC + Old_RSP]

MY_POP_PRESERVED_REGS

        ret
# _TEXT$LZMADECOPT ENDS

.end
