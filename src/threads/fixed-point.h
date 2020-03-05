#include <stdint.h>

#define f (1 << 14)
#define FRAC_MASK (~(0u) >> (8 * sizeof(int) - 14))

typedef int fixedpt_t;

inline int 
round_to_int (fixedpt_t x)
{
  if (x > 0)
    return (x + f / 2) / f;
  else
    return (x - f / 2) / f;
}

inline int 
trunc_to_int (fixedpt_t x)
{
  return x / f;
}

inline int 
ceil_to_int (fixedpt_t x)
{
  if ((x & FRAC_MASK) == 0)
    return x / f;
  else
    return x / f + 1;
}

inline int 
floor_to_int (fixedpt_t x)
{
  return (x & ~FRAC_MASK) / f;
}

inline fixedpt_t 
to_fixed (int n)
{
  return n * f;
}

inline fixedpt_t 
mult_fixed (fixedpt_t x, fixedpt_t y)
{
  return ((int64_t) x) * y / f;
}

inline fixedpt_t 
div_fixed (fixedpt_t x, fixedpt_t y)
{
  return ((int64_t) x) * f / y;
}

inline fixedpt_t 
add_fixed (fixedpt_t x, fixedpt_t y)
{
  return x + y;
}

inline fixedpt_t 
mult_int (fixedpt_t x, int n)
{
  return x * n;
}

inline fixedpt_t 
div_int (fixedpt_t x, int n)
{
  return x / n;
}

inline fixedpt_t 
add_int (fixedpt_t x, int n)
{
  return x + n * f;
}

#undef f
#undef FRAC_MASK