/* smp.h - i386 SMP controller for Mach
   Copyright (C) 2020 Free Software Foundation, Inc.
   Written by Almudena Garcia Jurado-Centurion

   This file is part of GNU Mach.

   GNU Mach is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   GNU Mach is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include <string.h>
#include <i386/apic.h>
#include <i386/smp.h>
#include <i386/cpu.h>
#include <i386/pio.h>
#include <i386/pit.h>
#include <i386/vm_param.h>
#include <i386/proc_reg.h>
#include <i386at/idt.h>
#include <i386at/cram.h>
#include <i386at/acpi_parse_apic.h>
#include <kern/debug.h>
#include <kern/printf.h>
#include <mach/machine.h>

#include <kern/smp.h>

/* Set to TRUE if the hypervisor advertises KVM_FEATURE_PV_UNHALT,
 * meaning paravirtualized spinlocks are supported.  Detected in smp_init(). */
boolean_t pv_spinlock_enabled = FALSE;

/*
 * smp_data_init: initialize smp_data structure
 * Must be called after smp_init(), once all APIC structures
 * has been initialized
 */
static void smp_data_init(void)
{
    uint8_t numcpus = apic_get_numcpus();
    smp_set_numcpus(numcpus);

    for(int i = 0; i < numcpus; i++){
            machine_slot[i].is_cpu = TRUE;
    }

}

static boolean_t
wait_for_ipi_completion(void)
{
    unsigned int spins = SMP_IPI_DELIVERY_SPINS;

    while (lapic->icr_low.delivery_status == SEND_PENDING) {
        if (spins == 0)
            return FALSE;
        spins--;
        cpu_pause();
    }
    return TRUE;
}

static boolean_t smp_send_ipi(unsigned logical_id, unsigned vector)
{
    unsigned long flags;
    int err;
    boolean_t success = TRUE;

    cpu_intr_save(&flags);

    if (!wait_for_ipi_completion()) {
        printf("Timed out waiting for a previous IPI delivery\n");
        success = FALSE;
        goto out;
    }

    lapic->error_status.r = 0;
    (void) lapic->error_status.r;

    apic_send_ipi(NO_SHORTHAND, FIXED, LOGICAL, ASSERT, EDGE, vector, logical_id);

    if (!wait_for_ipi_completion()) {
        printf("Timed out waiting for IPI 0x%x delivery\n", vector);
        success = FALSE;
        goto out;
    }

    /* A completed ICR write does not prove that the target accepted the IPI. */
    err = lapic->error_status.r;
    if (err) {
        printf("ESR error delivering IPI 0x%x: 0x%x\n", vector, err);
        success = FALSE;
    }

out:
    cpu_intr_restore(flags);
    return success;
}

void smp_remote_ast(unsigned logical_id)
{
    if (!smp_send_ipi(logical_id, CALL_AST_CHECK))
        panic("Failed to deliver remote AST IPI");
}

void smp_pmap_update(unsigned logical_id)
{
    if (!smp_send_ipi(logical_id, CALL_PMAP_UPDATE))
        panic("Failed to deliver pmap update IPI");
}

void smp_io_perm_update(unsigned logical_id)
{
    if (!smp_send_ipi(logical_id, CALL_IO_PERM_UPDATE))
        panic("Failed to deliver I/O permission update IPI");
}

static boolean_t
wait_for_ipi(void)
{
    int timeout = SMP_STARTUP_TIMEOUT_US;

    /* A missing or wedged target must not hold the boot processor forever. */
    while (lapic->icr_low.delivery_status == SEND_PENDING) {
        if (timeout == 0) {
            printf("IPI delivery timeout\n");
            return FALSE;
        }
        pit_udelay(SMP_STARTUP_POLL_US);
        timeout -= SMP_STARTUP_POLL_US;
    }
    return TRUE;
}

static int
smp_send_ipi_init(int apic_id)
{
    int err;

    lapic->error_status.r = 0;
    err = lapic->error_status.r;

    /* Assert INIT IPI:
     *
     * This is EDGE triggered to match the deassert, and is targeted at one
     * AP by its APIC ID rather than broadcast.
     */
    apic_send_ipi(NO_SHORTHAND, INIT, PHYSICAL, ASSERT, EDGE, 0, apic_id);

    /* Wait for delivery */
    if (!wait_for_ipi()) {
        printf("Timeout waiting for INIT assertion\n");
        return -1;
    }

    /* Deassert INIT IPI:
     *
     * NB: This must be an EDGE triggered deassert signal.
     * A LEVEL triggered deassert is only supported on very old hardware
     * that does not support STARTUP IPIs at all, and instead jump
     * via a warm reset vector.
     */
    apic_send_ipi(NO_SHORTHAND, INIT, PHYSICAL, DE_ASSERT, EDGE, 0, apic_id);

    /* Wait for delivery */
    if (!wait_for_ipi()) {
        printf("Timeout waiting for INIT deassertion\n");
        return -1;
    }

    err = lapic->error_status.r;
    if (err) {
        printf("ESR error upon INIT 0x%x\n", err);
        return -1;
    }
    return 0;
}

static int
smp_send_ipi_startup_twice(int apic_id, int vector)
{
    int i, accept_err, send_err;
    volatile int err;

    for (i = 0; i < 2; i++) {
        lapic->error_status.r = 0;
        err = lapic->error_status.r;
        /* Suppress unused variable warning for the necessary dummy read.*/
        (void) err;

        /* StartUp IPI:
         *
         * Have not seen any documentation for trigger mode for this IPI
         * but it seems to work with EDGE.  (AMD BKDG FAM16h document specifies dont care)
         */
        apic_send_ipi(NO_SHORTHAND, STARTUP, PHYSICAL, ASSERT, EDGE, vector, apic_id);

        pit_udelay(10);

        /* Wait for other cpu to accept IPI */
        if (!wait_for_ipi()) {
            printf("Timeout waiting for STARTUP IPI delivery\n");
            return -1;
        }
        send_err = lapic->error_status.r;

        pit_udelay(10);

        lapic->error_status.r = 0;
        accept_err = lapic->error_status.r & 0xef;

        if (send_err || accept_err)
            break;
    }

    if (send_err)
        printf("ESR error: DID NOT SEND? 0x%x\n", send_err);
    if (accept_err)
        printf("ESR error: delivery 0x%x\n", accept_err);

    return send_err | accept_err;
}

/* See Intel IA32/64 Software Developer's Manual 3A Section 8.4.4.1 */
int smp_startup_cpus(unsigned bsp_apic_id, phys_addr_t start_eip)
{
    int err;
    uint8_t ncpus;
    int cpu;
#if 0
    /* This block goes with a legacy method of INIT that only works with
     * old hardware that does not support SIPIs.
     * Must use INIT DEASSERT LEVEL triggered IPI to use this block.
     * (At least one AMD FCH does not support this IPI mode,
     * See AMD BKDG FAM16h document # 48751 page 461).
     */

    /* Tell CMOS to warm reset through through 40:67 */
    outb(CMOS_ADDR, CMOS_SHUTDOWN);
    outb(CMOS_DATA, CM_JMP_467);

    /* Set warm reset vector to point to AP startup code */
    uint16_t dword[2];
    dword[0] = 0;
    dword[1] = start_eip >> 4;
    memcpy((uint8_t *)phystokv(0x467), dword, 4);
#endif

    /* Local cache flush */
    asm("wbinvd":::"memory");

    printf("Sending IPIs from BSP APIC ID %u...\n", bsp_apic_id);

    /* Target each AP from the MADT individually.  The previous broadcast
     * "all excluding self" shorthand is rejected by some hypervisors
     * (bhyve) and is not the documented per-AP INIT/SIPI sequence. */
    ncpus = apic_get_numcpus();
    for (cpu = 0; cpu < ncpus; cpu++) {
        int apic_id = apic_get_cpu_apic_id(cpu);
        if (apic_id < 0 || apic_id == (int)bsp_apic_id)
            continue;
        err = smp_send_ipi_init(apic_id);
        if (!err)
            err = smp_send_ipi_startup_twice(apic_id, start_eip >> STARTUP_VECTOR_SHIFT);
        if (err) {
            printf("FATAL: AP %d failed to start\n", cpu);
            return -1;
        }
    }
    printf("done\n");
    return 0;
}

/*
 * smp_init: initialize the SMP support, starting the cpus searching
 * and enumeration.
 */
int smp_init(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Detect KVM paravirtualized spinlock support:
     * CPUID leaf 0x40000001, EBX bit 7 (KVM_FEATURE_PV_UNHALT).
     * Must be called before any AP starts so pv_spinlock_enabled
     * is set before lock contention between CPUs can occur. */
    eax = 0x40000001;
    ecx = 0;
    cpuid(eax, ebx, ecx, edx);
    pv_spinlock_enabled = (ebx & KVM_FEATURE_PV_UNHALT) ? TRUE : FALSE;

    smp_data_init();

    return 0;
}
