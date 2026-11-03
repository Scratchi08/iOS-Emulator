/*
 * jazelle.c — see jazelle.h for the (deliberately narrow) scope.
 */

#include "arm1176.h"
#include "arm1176_internal.h"
#include "jazelle.h"

uint32_t jazelle_mrc(arm1176_t *cpu, uint32_t crn, uint32_t opc1, uint32_t crm, uint32_t opc2)
{
    (void)cpu; (void)crn; (void)opc1; (void)crm; (void)opc2;
    return 0; /* JIDR/JMCR/JOSCR (and anything else in CP14): RAZ */
}

void jazelle_mcr(arm1176_t *cpu, uint32_t crn, uint32_t opc1, uint32_t crm, uint32_t opc2, uint32_t value)
{
    (void)cpu; (void)crn; (void)opc1; (void)crm; (void)opc2; (void)value;
    /* WI: writes to any CP14 Jazelle register are silently ignored. */
}

int jazelle_bxj(arm1176_t *cpu, uint32_t rm_value)
{
    /* Entry to real Jazelle state never succeeds (JIDR reports no
     * hardware acceleration), so this is the documented "otherwise"
     * path: branch to Rm, interworking into Thumb if bit0 is set —
     * i.e. plain BX semantics. */
    write_pc_bx(cpu, rm_value);
    return 3;
}
