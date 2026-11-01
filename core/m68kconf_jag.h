/* Musashi configuration for the Jaguar's 68000 (selected via MUSASHI_CNF). */
#ifndef M68KCONF_JAG_H
#define M68KCONF_JAG_H

#define M68K_OPT_OFF             0
#define M68K_OPT_ON              1
#define M68K_OPT_SPECIFY_HANDLER 2

#define M68K_COMPILE_FOR_MAME      M68K_OPT_OFF

/* Only a plain 68000 is needed. Turning the others off shrinks the dispatch. */
#define M68K_EMULATE_010            M68K_OPT_OFF
#define M68K_EMULATE_EC020          M68K_OPT_OFF
#define M68K_EMULATE_020            M68K_OPT_OFF
#define M68K_EMULATE_030            M68K_OPT_OFF
#define M68K_EMULATE_040            M68K_OPT_OFF

#define M68K_SEPARATE_READS         M68K_OPT_OFF
#define M68K_SIMULATE_PD_WRITES     M68K_OPT_OFF

/* TOM supplies vector 64 ($100) for its interrupt, so we need the ack hook. */
#define M68K_EMULATE_INT_ACK        M68K_OPT_SPECIFY_HANDLER
#define M68K_INT_ACK_CALLBACK(A)    jag_m68k_int_ack(A)

#define M68K_EMULATE_BKPT_ACK       M68K_OPT_OFF
#define M68K_BKPT_ACK_CALLBACK()    0
#define M68K_EMULATE_TRACE          M68K_OPT_OFF
#define M68K_EMULATE_RESET          M68K_OPT_OFF
#define M68K_RESET_CALLBACK()       0
#define M68K_CMPILD_HAS_CALLBACK    M68K_OPT_OFF
#define M68K_CMPILD_CALLBACK(v,r)   0
#define M68K_RTE_HAS_CALLBACK       M68K_OPT_OFF
#define M68K_RTE_CALLBACK()         0
#define M68K_TAS_HAS_CALLBACK       M68K_OPT_OFF
#define M68K_TAS_CALLBACK()         0
#define M68K_ILLG_HAS_CALLBACK      M68K_OPT_OFF
#define M68K_ILLG_CALLBACK(opcode)  0
#define M68K_TRAP_HAS_CALLBACK      M68K_OPT_OFF
#define M68K_TRAP_CALLBACK(trap)    0
#define M68K_EMULATE_FC             M68K_OPT_OFF
#define M68K_SET_FC_CALLBACK(A)     0
#define M68K_MONITOR_PC             M68K_OPT_OFF
#define M68K_SET_PC_CALLBACK(A)     0
#ifdef JAG_TRACE
#define M68K_INSTRUCTION_HOOK       M68K_OPT_SPECIFY_HANDLER
#define M68K_INSTRUCTION_CALLBACK(pc) jag_trace_pc(pc)
#else
#define M68K_INSTRUCTION_HOOK       M68K_OPT_OFF
#define M68K_INSTRUCTION_CALLBACK(pc) 0
#endif
#define M68K_EMULATE_PREFETCH       M68K_OPT_OFF
#define M68K_EMULATE_ADDRESS_ERROR  M68K_OPT_OFF
#define M68K_LOG_ENABLE             M68K_OPT_OFF
#define M68K_LOG_1010_1111          M68K_OPT_OFF
#define M68K_LOG_TRAP               M68K_OPT_OFF
#define M68K_LOG_FILEHANDLE         stderr
#define M68K_EMULATE_PMMU           M68K_OPT_OFF
#define M68K_USE_64_BIT             M68K_OPT_ON

#ifdef __cplusplus
extern "C" {
#endif
int jag_m68k_int_ack(int level);
void jag_trace_pc(unsigned int pc);
#ifdef __cplusplus
}
#endif

#endif
