/*
 * x86.h - x86 ISA definitions
 *
 * This file is a part of the SimpleScalar tool suite written by
 * Todd M. Austin as a part of the Multiscalar Research Project.
 *  
 * The tool suite is currently maintained by Doug Burger and Todd M. Austin.
 * 
 * Copyright (C) 1994, 1995, 1996, 1997, 1998 by Todd M. Austin
 *
 * This source file is distributed "as is" in the hope that it will be
 * useful.  The tool set comes with no warranty, and no author or
 * distributor accepts any responsibility for the consequences of its
 * use. 
 * 
 * Everyone is granted permission to copy, modify and redistribute
 * this tool set under the following conditions:
 * 
 *    This source code is distributed for non-commercial use only. 
 *    Please contact the maintainer for restrictions applying to 
 *    commercial use.
 *
 *    Permission is granted to anyone to make or distribute copies
 *    of this source code, either as received or modified, in any
 *    medium, provided that all copyright notices, permission and
 *    nonwarranty notices are preserved, and that the distributor
 *    grants the recipient permission for further redistribution as
 *    permitted by this document.
 *
 *    Permission is granted to distribute this file in compiled
 *    or executable form under the same conditions that apply for
 *    source code, provided that either:
 *
 *    A. it is accompanied by the corresponding machine-readable
 *       source code,
 *    B. it is accompanied by a written offer, with no time limit,
 *       to give anyone a machine-readable copy of the corresponding
 *       source code in return for reimbursement of the cost of
 *       distribution.  This written offer must permit verbatim
 *       duplication by anyone, or
 *    C. it is distributed by someone who received only the
 *       executable form, and is accompanied by a copy of the
 *       written offer of source code that they received concurrently.
 *
 * In other words, you are welcome to use, share and improve this
 * source file.  You are forbidden to forbid anyone else to use, share
 * and improve what you give them.
 *
 * INTERNET: dburger@cs.wisc.edu
 * US Mail:  1210 W. Dayton Street, Madison, WI 53706
 *
 */

#ifndef X86_H
#define X86_H

#include <cstdint>

extern "C" {

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <setjmp.h>

#include "host.h"
#include "misc.h"


/*
 * This file contains various definitions needed to decode, disassemble, and
 * execute x86 instructions.
 */

/* build for x86 target */
#define MAX_CORES 16
#define INVALID_CORE -1


/*
 * target-dependent type definitions
 */

/* address type definition (32-bit) */
typedef dword_t md_addr_t;

/* physical address type definition (64-bit) */
typedef qword_t md_paddr_t;


/* instruction flags */
#define F_ICOMP       0x00000001    /* integer computation */
#define F_FCOMP       0x00000002    /* FP computation */
#define F_CTRL        0x00000004    /* control inst */
#define F_UNCOND      0x00000008    /*   unconditional change */
#define F_COND        0x00000010    /*   conditional change */
#define F_MEM         0x00000020    /* memory access inst */
#define F_LOAD        0x00000040    /*   load inst */
#define F_STORE       0x00000080    /*   store inst */
#define F_TRAP        0x00000800    /* traping inst */
#define F_DIRJMP      0x00002000    /* direct jump */
#define F_INDIRJMP    0x00004000    /* indirect jump */
#define F_CALL        0x00008000    /* function call */
#define F_FPCOND      0x00010000    /* FP conditional branch */
#define F_IMM         0x00020000    /* instruction has immediate operand */
#define F_AGEN        0x00080000    /* AGEN micro-instruction */
#define F_IMMB        0x00100000    /* inst has 1-byte immediate */
#define F_IMMW        0x00200000    /* inst has 2-byte immediate */
#define F_IMMD        0x00300000    /* inst has 4-byte immediate */
#define F_IMMV        0x00400000    /* inst has 1-byte immediate */
#define F_IMMA        0x00500000    /* inst has 1-byte immediate */
#define F_UIMM        0x01000000    /* immediate operand is unsigned */
#define F_RETN        0x40000000    /* subroutine return */


#define FSW_TOP(X)        (((X) >> 11) & 0x07)
#define SET_FSW_TOP(X,N)    ((X) = (((X) & ~(7<<11)) | (((N) & 7)<<11)))


/* helper macros */

/*
 * various other helper macros/functions
 */

/* XXX returns non-zero if instruction is a function call */
#define MD_IS_CALL(OPFLAGS)          ((OPFLAGS)&F_CALL)

/* returns non-zero if instruction is a function return */
#define MD_IS_RETURN(OPFLAGS)        ((OPFLAGS)&F_RETN)

/* returns non-zero if instruction is an indirect jump */
#define MD_IS_INDIR(OPFLAGS)         ((OPFLAGS)&F_INDIRJMP)

/* globbing/fusion masks */
#define FUSION_NONE 0x0000LL
#define FUSION_LOAD_OP 0x0001LL
#define FUSION_STA_STD 0x0002LL
#define FUSION_PARTIAL 0x0004LL  /* for partial-register-write merging uops */
/* to add: OP_OP, OP_ST */
#define FUSION_LD_OP_ST 0x0008LL /* for atomic Mop execution */
#define FUSION_FP_LOAD_OP 0x0010LL /* same as load op, but for fp ops */

#define FUSION_TYPE(uop) (((uop_inst_t)((uop)->decode.raw_op)) >> 32)

/*
 * UOP stuff
 */

/* microcode field specifiers (xfields) */
enum md_xfield_t {
  /* integer register constants */
  XR_AL, XR_AH, XR_AX, XR_EAX, XR_eAX,
  XR_CL, XR_CH, XR_CX, XR_ECX, XR_eCX,
  XR_DL, XR_DH, XR_DX, XR_EDX, XR_eDX,
  XR_BL, XR_BH, XR_BX, XR_EBX, XR_eBX,
  XR_SP, XR_ESP, XR_eSP,
  XR_BP, XR_EBP, XR_eBP,
  XR_SI, XR_ESI, XR_eSI,
  XR_DI, XR_EDI, XR_eDI,

  /* specify NO segment register - needed for a few weird uop flows */
  XR_SEGNONE,

  /* integer temporary registers */
  XR_TMP0, XR_TMP1, XR_TMP2, XR_ZERO,

  /* FP register constants */
  XR_ST0, XR_ST1, XR_ST2, XR_ST3, XR_ST4, XR_ST5, XR_ST6, XR_ST7,

  /* FP temporary registers */
  XR_FTMP0, XR_FTMP1, XR_FTMP2,

  /* XMM registers */
  XR_XMM0, XR_XMM1, XR_XMM2, XR_XMM3, XR_XMM4, XR_XMM5, XR_XMM6, XR_XMM7,
  /* XMM temp registers */
  XR_XMMTMP0, XR_XMMTMP1, XR_XMMTMP2,

  /* x86 bitfields */
  XF_RO, XF_R, XF_RM, XF_BASE, XF_INDEX, XF_SCALE, XF_STI,
  XF_DISP, XF_IMMB, XF_IMMV, XF_IMMA, XE_SIZEV_IMMW, XF_CC,
  XF_SYSCALL, XF_SEG,

  /* literal expressions */
  XE_ZERO, XE_ONE, XE_MONE, XE_SIZEV, XE_MSIZEV, XE_ILEN,
  XE_CCE, XE_CCNE,
  XE_FCCB, XE_FCCNB, XE_FCCE, XE_FCCNE, XE_FCCBE, XE_FCCNBE, XE_FCCU, XE_FCCNU,
  XE_CF, XE_DF, XE_SFZFAFPFCF,
  XE_FP1, XE_FNOP, XE_FPUSH, XE_FPOP, XE_FPOPPOP,
};

/* UOP visibility switches */
#define UV        1ULL    /* visible */
#define UP        0ULL    /* private */
#define ISUV        ((uop[0].decode.raw_op >> 30) & 1)


/* UOP fields */
#define UHASIMM        ((uop[0].decode.raw_op >> 31) & 1)
#define USG             (UHASIMM ? (uop[2].decode.raw_op & 0xf): 0)
#define URD        ((uop[0].decode.raw_op >> 12) & 0xf)
#define UCC        URD
#define URS        ((uop[0].decode.raw_op >> 8) & 0xf)
#define URT        ((uop[0].decode.raw_op >> 4) & 0xf)
#define URU        (uop[0].decode.raw_op & 0xf)
#define ULIT        (uop[0].decode.raw_op & 0xf)
#define UIMMB                                \
  (UHASIMM                                \
   ? ((sdword_t)(sbyte_t)(uop[1].decode.raw_op & 0xff))                \
   : ((sdword_t)(sbyte_t)(uop[0].decode.raw_op & 0xff)))
#define UIMMUB                                \
  (UHASIMM                                \
   ? ((dword_t)(byte_t)(uop[1].decode.raw_op & 0xff))                    \
   : ((dword_t)(byte_t)(uop[0].decode.raw_op & 0xff)))
#define UIMMW                                \
  (UHASIMM                                \
   ? ((sdword_t)(sword_t)(uop[1].decode.raw_op & 0xffff))                \
   : ((sdword_t)(sword_t)(word_t)(uop[0].decode.raw_op & 0xff)))
#define UIMMUW                                \
  (UHASIMM                                \
   ? ((dword_t)(word_t)(uop[1].decode.raw_op & 0xffff))                \
   : ((dword_t)(word_t)(uop[0].decode.raw_op & 0xff)))
#define UIMMD                                \
  (UHASIMM                                \
   ? ((sdword_t)uop[1].decode.raw_op)                            \
   : ((sdword_t)(dword_t)(uop[0].decode.raw_op & 0xff)))
#define UIMMUD                                \
  (UHASIMM                                \
   ? ((dword_t)uop[1].decode.raw_op)                            \
   : ((dword_t)(uop[0].decode.raw_op & 0xff)))

/* mode-specific constants */
#define SIZE_W            2
#define SIZE_D            4
#define BITSIZE_W        16
#define BITSIZE_D        32
#define LOGSIZE_W        1
#define LOGSIZE_D        2
#define LOGBITSIZE_W        4
#define LOGBITSIZE_D        5

/* XXX X86 UOP definition */
#define MD_MAX_FLOWLEN        62 /* leave 2 for REP ctrl uops */
#define UOP_SEQ_SHIFT         6
typedef qword_t uop_inst_t;


namespace xiosim {
namespace x86 {
const size_t MAX_ILEN = 15;
}
}

//XXX: This should go away once I'm done with the cleanup
typedef struct {
    size_t len;
    bool rep;
    uint8_t code[xiosim::x86::MAX_ILEN];
} md_inst_t;

}

#endif /* X86_H */



