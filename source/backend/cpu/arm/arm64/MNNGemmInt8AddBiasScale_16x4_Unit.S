//
//  MNNGemmInt8AddBiasScale_16x4_Unit.S
//  MNN
//
//  Created by MNN on 2019/06/11.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#ifdef __aarch64__

#include "MNNAsmGlobal.h"

.text
.align 5

.macro MLA_WEIGHTZERO d0, s0, s1, idx // idx for xKernelSum
    fmla \d0\().4s, \s1\().4s, \s0\().s[\idx]
.endm
.macro ReLU_FP32_4 s0, s1, s2, s3, z0, z1 // z0:min z1:max
    fmin \s0\().4s, \s0\().4s, \z1\().4s
    fmin \s1\().4s, \s1\().4s, \z1\().4s
    fmin \s2\().4s, \s2\().4s, \z1\().4s
    fmin \s3\().4s, \s3\().4s, \z1\().4s
    fmax \s0\().4s, \s0\().4s, \z0\().4s
    fmax \s1\().4s, \s1\().4s, \z0\().4s
    fmax \s2\().4s, \s2\().4s, \z0\().4s
    fmax \s3\().4s, \s3\().4s, \z0\().4s
.endm
.macro ReLU_FP32_3 s0, s1, s2, z0, z1 // z0:min z1:max
    fmin \s0\().4s, \s0\().4s, \z1\().4s
    fmin \s1\().4s, \s1\().4s, \z1\().4s
    fmin \s2\().4s, \s2\().4s, \z1\().4s
    fmax \s0\().4s, \s0\().4s, \z0\().4s
    fmax \s1\().4s, \s1\().4s, \z0\().4s
    fmax \s2\().4s, \s2\().4s, \z0\().4s
.endm
.macro ReLU_FP32_2 s0, s1, z0, z1 // z0:min z1:max
    fmin \s0\().4s, \s0\().4s, \z1\().4s
    fmin \s1\().4s, \s1\().4s, \z1\().4s
    fmax \s0\().4s, \s0\().4s, \z0\().4s
    fmax \s1\().4s, \s1\().4s, \z0\().4s
.endm
.macro ReLU_FP32_1 s0, z0, z1 // z0:min z1:max
    fmin \s0\().4s, \s0\().4s, \z1\().4s
    fmax \s0\().4s, \s0\().4s, \z0\().4s
.endm
.macro MUL_SCALE4 s, d0, d1, d2, d3
    fmul \d0\().4s, \d0\().4s, \s\().4s
    fmul \d1\().4s, \d1\().4s, \s\().4s
    fmul \d2\().4s, \d2\().4s, \s\().4s
    fmul \d3\().4s, \d3\().4s, \s\().4s
.endm
.macro MUL_SCALE3 s, d0, d1, d2
    fmul \d0\().4s, \d0\().4s, \s\().4s
    fmul \d1\().4s, \d1\().4s, \s\().4s
    fmul \d2\().4s, \d2\().4s, \s\().4s
.endm
.macro MUL_SCALE2 s, d0, d1
    fmul \d0\().4s, \d0\().4s, \s\().4s
    fmul \d1\().4s, \d1\().4s, \s\().4s
.endm
.macro MUL_SCALE1 s, d0
    fmul \d0\().4s, \d0\().4s, \s\().4s
.endm
.macro MUL_EXTRA_SCALE s, d0, d1, d2, d3
    fmul \d0\().4s, \d0\().4s, \s\().s[0]
    fmul \d1\().4s, \d1\().4s, \s\().s[1]
    fmul \d2\().4s, \d2\().4s, \s\().s[2]
    fmul \d3\().4s, \d3\().4s, \s\().s[3]
.endm

asm_function MNNGemmInt8AddBiasScale_16x4_Unit

/* 
struct QuanPostTreatParameters {
    const float* scale;
    const float* biasFloat;
    int32_t maxValue;
    int32_t minValue;
    int32_t useInt8 = 1; // Save result as int8_t dataType; otherwise float32.
    float roundValuePos = 0.5f;
    float roundValueNeg = -0.5f;
    float* srcKernelSum;
    float* weightQuanBias;
    float* fp32minmax;
    ssize_t blockNum;
    const int32_t* bias;
    float* extraScale;
};
*/

//void MNNGemmInt8AddBiasScale_16x4_Unit(int8_t* dst, const int8_t* src, const int8_t* weight, size_t src_depth_quad, size_t dst_step,
//                                              size_t dst_depth_quad, const QuanPostTreatParameters* post, size_t realSize) {

//Auto: x0: dst*, x1: src*, x2:weight*, x3: src_depth_quad, x4: dst_step, 
// x5: dst_depth_quad, x6: post, x7: realSize

//Load from post:
// x7: scale, x10: bias, w11: maxValue, w6: minValue, w13: UseInt8, x14: srcKernelSum, x12: weightQuantBias
mov x8, x7
mov x15, x6
ldr x7, [x15, #0]
ldr x10, [x15, #8]
ldr w11, [x15, #16]
ldr w6, [x15, #20]
ldr w13, [x15, #24]
ldr x14, [x15, #40] // srcKernelSum
ldr x12, [x15, #48] // weightQuantBias

stp d14, d15, [sp, #(-16 * 8)]!
stp d12, d13, [sp, #(16 * 1)]
stp d10, d11, [sp, #(16 * 2)]
stp d8,  d9,  [sp, #(16 * 3)]
stp x19, x20, [sp, #(16 * 4)]
stp x21, x22, [sp, #(16 * 5)]
stp x23, x24, [sp, #(16 * 6)]

ldr x19, [x15, #56] // fp32 min max
ldr x21, [x15, #64] // blockNum
ldr x23, [x15, #80] // extraScale
lsl x21, x3, #6    // src_depth_quad* SRC_UNIT * UNIT * sizeof(int8_t)
add x20, x19, #4
lsl x24, x8, #4    // eDest * SRC_UNIT
Start:
cmp x8, #3
beq L3Dz

cmp x8, #2
beq L2Dz

cmp x8, #1
beq L1Dz

cmp w13, #1
bne L4LoopDz

L4LoopDz:
    mov x8, x1
    mov x22, x2
    ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64
    ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x1], #64
    
    smull v8.8h, v0.8b, v4.8b
    smull v9.8h, v1.8b, v4.8b
    smull v10.8h, v2.8b, v4.8b
    smull v11.8h, v3.8b, v4.8b
    smull v12.8h, v0.8b, v5.8b
    smull v13.8h, v1.8b, v5.8b
    smull v14.8h, v2.8b, v5.8b
    smull v15.8h, v3.8b, v5.8b

    smlal2 v8.8h, v0.16b, v4.16b
    smlal2 v9.8h, v1.16b, v4.16b
    smlal2 v10.8h, v2.16b, v4.16b
    smlal2 v11.8h, v3.16b, v4.16b
    smlal2 v12.8h, v0.16b, v5.16b
    smlal2 v13.8h, v1.16b, v5.16b
    smlal2 v14.8h, v2.16b, v5.16b
    smlal2 v15.8h, v3.16b, v5.16b

    L4Initialize:
        saddlp v16.4s, v8.8h
        saddlp v17.4s, v9.8h
        saddlp v18.4s, v10.8h
        saddlp v19.4s, v11.8h
        saddlp v20.4s, v12.8h
        saddlp v21.4s, v13.8h
        saddlp v22.4s, v14.8h
        saddlp v23.4s, v15.8h

        smull v8.8h, v0.8b, v6.8b
        smull v9.8h, v1.8b, v6.8b
        smull v10.8h, v2.8b, v6.8b
        smull v11.8h, v3.8b, v6.8b
        smull v12.8h, v0.8b, v7.8b
        smull v13.8h, v1.8b, v7.8b
        smull v14.8h, v2.8b, v7.8b
        smull v15.8h, v3.8b, v7.8b
        subs x9, x3, #1
        smlal2 v8.8h,  v0.16b, v6.16b
        smlal2 v9.8h,  v1.16b, v6.16b
        smlal2 v10.8h, v2.16b, v6.16b
        smlal2 v11.8h, v3.16b, v6.16b
        smlal2 v12.8h, v0.16b, v7.16b
        smlal2 v13.8h, v1.16b, v7.16b
        smlal2 v14.8h, v2.16b, v7.16b
        smlal2 v15.8h, v3.16b, v7.16b

        saddlp v24.4s, v8.8h
        saddlp v25.4s, v9.8h
        saddlp v26.4s, v10.8h
        saddlp v27.4s, v11.8h
        saddlp v28.4s, v12.8h
        saddlp v29.4s, v13.8h
        saddlp v30.4s, v14.8h
        saddlp v31.4s, v15.8h
    L4InitializeEnd:
        beq ComputeSum
    
    L4LoopSz:
        ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x1], #64
        ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64

        smull v8.8h, v0.8b, v4.8b
        smull v9.8h, v1.8b, v4.8b
        smull v10.8h, v2.8b, v4.8b
        smull v11.8h, v3.8b, v4.8b
        smull v12.8h, v0.8b, v5.8b
        smull v13.8h, v1.8b, v5.8b
        smull v14.8h, v2.8b, v5.8b
        smull v15.8h, v3.8b, v5.8b

        smlal2 v8.8h, v0.16b, v4.16b
        smlal2 v9.8h, v1.16b, v4.16b
        smlal2 v10.8h, v2.16b, v4.16b
        smlal2 v11.8h, v3.16b, v4.16b
        smlal2 v12.8h, v0.16b, v5.16b
        smlal2 v13.8h, v1.16b, v5.16b
        smlal2 v14.8h, v2.16b, v5.16b
        smlal2 v15.8h, v3.16b, v5.16b

        sadalp v16.4s, v8.8h
        sadalp v17.4s, v9.8h
        sadalp v18.4s, v10.8h
        sadalp v19.4s, v11.8h
        sadalp v20.4s, v12.8h
        sadalp v21.4s, v13.8h
        sadalp v22.4s, v14.8h
        sadalp v23.4s, v15.8h

        smull v8.8h, v0.8b, v6.8b
        smull v9.8h, v1.8b, v6.8b
        smull v10.8h, v2.8b, v6.8b
        smull v11.8h, v3.8b, v6.8b
        smull v12.8h, v0.8b, v7.8b
        smull v13.8h, v1.8b, v7.8b
        smull v14.8h, v2.8b, v7.8b
        smull v15.8h, v3.8b, v7.8b

        subs x9, x9, #1

        smlal2 v8.8h, v0.16b, v6.16b
        smlal2 v9.8h, v1.16b, v6.16b
        smlal2 v10.8h, v2.16b, v6.16b
        smlal2 v11.8h, v3.16b, v6.16b
        smlal2 v12.8h, v0.16b, v7.16b
        smlal2 v13.8h, v1.16b, v7.16b
        smlal2 v14.8h, v2.16b, v7.16b
        smlal2 v15.8h, v3.16b, v7.16b

        sadalp v24.4s, v8.8h
        sadalp v25.4s, v9.8h
        sadalp v26.4s, v10.8h
        sadalp v27.4s, v11.8h
        sadalp v28.4s, v12.8h
        sadalp v29.4s, v13.8h
        sadalp v30.4s, v14.8h
        sadalp v31.4s, v15.8h

        bne L4LoopSz

    ComputeSum:

    addp v4.4s, v16.4s, v17.4s
    addp v5.4s, v18.4s, v19.4s
    addp v6.4s, v20.4s, v21.4s
    addp v7.4s, v22.4s, v23.4s
    addp v8.4s, v24.4s, v25.4s
    addp v9.4s, v26.4s, v27.4s
    addp v10.4s, v28.4s, v29.4s
    addp v11.4s, v30.4s, v31.4s

    addp v12.4s, v4.4s, v5.4s
    addp v13.4s, v6.4s, v7.4s
    addp v14.4s, v8.4s, v9.4s
    addp v15.4s, v10.4s, v11.4s

    L4Quan:
    ld1 {v1.4s}, [x7], #16 // scalefuse
    ld1 {v20.4s}, [x14] // srcKernelSum
    ld1 {v21.4s}, [x12], #16 // weightQuanZero

    scvtf v4.4s, v12.4s
    scvtf v5.4s, v13.4s
    scvtf v6.4s, v14.4s
    scvtf v7.4s, v15.4s

    cbz x23, TILE4_MUL_OHE_SCALE
    ld1 {v2.4s}, [x23]
    MUL_EXTRA_SCALE v2, v4, v5, v6, v7

    TILE4_MUL_OHE_SCALE:
    MUL_SCALE4 v1, v4, v5, v6, v7

    MLA_WEIGHTZERO v4, v20, v21, 0
    MLA_WEIGHTZERO v5, v20, v21, 1
    MLA_WEIGHTZERO v6, v20, v21, 2
    MLA_WEIGHTZERO v7, v20, v21, 3

    cmp w13, #1
    beq L4QuantUseInt8

    L4_Add_BIAS:
    cbz x10, L4_ADD_DSTV
    ld1 {v0.4s}, [x10], #16
    fadd v4.4s, v4.4s, v0.4s
    fadd v5.4s, v5.4s, v0.4s
    fadd v6.4s, v6.4s, v0.4s
    fadd v7.4s, v7.4s, v0.4s
    b L4_POST

    L4_ADD_DSTV:
    ld1 {v8.4s, v9.4s, v10.4s, v11.4s}, [x0]
    fadd v4.4s, v4.4s, v8.4s
    fadd v5.4s, v5.4s, v9.4s
    fadd v6.4s, v6.4s, v10.4s
    fadd v7.4s, v7.4s, v11.4s

    L4_POST:
    cbz x19, L4_STORE
    ld1r {v26.4s}, [x19] // f32 min
    ld1r {v27.4s}, [x20] // f32 max
    ReLU_FP32_4 v4, v5, v6, v7, v26, v27

    L4_STORE:
    st1 {v4.4s, v5.4s, v6.4s, v7.4s}, [x0], x4
    b L4LoopCheck

    L4QuantUseInt8:
    ld1 {v0.4s}, [x10], #16
    fadd v4.4s, v4.4s, v0.4s
    fadd v5.4s, v5.4s, v0.4s
    fadd v6.4s, v6.4s, v0.4s
    fadd v7.4s, v7.4s, v0.4s

    dup v31.16b, w6 // Min
    dup v30.16b, w11 // Max
    fcvtas v8.4s, v4.4s
    fcvtas v9.4s, v5.4s
    fcvtas v10.4s, v6.4s
    fcvtas v11.4s, v7.4s

    sqxtn v0.4h, v8.4s
    sqxtn2 v0.8h, v9.4s
    sqxtn v1.4h, v10.4s
    sqxtn2 v1.8h, v11.4s

    sqxtn v2.8b, v0.8h
    sqxtn2 v2.16b, v1.8h

    smin v2.16b, v2.16b, v30.16b
    smax v2.16b, v2.16b, v31.16b
    st1 {v2.16b}, [x0], x4
L4LoopCheck:
    subs x5, x5, #1
    mov x1, x8
    add x2, x22, x21
    bne L4LoopDz

b End

L3Dz:
cmp w13, #1
bne L3LoopDz
sub x4, x4, #8
L3LoopDz:
    mov x8, x1
    mov x22, x2
    ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64
    ld1 {v4.16b, v5.16b, v6.16b}, [x1], x24

    smull v8.8h, v0.8b, v4.8b
    smull v9.8h, v1.8b, v4.8b
    smull v10.8h, v2.8b, v4.8b
    smull v11.8h, v3.8b, v4.8b
    smull v12.8h, v0.8b, v5.8b
    smull v13.8h, v1.8b, v5.8b
    smull v14.8h, v2.8b, v5.8b
    smull v15.8h, v3.8b, v5.8b
    
    smlal2 v8.8h, v0.16b, v4.16b
    smlal2 v9.8h, v1.16b, v4.16b
    smlal2 v10.8h, v2.16b, v4.16b
    smlal2 v11.8h, v3.16b, v4.16b
    smlal2 v12.8h, v0.16b, v5.16b
    smlal2 v13.8h, v1.16b, v5.16b
    smlal2 v14.8h, v2.16b, v5.16b
    smlal2 v15.8h, v3.16b, v5.16b

    L3Initialize:
        saddlp v16.4s, v8.8h
        saddlp v17.4s, v9.8h
        saddlp v18.4s, v10.8h
        saddlp v19.4s, v11.8h
        saddlp v20.4s, v12.8h
        saddlp v21.4s, v13.8h
        saddlp v22.4s, v14.8h
        saddlp v23.4s, v15.8h

        smull v8.8h, v0.8b, v6.8b
        smull v9.8h, v1.8b, v6.8b
        smull v10.8h, v2.8b, v6.8b
        smull v11.8h, v3.8b, v6.8b

        subs x9, x3, #1

        smlal2 v8.8h,  v0.16b, v6.16b
        smlal2 v9.8h,  v1.16b, v6.16b
        smlal2 v10.8h, v2.16b, v6.16b
        smlal2 v11.8h, v3.16b, v6.16b

        saddlp v24.4s, v8.8h
        saddlp v25.4s, v9.8h
        saddlp v26.4s, v10.8h
        saddlp v27.4s, v11.8h
    L3InitializeEnd:
        beq L3ComputeSum

    L3LoopSz:
        ld1 {v4.16b, v5.16b, v6.16b}, [x1], x24
        ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64

        smull v8.8h, v0.8b, v4.8b
        smull v9.8h, v1.8b, v4.8b
        smull v10.8h, v2.8b, v4.8b
        smull v11.8h, v3.8b, v4.8b
        smull v12.8h, v0.8b, v5.8b
        smull v13.8h, v1.8b, v5.8b
        smull v14.8h, v2.8b, v5.8b
        smull v15.8h, v3.8b, v5.8b

        smlal2 v8.8h, v0.16b, v4.16b
        smlal2 v9.8h, v1.16b, v4.16b
        smlal2 v10.8h, v2.16b, v4.16b
        smlal2 v11.8h, v3.16b, v4.16b
        smlal2 v12.8h, v0.16b, v5.16b
        smlal2 v13.8h, v1.16b, v5.16b
        smlal2 v14.8h, v2.16b, v5.16b
        smlal2 v15.8h, v3.16b, v5.16b

        sadalp v16.4s, v8.8h
        sadalp v17.4s, v9.8h
        sadalp v18.4s, v10.8h
        sadalp v19.4s, v11.8h
        sadalp v20.4s, v12.8h
        sadalp v21.4s, v13.8h
        sadalp v22.4s, v14.8h
        sadalp v23.4s, v15.8h

        smull v8.8h, v0.8b, v6.8b
        smull v9.8h, v1.8b, v6.8b
        smull v10.8h, v2.8b, v6.8b
        smull v11.8h, v3.8b, v6.8b

        subs x9, x9, #1

        smlal2 v8.8h,  v0.16b, v6.16b
        smlal2 v9.8h,  v1.16b, v6.16b
        smlal2 v10.8h, v2.16b, v6.16b
        smlal2 v11.8h, v3.16b, v6.16b

        sadalp v24.4s, v8.8h
        sadalp v25.4s, v9.8h
        sadalp v26.4s, v10.8h
        sadalp v27.4s, v11.8h

        bne L3LoopSz

    L3ComputeSum:
    addp v4.4s, v16.4s, v17.4s
    addp v5.4s, v18.4s, v19.4s
    addp v6.4s, v20.4s, v21.4s
    addp v7.4s, v22.4s, v23.4s
    addp v8.4s, v24.4s, v25.4s
    addp v9.4s, v26.4s, v27.4s

    addp v12.4s, v4.4s, v5.4s
    addp v13.4s, v6.4s, v7.4s
    addp v14.4s, v8.4s, v9.4s

    L3Quan:
    ld1 {v1.4s}, [x7], #16
    ld1 {v20.d}[0], [x14], #8 // srcKernelSum
    ld1 {v20.s}[2], [x14]
    ld1 {v21.4s}, [x12], #16 // weightQuanZero

    scvtf v4.4s, v12.4s
    scvtf v5.4s, v13.4s
    scvtf v6.4s, v14.4s
    MUL_SCALE3 v1, v4, v5, v6

    cbz x23, TILE3_MUL_OHE_SCALE
    ld1 {v2.d}[0], [x23], #8
    ld1 {v2.s}[2], [x23]
    fmul v4.4s, v4.4s, v2.s[0]
    fmul v5.4s, v5.4s, v2.s[1]
    fmul v6.4s, v6.4s, v2.s[2]
    sub x23, x23, #8

    TILE3_MUL_OHE_SCALE:
    sub x14, x14, #8
    MLA_WEIGHTZERO v4, v20, v21, 0
    MLA_WEIGHTZERO v5, v20, v21, 1
    MLA_WEIGHTZERO v6, v20, v21, 2

    cmp w13, #1
    beq L3QuantUseInt8

    L3_ADD_BIAS:
    cbz x10, L3_ADD_DSTV
    ld1 {v0.4s}, [x10], #16
    fadd v4.4s, v4.4s, v0.4s
    fadd v5.4s, v5.4s, v0.4s
    fadd v6.4s, v6.4s, v0.4s
    b L3_POST

    L3_ADD_DSTV:
    ld1 {v0.4s, v1.4s, v2.4s}, [x0]
    fadd v4.4s, v4.4s, v0.4s
    fadd v5.4s, v5.4s, v1.4s
    fadd v6.4s, v6.4s, v2.4s

    L3_POST:
    cbz x19, L3_STORE
    ld1r {v26.4s}, [x19] // f32 min
    ld1r {v27.4s}, [x20] // f32 max
    ReLU_FP32_3 v4, v5, v6, v26, v27
    L3_STORE:
    st1 {v4.4s, v5.4s, v6.4s}, [x0], x4
    b L3LoopCheck

    L3QuantUseInt8:
    ld1 {v0.4s}, [x10], #16
    fadd v4.4s, v4.4s, v0.4s
    fadd v5.4s, v5.4s, v0.4s
    fadd v6.4s, v6.4s, v0.4s
    dup v31.16b, w6 // Min
    dup v30.16b, w11 // Max
    fcvtas v8.4s, v4.4s
    fcvtas v9.4s, v5.4s
    fcvtas v10.4s, v6.4s

    sqxtn v0.4h, v8.4s
    sqxtn2 v0.8h, v9.4s
    sqxtn v1.4h, v10.4s

    sqxtn v2.8b, v0.8h
    sqxtn v3.8b, v1.8h

    smin v2.8b, v30.8b, v2.8b
    smin v3.8b, v30.8b, v3.8b

    smax v2.8b, v31.8b, v2.8b
    smax v3.8b, v31.8b, v3.8b

   
    st1 {v2.8b}, [x0], #8
    st1 {v3.s}[0], [x0], x4
L3LoopCheck:
    subs x5, x5, #1
    mov x1, x8
    add x2, x22, x21
    bne L3LoopDz

b End

L2Dz:
L2LoopDz:
    mov x8, x1
    mov x22, x2
    ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64
    ld1 {v4.16b, v5.16b}, [x1], x24
    
    
    smull v8.8h, v0.8b, v4.8b
    smull v9.8h, v1.8b, v4.8b
    smull v10.8h, v2.8b, v4.8b
    smull v11.8h, v3.8b, v4.8b
    smull v12.8h, v0.8b, v5.8b
    smull v13.8h, v1.8b, v5.8b
    smull v14.8h, v2.8b, v5.8b
    smull v15.8h, v3.8b, v5.8b

    smlal2 v8.8h, v0.16b, v4.16b
    smlal2 v9.8h, v1.16b, v4.16b
    smlal2 v10.8h, v2.16b, v4.16b
    smlal2 v11.8h, v3.16b, v4.16b
    smlal2 v12.8h, v0.16b, v5.16b
    smlal2 v13.8h, v1.16b, v5.16b
    smlal2 v14.8h, v2.16b, v5.16b
    smlal2 v15.8h, v3.16b, v5.16b

    L2Initialize:
        saddlp v16.4s, v8.8h
        saddlp v17.4s, v9.8h
        saddlp v18.4s, v10.8h
        saddlp v19.4s, v11.8h
        saddlp v20.4s, v12.8h
        saddlp v21.4s, v13.8h
        saddlp v22.4s, v14.8h
        saddlp v23.4s, v15.8h
        subs x9, x3, #1
    L2InitializeEnd:
        beq L2ComputeSum

    L2LoopSz:
        ld1 {v4.16b, v5.16b}, [x1], x24
        ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64

        smull v8.8h, v0.8b, v4.8b
        smull v9.8h, v1.8b, v4.8b
        smull v10.8h, v2.8b, v4.8b
        smull v11.8h, v3.8b, v4.8b
        smull v12.8h, v0.8b, v5.8b
        smull v13.8h, v1.8b, v5.8b
        smull v14.8h, v2.8b, v5.8b
        smull v15.8h, v3.8b, v5.8b

        smlal2 v8.8h, v0.16b, v4.16b
        smlal2 v9.8h, v1.16b, v4.16b
        smlal2 v10.8h, v2.16b, v4.16b
        smlal2 v11.8h, v3.16b, v4.16b

        subs x9, x9, #1
        smlal2 v12.8h, v0.16b, v5.16b
        smlal2 v13.8h, v1.16b, v5.16b
        smlal2 v14.8h, v2.16b, v5.16b
        smlal2 v15.8h, v3.16b, v5.16b

        sadalp v16.4s, v8.8h
        sadalp v17.4s, v9.8h
        sadalp v18.4s, v10.8h
        sadalp v19.4s, v11.8h
        sadalp v20.4s, v12.8h
        sadalp v21.4s, v13.8h
        sadalp v22.4s, v14.8h
        sadalp v23.4s, v15.8h

        bne L2LoopSz

    L2ComputeSum:

    addp v4.4s, v16.4s, v17.4s
    addp v5.4s, v18.4s, v19.4s
    addp v6.4s, v20.4s, v21.4s
    addp v7.4s, v22.4s, v23.4s

    addp v12.4s, v4.4s, v5.4s
    addp v13.4s, v6.4s, v7.4s

    L2Quan:
    ld1 {v1.4s}, [x7], #16
    ld1 {v20.d}[0], [x14] // srcKernelSum
    ld1 {v21.4s}, [x12], #16 // weightQuanZero

    scvtf v4.4s, v12.4s
    scvtf v5.4s, v13.4s
    MUL_SCALE2 v1, v4, v5

    cbz x23, TILE2_MUL_OHE_SCALE
    ld1 {v2.d}[0], [x23]
    fmul v4.4s, v4.4s, v2.s[0]
    fmul v5.4s, v5.4s, v2.s[1]

    TILE2_MUL_OHE_SCALE:
    MLA_WEIGHTZERO v4, v20, v21, 0
    MLA_WEIGHTZERO v5, v20, v21, 1

    cmp w13, #1
    beq L2QuantUseInt8

    L2_ADD_BIAS:
    cbz x10, L2_ADD_DSTV
    ld1 {v0.4s}, [x10], #16
    fadd v4.4s, v4.4s, v0.4s
    fadd v5.4s, v5.4s, v0.4s
    b L2_POST
    
    L2_ADD_DSTV:
    ld1 {v0.4s, v1.4s}, [x0]
    fadd v4.4s, v4.4s, v0.4s
    fadd v5.4s, v5.4s, v1.4s

    L2_POST:
    cbz x19, L2_STORE
    ld1r {v26.4s}, [x19] // f32 min
    ld1r {v27.4s}, [x20] // f32 max
    ReLU_FP32_2 v4, v5, v26, v27

    L2_STORE:
    st1 {v4.4s, v5.4s}, [x0], x4
    b L2LoopCheck

    L2QuantUseInt8:
    ld1 {v0.4s}, [x10], #16
    fadd v4.4s, v4.4s, v0.4s
    fadd v5.4s, v5.4s, v0.4s
    dup v31.8b, w6 // Min
    dup v30.8b, w11 // Max
    fcvtas v8.4s, v4.4s
    fcvtas v9.4s, v5.4s

    sqxtn v0.4h, v8.4s
    sqxtn2 v0.8h, v9.4s

    sqxtn v2.8b, v0.8h

    smin v8.8b, v30.8b, v8.8b
    smax v9.8b, v31.8b, v9.8b

    
    st1 {v2.8b}, [x0], x4
L2LoopCheck:
    subs x5, x5, #1
    mov x1, x8
    add x2, x22, x21
    bne L2LoopDz

b End

L1Dz:
L1LoopDz:
    mov x8, x1
    mov x22, x2
    ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64
    dup v16.4s, wzr
    dup v17.4s, wzr
    ld1 {v4.16b}, [x1], x24
    
    smull v8.8h, v0.8b, v4.8b
    dup v18.4s, wzr
    smull v9.8h, v1.8b, v4.8b
    dup v19.4s, wzr
    smull v10.8h, v2.8b, v4.8b
    smull v11.8h, v3.8b, v4.8b
    subs x9, x3, #1
    smlal2 v8.8h, v0.16b, v4.16b
    smlal2 v9.8h, v1.16b, v4.16b
    smlal2 v10.8h, v2.16b, v4.16b
    smlal2 v11.8h, v3.16b, v4.16b
    beq L1LoopSzEnd

    L1LoopSz:
        sadalp v16.4s, v8.8h
        ld1 {v4.16b}, [x1], x24
        sadalp v17.4s, v9.8h
        sadalp v18.4s, v10.8h
        sadalp v19.4s, v11.8h
        sadalp v20.4s, v12.8h
        sadalp v21.4s, v13.8h
        sadalp v22.4s, v14.8h
        sadalp v23.4s, v15.8h

        ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64

        smull v8.8h, v0.8b, v4.8b
        smull v9.8h, v1.8b, v4.8b
        smull v10.8h, v2.8b, v4.8b
        smull v11.8h, v3.8b, v4.8b

        smlal2 v8.8h, v0.16b, v4.16b
        smlal2 v9.8h, v1.16b, v4.16b
        smlal2 v10.8h, v2.16b, v4.16b
        smlal2 v11.8h, v3.16b, v4.16b

        subs x9, x9, #1
        bne L1LoopSz

    L1LoopSzEnd:
    sadalp v16.4s, v8.8h
    sadalp v17.4s, v9.8h
    sadalp v18.4s, v10.8h
    sadalp  v19.4s, v11.8h

    addp v4.4s, v16.4s, v17.4s
    addp v5.4s, v18.4s, v19.4s

    addp v12.4s, v4.4s, v5.4s

    L1Quan:
    ld1 {v1.4s}, [x7], #16
    ld1 {v20.s}[0], [x14] // srcKernelSum
    ld1 {v21.4s}, [x12], #16 // weightQuanZero

    scvtf v4.4s, v12.4s
    MUL_SCALE1 v1, v4

    cbz x23, TILE1_MUL_OHE_SCALE
    ld1 {v2.s}[0], [x23]
    fmul v4.4s, v4.4s, v2.s[0]

    TILE1_MUL_OHE_SCALE:
    MLA_WEIGHTZERO v4, v20, v21, 0

    cmp w13, #1
    beq L1QuantUseInt8

    L1_ADD_BIAS:
    cbz x10, L1_ADD_DSTV
    ld1 {v0.4s}, [x10], #16
    fadd v4.4s, v4.4s, v0.4s
    b L1_POST

    L1_ADD_DSTV:
    ld1 {v0.4s}, [x0]
    fadd v4.4s, v4.4s, v0.4s

    L1_POST:
    cbz x19, L1_STORE
    ld1r {v26.4s}, [x19] // f32 min
    ld1r {v27.4s}, [x20] // f32 max
    ReLU_FP32_1 v4, v26, v27

    L1_STORE:
    st1 {v4.4s}, [x0], x4
    b L1LoopCheck

    L1QuantUseInt8:
    ld1 {v0.4s}, [x10], #16
    fadd v4.4s, v4.4s, v0.4s
    dup v31.4s, w6 // Min
    dup v30.4s, w11 // Max

    fcvtas v8.4s, v4.4s

    smin v8.4s, v30.4s, v8.4s

    smax v8.4s, v31.4s, v8.4s

    sqxtn v0.4h, v8.4s

    sqxtn v2.8b, v0.8h
    st1 {v2.s}[0], [x0], x4
L1LoopCheck:
    subs x5, x5, #1
    mov x1, x8
    add x2, x22, x21
    bne L1LoopDz

End:
ldp x23, x24, [sp, #(16 * 6)]
ldp x21, x22, [sp, #(16 * 5)]
ldp x19, x20, [sp, #(16 * 4)]
ldp d8,  d9,  [sp, #(16 * 3)]
ldp d10, d11, [sp, #(16 * 2)]
ldp d12, d13, [sp, #(16 * 1)]
ldp d14, d15, [sp], #(16 * 8)
ret

#endif
