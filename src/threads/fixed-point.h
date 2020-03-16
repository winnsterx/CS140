#include <stdint.h>
typedef int fixedpt_t;

int round_to_int (fixedpt_t x);
int trunc_to_int (fixedpt_t x);
int ceil_to_int (fixedpt_t x);
int floor_to_int (fixedpt_t x);
fixedpt_t to_fixed (int n);
fixedpt_t mult_fixed (fixedpt_t x, fixedpt_t y);
fixedpt_t div_fixed (fixedpt_t x, fixedpt_t y);
fixedpt_t add_fixed (fixedpt_t x, fixedpt_t y);
fixedpt_t mult_int (fixedpt_t x, int n);
fixedpt_t div_int (fixedpt_t x, int n);
fixedpt_t add_int (fixedpt_t x, int n);
