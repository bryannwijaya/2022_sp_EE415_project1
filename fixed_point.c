#include "threads/fixed_point.h"
#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"

/* Conversion factor use to convert to and from 17.14 
   float-point format. 
   This is 0000 0000 0000 0000 0100 0000 0000 0000 in binary. */
#define F (1<<14)

/* In this section, let n be integer, x and y fixed-point numbers, 
   and F be 1 in 17.14 format. */

/* Converts integer to fixed point. */
fp_t 
cvt_int_to_fp (int n)
{
  return n * F;
} 

/* Converts fixed-point to integer, but truncated
   (i.e., rounding toward zero). */
int
cvt_fp_to_int_truncate (fp_t x)
{
  return x / F;
}

/* Converts fixed-point to integer, rounded to nearest. */
int
cvt_fp_to_int_round (fp_t x)
{
  if (x >= 0)
    return (x + F / 2) / F;
  else
    return (x - F / 2) / F;
}

/* Adds up two fixed-points. */
fp_t
add_two_fp (fp_t x, fp_t y)
{
  return x + y;
}

/* Subtracts fixed-point from fixed-point. */
fp_t 
sub_fp_from_fp (fp_t x, fp_t y)
{
  return x - y;
}

/* Adds up a fixed-point and an integer. */
fp_t
add_fp_and_int (fp_t x, int n)
{
  return x + n * F;
}

/* Subtracts integer from a fixed-point. */
fp_t
sub_int_from_fp (fp_t x, int n)
{
  return x - n * F;
}

/* Product of two fixed-points. */
fp_t
product_two_fp (fp_t x, fp_t y)
{
  return ((int64_t) x) * y / F;
}

/* Product of a fixed-point and an integer. */
fp_t
product_fp_and_int (fp_t x, int n)
{
  return x * n;
}

/* Dividing a fixed-point by a fixed-point. */
fp_t
div_fp_by_fp (fp_t x, fp_t y)
{
  return ((int64_t) x) * F / y;
}

/* Dividing a fixed-point by an integer. */
fp_t
div_fp_by_int (fp_t x, int n)
{
  return x / n;
}