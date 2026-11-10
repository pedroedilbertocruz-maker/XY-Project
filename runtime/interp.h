/* Interpretador ARM/Thumb/VFP (interp.c) */
#pragma once
#include "rt.h"
typedef enum { IP_RET = 0, IP_SVC, IP_UNDEF, IP_STEPS } IpExit;
IpExit ip_run(Ctx *c, uint32_t start, uint32_t ret, long max_steps, int do_svc, long *steps_out);
void ip_overlay_begin(void);
void ip_overlay_end(void);
uint32_t ip_overlay_count(void);
uint32_t ip_overlay_addr(uint32_t i);
uint8_t ip_overlay_val(uint32_t a);
uint32_t ip_overlay_pc(uint32_t a);
extern int ip_overflow;
extern uint32_t ip_last_info;
extern void (*ip_tick_hook)(void);
