#ifndef PS3_SPU_NATIVE_EXTRA_FP_H
#define PS3_SPU_NATIVE_EXTRA_FP_H
/* Startup-only mode shared by the audited lift and its execution owner.
 * 0: original arithmetic/environment; 1: native RTZ; 2: checked native RTZ.
 * Each symbol is linked only when its corresponding adapter is configured. */
extern int spu_native_extra_fp_mode;
extern int spu_native_image39_fp_mode;
#endif
