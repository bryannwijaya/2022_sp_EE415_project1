#include <debug.h>
#include <list.h>
#include <stdint.h>

/* Float-point type.
   This is to distinguish float-point from actual integers. */
typedef int fp_t;

/* Fixed-point arithmetic operations */
fp_t cvt_int_to_fp (int n);
int cvt_fp_to_int_truncate (fp_t x);
int cvt_fp_to_int_round (fp_t x);
fp_t add_two_fp (fp_t x, fp_t y);
fp_t  sub_fp_from_fp (fp_t x, fp_t y);
fp_t add_fp_and_int (fp_t x, int n);
fp_t sub_int_from_fp (fp_t x, int n);
fp_t product_two_fp (fp_t x, fp_t y);
fp_t product_fp_and_int (fp_t x, int n);
fp_t div_fp_by_fp (fp_t x, fp_t y);
fp_t div_fp_by_int (fp_t x, int n);