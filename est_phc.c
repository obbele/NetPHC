/*	$NetBSD: est.c,v 1.9 2008/04/28 20:23:40 martin Exp $	*/
/*
 * Copyright (c) 2003 Michael Eriksson.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*-
 * Copyright (c) 2004 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This is a driver for Intel's Enhanced SpeedStep Technology (EST),
 * as implemented in Pentium M processors.
 *
 * Reference documentation:
 *
 * - IA-32 Intel Architecture Software Developer's Manual, Volume 3:
 *   System Programming Guide.
 *   Section 13.14, Enhanced Intel SpeedStep technology.
 *   Table B-2, MSRs in Pentium M Processors.
 *   http://www.intel.com/design/pentium4/manuals/253668.htm
 *
 * - Intel Pentium M Processor Datasheet.
 *   Table 5, Voltage and Current Specifications.
 *   http://www.intel.com/design/mobile/datashts/252612.htm
 *
 * - Intel Pentium M Processor on 90 nm Process with 2-MB L2 Cache Datasheet
 *   Table 3-4, 3-5, 3-6, Voltage and Current Specifications.
 *   http://www.intel.com/design/mobile/datashts/302189.htm
 *
 * - Linux cpufreq patches, speedstep-centrino.c.
 *   Encoding of MSR_PERF_CTL and MSR_PERF_STATUS.
 *   http://www.codemonkey.org.uk/projects/cpufreq/cpufreq-2.4.22-pre6-1.gz
 *
 *   ACPI objects: _PCT is MSR location, _PSS is freq/voltage, _PPC is caps.
 */

/* #define EST_DEBUG */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: est.c,v 1.9 2008/04/28 20:23:40 martin Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kmem.h>
#include <sys/sysctl.h>
#include <sys/once.h>

#include <x86/cpuvar.h>
#include <x86/cputypes.h>
#include <x86/cpu_msr.h>

#include <machine/cpu.h>
#include <machine/specialreg.h>

#include "opt_est.h"
#ifdef EST_FREQ_USERWRITE
#define	EST_TARGET_CTLFLAG	(CTLFLAG_READWRITE | CTLFLAG_ANYWRITE)
#else
#define	EST_TARGET_CTLFLAG	CTLFLAG_READWRITE
#endif

/* Convert MHz and mV into IDs for passing to the MSR. */
#define ID16(MHz, mV, bus_clk) \
	((((MHz * 100 + 50) / bus_clk) << 8) | ((mV ? mV - 700 : 0) >> 4))

/* Possible bus speeds (multiplied by 100 for rounding) */
enum { BUS100 = 10000, BUS133 = 13333, BUS166 = 16666, BUS200 = 20000 };

/* Ultra Low Voltage Intel Pentium M processor 900 MHz */
static const uint16_t pm130_900_ulv[] = {
	ID16( 900, 1004, BUS100),
	ID16( 800,  988, BUS100),
	ID16( 600,  844, BUS100),
};

/* Ultra Low Voltage Intel Pentium M processor 1.00 GHz */
static const uint16_t pm130_1000_ulv[] = {
	ID16(1000, 1004, BUS100),
	ID16( 900,  988, BUS100),
	ID16( 800,  972, BUS100),
	ID16( 600,  844, BUS100),
};

/* Ultra Low Voltage Intel Pentium M processor 1.10 GHz */
static const uint16_t pm130_1100_ulv[] = {
	ID16(1100, 1004, BUS100),
	ID16(1000,  988, BUS100),
	ID16( 900,  972, BUS100),
	ID16( 800,  956, BUS100),
	ID16( 600,  844, BUS100),
};

/* Low Voltage Intel Pentium M processor 1.10 GHz */
static const uint16_t pm130_1100_lv[] = {
	ID16(1100, 1180, BUS100),
	ID16(1000, 1164, BUS100),
	ID16( 900, 1100, BUS100),
	ID16( 800, 1020, BUS100),
	ID16( 600,  956, BUS100),
};

/* Low Voltage Intel Pentium M processor 1.20 GHz */
static const uint16_t pm130_1200_lv[] = {
	ID16(1200, 1180, BUS100),
	ID16(1100, 1164, BUS100),
	ID16(1000, 1100, BUS100),
	ID16( 900, 1020, BUS100),
	ID16( 800, 1004, BUS100),
	ID16( 600,  956, BUS100),
};

/* Low Voltage Intel Pentium M processor 1.30 GHz */
static const uint16_t pm130_1300_lv[] = {
	ID16(1300, 1180, BUS100),
	ID16(1200, 1164, BUS100),
	ID16(1100, 1100, BUS100),
	ID16(1000, 1020, BUS100),
	ID16( 900, 1004, BUS100),
	ID16( 800,  988, BUS100),
	ID16( 600,  956, BUS100),
};

/* Intel Pentium M processor 1.30 GHz */
static const uint16_t pm130_1300[] = {
	ID16(1300, 1388, BUS100),
	ID16(1200, 1356, BUS100),
	ID16(1000, 1292, BUS100),
	ID16( 800, 1260, BUS100),
	ID16( 600,  956, BUS100),
};

/* Intel Pentium M processor 1.40 GHz */
static const uint16_t pm130_1400[] = {
	ID16(1400, 1484, BUS100),
	ID16(1200, 1436, BUS100),
	ID16(1000, 1308, BUS100),
	ID16( 800, 1180, BUS100),
	ID16( 600,  956, BUS100),
};

/* Intel Pentium M processor 1.50 GHz */
static const uint16_t pm130_1500[] = {
	ID16(1500, 1484, BUS100),
	ID16(1400, 1452, BUS100),
	ID16(1200, 1356, BUS100),
	ID16(1000, 1228, BUS100),
	ID16( 800, 1116, BUS100),
	ID16( 600,  956, BUS100),
};

/* Intel Pentium M processor 1.60 GHz */
static const uint16_t pm130_1600[] = {
	ID16(1600, 1484, BUS100),
	ID16(1400, 1420, BUS100),
	ID16(1200, 1276, BUS100),
	ID16(1000, 1164, BUS100),
	ID16( 800, 1036, BUS100),
	ID16( 600,  956, BUS100),
};

/* Intel Pentium M processor 1.70 GHz */
static const uint16_t pm130_1700[] = {
	ID16(1700, 1484, BUS100),
	ID16(1400, 1308, BUS100),
	ID16(1200, 1228, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1004, BUS100),
	ID16( 600,  956, BUS100),
};

/* Intel Pentium M processor 723 1.0 GHz */
static const uint16_t pm90_n723[] = {
	ID16(1000,  940, BUS100),
	ID16( 900,  908, BUS100),
	ID16( 800,  876, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 733 1.1 GHz, VID #G */
static const uint16_t pm90_n733g[] = {
	ID16(1100,  956, BUS100),
	ID16(1000,  940, BUS100),
	ID16( 900,  908, BUS100),
	ID16( 800,  876, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 733 1.1 GHz, VID #H */
static const uint16_t pm90_n733h[] = {
	ID16(1100,  940, BUS100),
	ID16(1000,  924, BUS100),
	ID16( 900,  892, BUS100),
	ID16( 800,  876, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 733 1.1 GHz, VID #I */
static const uint16_t pm90_n733i[] = {
	ID16(1100,  924, BUS100),
	ID16(1000,  908, BUS100),
	ID16( 900,  892, BUS100),
	ID16( 800,  860, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 733 1.1 GHz, VID #J */
static const uint16_t pm90_n733j[] = {
	ID16(1100,  908, BUS100),
	ID16(1000,  892, BUS100),
	ID16( 900,  876, BUS100),
	ID16( 800,  860, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 733 1.1 GHz, VID #K */
static const uint16_t pm90_n733k[] = {
	ID16(1100,  892, BUS100),
	ID16(1000,  876, BUS100),
	ID16( 900,  860, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 733 1.1 GHz, VID #L */
static const uint16_t pm90_n733l[] = {
	ID16(1100,  876, BUS100),
	ID16(1000,  876, BUS100),
	ID16( 900,  860, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 753 1.2 GHz, VID #G */
static const uint16_t pm90_n753g[] = {
	ID16(1200,  956, BUS100),
	ID16(1100,  940, BUS100),
	ID16(1000,  908, BUS100),
	ID16( 900,  892, BUS100),
	ID16( 800,  860, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 753 1.2 GHz, VID #H */
static const uint16_t pm90_n753h[] = {
	ID16(1200,  940, BUS100),
	ID16(1100,  924, BUS100),
	ID16(1000,  908, BUS100),
	ID16( 900,  876, BUS100),
	ID16( 800,  860, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 753 1.2 GHz, VID #I */
static const uint16_t pm90_n753i[] = {
	ID16(1200,  924, BUS100),
	ID16(1100,  908, BUS100),
	ID16(1000,  892, BUS100),
	ID16( 900,  876, BUS100),
	ID16( 800,  860, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 753 1.2 GHz, VID #J */
static const uint16_t pm90_n753j[] = {
	ID16(1200,  908, BUS100),
	ID16(1100,  892, BUS100),
	ID16(1000,  876, BUS100),
	ID16( 900,  860, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 753 1.2 GHz, VID #K */
static const uint16_t pm90_n753k[] = {
	ID16(1200,  892, BUS100),
	ID16(1100,  892, BUS100),
	ID16(1000,  876, BUS100),
	ID16( 900,  860, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 753 1.2 GHz, VID #L */
static const uint16_t pm90_n753l[] = {
	ID16(1200,  876, BUS100),
	ID16(1100,  876, BUS100),
	ID16(1000,  860, BUS100),
	ID16( 900,  844, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 773 1.3 GHz, VID #G */
static const uint16_t pm90_n773g[] = {
	ID16(1300,  956, BUS100),
	ID16(1200,  940, BUS100),
	ID16(1100,  924, BUS100),
	ID16(1000,  908, BUS100),
	ID16( 900,  876, BUS100),
	ID16( 800,  860, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 773 1.3 GHz, VID #H */
static const uint16_t pm90_n773h[] = {
	ID16(1300,  940, BUS100),
	ID16(1200,  924, BUS100),
	ID16(1100,  908, BUS100),
	ID16(1000,  892, BUS100),
	ID16( 900,  876, BUS100),
	ID16( 800,  860, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 773 1.3 GHz, VID #I */
static const uint16_t pm90_n773i[] = {
	ID16(1300,  924, BUS100),
	ID16(1200,  908, BUS100),
	ID16(1100,  892, BUS100),
	ID16(1000,  876, BUS100),
	ID16( 900,  860, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 773 1.3 GHz, VID #J */
static const uint16_t pm90_n773j[] = {
	ID16(1300,  908, BUS100),
	ID16(1200,  908, BUS100),
	ID16(1100,  892, BUS100),
	ID16(1000,  876, BUS100),
	ID16( 900,  860, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 773 1.3 GHz, VID #K */
static const uint16_t pm90_n773k[] = {
	ID16(1300,  892, BUS100),
	ID16(1200,  892, BUS100),
	ID16(1100,  876, BUS100),
	ID16(1000,  860, BUS100),
	ID16( 900,  860, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 773 1.3 GHz, VID #L */
static const uint16_t pm90_n773l[] = {
	ID16(1300,  876, BUS100),
	ID16(1200,  876, BUS100),
	ID16(1100,  860, BUS100),
	ID16(1000,  860, BUS100),
	ID16( 900,  844, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  812, BUS100),
};

/* Intel Pentium M processor 738 1.4 GHz */
static const uint16_t pm90_n738[] = {
	ID16(1400, 1116, BUS100),
	ID16(1300, 1116, BUS100),
	ID16(1200, 1100, BUS100),
	ID16(1100, 1068, BUS100),
	ID16(1000, 1052, BUS100),
	ID16( 900, 1036, BUS100),
	ID16( 800, 1020, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 758 1.5 GHz */
static const uint16_t pm90_n758[] = {
	ID16(1500, 1116, BUS100),
	ID16(1400, 1116, BUS100),
	ID16(1300, 1100, BUS100),
	ID16(1200, 1084, BUS100),
	ID16(1100, 1068, BUS100),
	ID16(1000, 1052, BUS100),
	ID16( 900, 1036, BUS100),
	ID16( 800, 1020, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 778 1.6 GHz */
static const uint16_t pm90_n778[] = {
	ID16(1600, 1116, BUS100),
	ID16(1500, 1116, BUS100),
	ID16(1400, 1100, BUS100),
	ID16(1300, 1184, BUS100),
	ID16(1200, 1068, BUS100),
	ID16(1100, 1052, BUS100),
	ID16(1000, 1052, BUS100),
	ID16( 900, 1036, BUS100),
	ID16( 800, 1020, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 710 1.4 GHz, 533 MHz FSB */
static const uint16_t pm90_n710[] = {
	ID16(1400, 1340, BUS133),
	ID16(1200, 1228, BUS133),
	ID16(1000, 1148, BUS133),
	ID16( 800, 1068, BUS133),
	ID16( 600,  998, BUS133),
};

/* Intel Pentium M processor 715 1.5 GHz, VID #A */
static const uint16_t pm90_n715a[] = {
	ID16(1500, 1340, BUS100),
	ID16(1200, 1228, BUS100),
	ID16(1000, 1148, BUS100),
	ID16( 800, 1068, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 715 1.5 GHz, VID #B */
static const uint16_t pm90_n715b[] = {
	ID16(1500, 1324, BUS100),
	ID16(1200, 1212, BUS100),
	ID16(1000, 1148, BUS100),
	ID16( 800, 1068, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 715 1.5 GHz, VID #C */
static const uint16_t pm90_n715c[] = {
	ID16(1500, 1308, BUS100),
	ID16(1200, 1212, BUS100),
	ID16(1000, 1132, BUS100),
	ID16( 800, 1068, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 715 1.5 GHz, VID #D */
static const uint16_t pm90_n715d[] = {
	ID16(1500, 1276, BUS100),
	ID16(1200, 1180, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 725 1.6 GHz, VID #A */
static const uint16_t pm90_n725a[] = {
	ID16(1600, 1340, BUS100),
	ID16(1400, 1276, BUS100),
	ID16(1200, 1212, BUS100),
	ID16(1000, 1132, BUS100),
	ID16( 800, 1068, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 725 1.6 GHz, VID #B */
static const uint16_t pm90_n725b[] = {
	ID16(1600, 1324, BUS100),
	ID16(1400, 1260, BUS100),
	ID16(1200, 1196, BUS100),
	ID16(1000, 1132, BUS100),
	ID16( 800, 1068, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 725 1.6 GHz, VID #C */
static const uint16_t pm90_n725c[] = {
	ID16(1600, 1308, BUS100),
	ID16(1400, 1244, BUS100),
	ID16(1200, 1180, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 725 1.6 GHz, VID #D */
static const uint16_t pm90_n725d[] = {
	ID16(1600, 1276, BUS100),
	ID16(1400, 1228, BUS100),
	ID16(1200, 1164, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 730 1.6 GHz, 533 MHz FSB */
static const uint16_t pm90_n730[] = {
       ID16(1600, 1308, BUS133),
       ID16(1333, 1260, BUS133),
       ID16(1200, 1212, BUS133),
       ID16(1067, 1180, BUS133),
       ID16( 800,  988, BUS133),
};

/* Intel Pentium M processor 735 1.7 GHz, VID #A */
static const uint16_t pm90_n735a[] = {
	ID16(1700, 1340, BUS100),
	ID16(1400, 1244, BUS100),
	ID16(1200, 1180, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 735 1.7 GHz, VID #B */
static const uint16_t pm90_n735b[] = {
	ID16(1700, 1324, BUS100),
	ID16(1400, 1244, BUS100),
	ID16(1200, 1180, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 735 1.7 GHz, VID #C */
static const uint16_t pm90_n735c[] = {
	ID16(1700, 1308, BUS100),
	ID16(1400, 1228, BUS100),
	ID16(1200, 1164, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 735 1.7 GHz, VID #D */
static const uint16_t pm90_n735d[] = {
	ID16(1700, 1276, BUS100),
	ID16(1400, 1212, BUS100),
	ID16(1200, 1148, BUS100),
	ID16(1000, 1100, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 740 1.73 GHz, 533 MHz FSB */
static const uint16_t pm90_n740[] = {
       ID16(1733, 1356, BUS133),
       ID16(1333, 1212, BUS133),
       ID16(1067, 1100, BUS133),
       ID16( 800,  988, BUS133),
};

/* Intel Pentium M processor 745 1.8 GHz, VID #A */
static const uint16_t pm90_n745a[] = {
	ID16(1800, 1340, BUS100),
	ID16(1600, 1292, BUS100),
	ID16(1400, 1228, BUS100),
	ID16(1200, 1164, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 745 1.8 GHz, VID #B */
static const uint16_t pm90_n745b[] = {
	ID16(1800, 1324, BUS100),
	ID16(1600, 1276, BUS100),
	ID16(1400, 1212, BUS100),
	ID16(1200, 1164, BUS100),
	ID16(1000, 1116, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 745 1.8 GHz, VID #C */
static const uint16_t pm90_n745c[] = {
	ID16(1800, 1308, BUS100),
	ID16(1600, 1260, BUS100),
	ID16(1400, 1212, BUS100),
	ID16(1200, 1148, BUS100),
	ID16(1000, 1100, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 745 1.8 GHz, VID #D */
static const uint16_t pm90_n745d[] = {
	ID16(1800, 1276, BUS100),
	ID16(1600, 1228, BUS100),
	ID16(1400, 1180, BUS100),
	ID16(1200, 1132, BUS100),
	ID16(1000, 1084, BUS100),
	ID16( 800, 1036, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 750 1.86 GHz, 533 MHz FSB */
/* values extracted from \_PR\NPSS (via _PSS) SDST ACPI table */
static const uint16_t pm90_n750[] = {
	ID16(1867, 1308, BUS133),
	ID16(1600, 1228, BUS133),
	ID16(1333, 1148, BUS133),
	ID16(1067, 1068, BUS133),
	ID16( 800,  988, BUS133),
};

/* Intel Pentium M processor 755 2.0 GHz, VID #A */
static const uint16_t pm90_n755a[] = {
	ID16(2000, 1340, BUS100),
	ID16(1800, 1292, BUS100),
	ID16(1600, 1244, BUS100),
	ID16(1400, 1196, BUS100),
	ID16(1200, 1148, BUS100),
	ID16(1000, 1100, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 755 2.0 GHz, VID #B */
static const uint16_t pm90_n755b[] = {
	ID16(2000, 1324, BUS100),
	ID16(1800, 1276, BUS100),
	ID16(1600, 1228, BUS100),
	ID16(1400, 1180, BUS100),
	ID16(1200, 1132, BUS100),
	ID16(1000, 1084, BUS100),
	ID16( 800, 1036, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 755 2.0 GHz, VID #C */
static const uint16_t pm90_n755c[] = {
	ID16(2000, 1308, BUS100),
	ID16(1800, 1276, BUS100),
	ID16(1600, 1228, BUS100),
	ID16(1400, 1180, BUS100),
	ID16(1200, 1132, BUS100),
	ID16(1000, 1084, BUS100),
	ID16( 800, 1036, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 755 2.0 GHz, VID #D */
static const uint16_t pm90_n755d[] = {
	ID16(2000, 1276, BUS100),
	ID16(1800, 1244, BUS100),
	ID16(1600, 1196, BUS100),
	ID16(1400, 1164, BUS100),
	ID16(1200, 1116, BUS100),
	ID16(1000, 1084, BUS100),
	ID16( 800, 1036, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 760 2.0 GHz, 533 MHz FSB */
static const uint16_t pm90_n760[] = {
	ID16(2000, 1356, BUS133),
	ID16(1600, 1244, BUS133),
	ID16(1333, 1164, BUS133),
	ID16(1067, 1084, BUS133),
	ID16( 800,  988, BUS133),
};

/* Intel Pentium M processor 765 2.1 GHz, VID #A */
static const uint16_t pm90_n765a[] = {
	ID16(2100, 1340, BUS100),
	ID16(1800, 1276, BUS100),
	ID16(1600, 1228, BUS100),
	ID16(1400, 1180, BUS100),
	ID16(1200, 1132, BUS100),
	ID16(1000, 1084, BUS100),
	ID16( 800, 1036, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 765 2.1 GHz, VID #B */
static const uint16_t pm90_n765b[] = {
	ID16(2100, 1324, BUS100),
	ID16(1800, 1260, BUS100),
	ID16(1600, 1212, BUS100),
	ID16(1400, 1180, BUS100),
	ID16(1200, 1132, BUS100),
	ID16(1000, 1084, BUS100),
	ID16( 800, 1036, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 765 2.1 GHz, VID #C */
static const uint16_t pm90_n765c[] = {
	ID16(2100, 1308, BUS100),
	ID16(1800, 1244, BUS100),
	ID16(1600, 1212, BUS100),
	ID16(1400, 1164, BUS100),
	ID16(1200, 1116, BUS100),
	ID16(1000, 1084, BUS100),
	ID16( 800, 1036, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 765 2.1 GHz, VID #E */
static const uint16_t pm90_n765e[] = {
	ID16(2100, 1356, BUS100),
	ID16(1800, 1292, BUS100),
	ID16(1600, 1244, BUS100),
	ID16(1400, 1196, BUS100),
	ID16(1200, 1148, BUS100),
	ID16(1000, 1100, BUS100),
	ID16( 800, 1052, BUS100),
	ID16( 600,  988, BUS100),
};

/* Intel Pentium M processor 770 2.13 GHz */
static const uint16_t pm90_n770[] = {
	ID16(2133, 1356, BUS133),
	ID16(1867, 1292, BUS133),
	ID16(1600, 1212, BUS133),
	ID16(1333, 1148, BUS133),
	ID16(1067, 1068, BUS133),
	ID16( 800,  988, BUS133),
};

/* Intel Pentium M processor 780 2.26 GHz */ 
static const uint16_t pm90_n780[] = {
	ID16(2267, 1388, BUS133),
	ID16(1867, 1292, BUS133),
	ID16(1600, 1212, BUS133),
	ID16(1333, 1148, BUS133),
	ID16(1067, 1068, BUS133), 
	ID16( 800,  988, BUS133),
}; 

/*
 * VIA C7-M 500 MHz FSB, 400 MHz FSB, and ULV variants.
 * Data from the "VIA C7-M Processor BIOS Writer's Guide (v2.17)" datasheet.
 */

/* 1.00GHz Centaur C7-M ULV */
static const uint16_t C7M_770_ULV[] = {
	ID16(1000,  844, BUS100),
	ID16( 800,  796, BUS100),
	ID16( 600,  796, BUS100),
	ID16( 400,  796, BUS100),
};

/* 1.00GHz Centaur C7-M ULV */
static const uint16_t C7M_779_ULV[] = {
	ID16(1000,  796, BUS100),
	ID16( 800,  796, BUS100),
	ID16( 600,  796, BUS100),
	ID16( 400,  796, BUS100),
};

/* 1.20GHz Centaur C7-M ULV */
static const uint16_t C7M_772_ULV[] = {
	ID16(1200,  844, BUS100),
	ID16(1000,  844, BUS100),
	ID16( 800,  828, BUS100),
	ID16( 600,  796, BUS100),
	ID16( 400,  796, BUS100),
};

/* 1.50GHz Centaur C7-M ULV */
static const uint16_t C7M_775_ULV[] = {
	ID16(1500,  956, BUS100),
	ID16(1400,  940, BUS100),
	ID16(1000,  860, BUS100),
	ID16( 800,  828, BUS100),
	ID16( 600,  796, BUS100),
	ID16( 400,  796, BUS100),
};

/* 1.20GHz Centaur C7-M 400 MHz FSB */
static const uint16_t C7M_771[] = {
	ID16(1200,  860, BUS100),
	ID16(1000,  860, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  844, BUS100),
	ID16( 400,  844, BUS100),
};

/* 1.50GHz Centaur C7-M 400 MHz FSB */
static const uint16_t C7M_754[] = {
	ID16(1500, 1004, BUS100),
	ID16(1400,  988, BUS100),
	ID16(1000,  940, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  844, BUS100),
	ID16( 400,  844, BUS100),
};

/* 1.60GHz Centaur C7-M 400 MHz FSB */
static const uint16_t C7M_764[] = {
	ID16(1600, 1084, BUS100),
	ID16(1400, 1052, BUS100),
	ID16(1000, 1004, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  844, BUS100),
	ID16( 400,  844, BUS100),
};

/* 1.80GHz Centaur C7-M 400 MHz FSB */
static const uint16_t C7M_784[] = {
	ID16(1800, 1148, BUS100),
	ID16(1600, 1100, BUS100),
	ID16(1400, 1052, BUS100),
	ID16(1000, 1004, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  844, BUS100),
	ID16( 400,  844, BUS100),
};

/* 2.00GHz Centaur C7-M 400 MHz FSB */
static const uint16_t C7M_794[] = {
	ID16(2000, 1148, BUS100),
	ID16(1800, 1132, BUS100),
	ID16(1600, 1100, BUS100),
	ID16(1400, 1052, BUS100),
	ID16(1000, 1004, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  844, BUS100),
	ID16( 400,  844, BUS100),
};

/* 1.60GHz Centaur C7-M 533 MHz FSB */
static const uint16_t C7M_765[] = {
	ID16(1600, 1084, BUS133),
	ID16(1467, 1052, BUS133),
	ID16(1200, 1004, BUS133),
	ID16( 800,  844, BUS133),
	ID16( 667,  844, BUS133),
	ID16( 533,  844, BUS133),
};

/* 2.00GHz Centaur C7-M 533 MHz FSB */
static const uint16_t C7M_785[] = {
	ID16(1867, 1148, BUS133),
	ID16(1600, 1100, BUS133),
	ID16(1467, 1052, BUS133),
	ID16(1200, 1004, BUS133),
	ID16( 800,  844, BUS133),
	ID16( 667,  844, BUS133),
	ID16( 533,  844, BUS133),
};

/* 2.00GHz Centaur C7-M 533 MHz FSB */
static const uint16_t C7M_795[] = {
	ID16(2000, 1148, BUS133),
	ID16(1867, 1132, BUS133),
	ID16(1600, 1100, BUS133),
	ID16(1467, 1052, BUS133),
	ID16(1200, 1004, BUS133),
	ID16( 800,  844, BUS133),
	ID16( 667,  844, BUS133),
	ID16( 533,  844, BUS133),
};

/* 1.00GHz VIA Eden 90nm 'Esther' */
static const uint16_t eden90_1000[] = {
	ID16(1000,  844, BUS100),
	ID16( 800,  844, BUS100),
	ID16( 600,  844, BUS100),
	ID16( 400,  844, BUS100),
};

struct fqlist {
	int vendor;
	unsigned bus_clk;
	unsigned n;
	const uint16_t *table;
};

#define ENTRY(ven, bus_clk, tab) \
	{ CPUVENDOR_##ven, bus_clk == BUS133 ? 1 : 0, __arraycount(tab), tab }

#define BUS_CLK(fqp) ((fqp)->bus_clk ? BUS133 : BUS100)

static const struct fqlist est_cpus[] = {
	ENTRY(INTEL, BUS100, pm130_900_ulv),
	ENTRY(INTEL, BUS100, pm130_1000_ulv),
	ENTRY(INTEL, BUS100, pm130_1100_ulv),
	ENTRY(INTEL, BUS100, pm130_1100_lv),
	ENTRY(INTEL, BUS100, pm130_1200_lv),
	ENTRY(INTEL, BUS100, pm130_1300_lv),
	ENTRY(INTEL, BUS100, pm130_1300),
	ENTRY(INTEL, BUS100, pm130_1400),
	ENTRY(INTEL, BUS100, pm130_1500),
	ENTRY(INTEL, BUS100, pm130_1600),
	ENTRY(INTEL, BUS100, pm130_1700),
	ENTRY(INTEL, BUS100, pm90_n723),
	ENTRY(INTEL, BUS100, pm90_n733g),
	ENTRY(INTEL, BUS100, pm90_n733h),
	ENTRY(INTEL, BUS100, pm90_n733i),
	ENTRY(INTEL, BUS100, pm90_n733j),
	ENTRY(INTEL, BUS100, pm90_n733k),
	ENTRY(INTEL, BUS100, pm90_n733l),
	ENTRY(INTEL, BUS100, pm90_n753g),
	ENTRY(INTEL, BUS100, pm90_n753h),
	ENTRY(INTEL, BUS100, pm90_n753i),
	ENTRY(INTEL, BUS100, pm90_n753j),
	ENTRY(INTEL, BUS100, pm90_n753k),
	ENTRY(INTEL, BUS100, pm90_n753l),
	ENTRY(INTEL, BUS100, pm90_n773g),
	ENTRY(INTEL, BUS100, pm90_n773h),
	ENTRY(INTEL, BUS100, pm90_n773i),
	ENTRY(INTEL, BUS100, pm90_n773j),
	ENTRY(INTEL, BUS100, pm90_n773k),
	ENTRY(INTEL, BUS100, pm90_n773l),
	ENTRY(INTEL, BUS100, pm90_n738),
	ENTRY(INTEL, BUS100, pm90_n758),
	ENTRY(INTEL, BUS100, pm90_n778),

	ENTRY(INTEL, BUS133, pm90_n710),
	ENTRY(INTEL, BUS100, pm90_n715a),
	ENTRY(INTEL, BUS100, pm90_n715b),
	ENTRY(INTEL, BUS100, pm90_n715c),
	ENTRY(INTEL, BUS100, pm90_n715d),
	ENTRY(INTEL, BUS100, pm90_n725a),
	ENTRY(INTEL, BUS100, pm90_n725b),
	ENTRY(INTEL, BUS100, pm90_n725c),
	ENTRY(INTEL, BUS100, pm90_n725d),
	ENTRY(INTEL, BUS133, pm90_n730),
	ENTRY(INTEL, BUS100, pm90_n735a),
	ENTRY(INTEL, BUS100, pm90_n735b),
	ENTRY(INTEL, BUS100, pm90_n735c),
	ENTRY(INTEL, BUS100, pm90_n735d),
	ENTRY(INTEL, BUS133, pm90_n740),
	ENTRY(INTEL, BUS100, pm90_n745a),
	ENTRY(INTEL, BUS100, pm90_n745b),
	ENTRY(INTEL, BUS100, pm90_n745c),
	ENTRY(INTEL, BUS100, pm90_n745d),
	ENTRY(INTEL, BUS133, pm90_n750),
	ENTRY(INTEL, BUS100, pm90_n755a),
	ENTRY(INTEL, BUS100, pm90_n755b),
	ENTRY(INTEL, BUS100, pm90_n755c),
	ENTRY(INTEL, BUS100, pm90_n755d),
	ENTRY(INTEL, BUS133, pm90_n760),
	ENTRY(INTEL, BUS100, pm90_n765a),
	ENTRY(INTEL, BUS100, pm90_n765b),
	ENTRY(INTEL, BUS100, pm90_n765c),
	ENTRY(INTEL, BUS100, pm90_n765e),
	ENTRY(INTEL, BUS133, pm90_n770),
	ENTRY(INTEL, BUS133, pm90_n780),

	ENTRY(IDT, BUS100, C7M_770_ULV),
	ENTRY(IDT, BUS100, C7M_779_ULV),
	ENTRY(IDT, BUS100, C7M_772_ULV),
	ENTRY(IDT, BUS100, C7M_771),
	ENTRY(IDT, BUS100, C7M_775_ULV),
	ENTRY(IDT, BUS100, C7M_754),
	ENTRY(IDT, BUS100, C7M_764),
	ENTRY(IDT, BUS133, C7M_765),
	ENTRY(IDT, BUS100, C7M_784),
	ENTRY(IDT, BUS133, C7M_785),
	ENTRY(IDT, BUS100, C7M_794),
	ENTRY(IDT, BUS133, C7M_795),

	ENTRY(IDT, BUS100, eden90_1000)
};

#define MSR2FREQINC(msr)	(((int) (msr) >> 8) & 0xff)
#define MSR2VOLTINC(msr)	((int) (msr) & 0xff)

#define MSR2MHZ(msr, bus)	((MSR2FREQINC((msr)) * (bus) + 50) / 100)
#define MSR2MV(msr)		(MSR2VOLTINC(msr) * 16 + 700)

static const struct 	fqlist *est_fqlist;	/* not NULL if functional */
static uint16_t		*fake_table;		/* guessed est_cpu table */
static struct fqlist    fake_fqlist;
static int 		est_node_target, est_node_current;
static const char 	est_desc[] = "Enhanced SpeedStep";
static int		lvendor, bus_clock;

static int		est_sysctl_helper(SYSCTLFN_PROTO);
static int		est_init_once(void);
static void		est_init_main(int);

#define PHC_ID16(FID, VID)	( ((FID) << 8) | (VID) )
#define PHC_MAXLEN		30
static uint16_t*	phc_origin_table;	/* PHC: keep orignal settings */
static char		*phc_string_vids;
static int		phc_est_sysctl_helper(SYSCTLFN_PROTO);

/* atoi clone:
 * parse a string, discarding no-digit character
 * and returning the first int found.
 *
 * (*remain) points to the remaining string.
 *
 * return -1 on error / end of string
 */
static int
phc_atoi(char *string, char **remain)
{
	char *pc = string;
	int result = 0;

	while ( (*pc != '\0') && ! isdigit(*pc) )
		pc++;

	if (*pc == '\0')
		return -1;

	while (isdigit(*pc)) {
		result = *pc - '0' + 10 * (result);
		pc++;
	}

	return *remain = pc, result;
}

static int
phc_est_sysctl_helper(SYSCTLFN_ARGS)
{
	struct sysctlnode	node;
	int			error;
	char			input_string[PHC_MAXLEN];
	struct msr_cpu_broadcast mcb;
	int			i,fq,*vids;
	char			*string,*remain;

	if (est_fqlist == NULL)
		return EOPNOTSUPP;

	strlcpy( input_string, phc_string_vids, sizeof(input_string));

	node = *rnode;
	node.sysctl_data = input_string;
	node.sysctl_size = sizeof(input_string);

	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error
		|| newp == NULL
		|| strncmp(input_string, phc_string_vids, PHC_MAXLEN) == 0 )
		return error;

	/* () input_string is different, process it () */

	/* Parse input string one Voltage ID at a time */
	remain = string = input_string;

	vids = kmem_alloc(est_fqlist->n * sizeof(int), KM_SLEEP);
	if (vids == NULL)
		return ENOMEM;

	/* First round: check input values */
	for( i = 0; i< est_fqlist->n; i++) {
		int ref_vid = MSR2VOLTINC(phc_origin_table[i]);
		int vid = phc_atoi( string, &remain);

		if (vid == -1) {
			printf("%s: require at least %d values\n",
					__func__, est_fqlist->n);
			kmem_free( vids, est_fqlist->n * sizeof(int));
			return EINVAL;
		}

		if ( vid < 0 || vid > ref_vid ) {
			printf("%s: %d VID out of bounds\n",
					__func__, vid);
			kmem_free( vids, est_fqlist->n * sizeof(int));
			return EINVAL;
		}

		vids[i] = vid;

		/* iteration */
		string = remain;
	}

	/* ignoring rest of string
	 * in case where input_string is too long */
	*remain = '\0';

	/* Save new VIDs */
	for( i = 0; i< est_fqlist->n; i++) {
		int ref_fid = MSR2FREQINC(phc_origin_table[i]);
		fake_table[i] = PHC_ID16( ref_fid, vids[i]);
#ifdef EST_DEBUG
		printf("PHC: using new VID %d for FID %d\n"
					, vids[i], ref_fid);
#endif /* EST_DEBUG */
	}

	/* clean memory <!> */
	kmem_free( vids, est_fqlist->n * sizeof(int));

	/* save string for futur display */
	strncpy ( phc_string_vids, input_string, PHC_MAXLEN);

	/* reset MSR */
	fq = MSR2MHZ(rdmsr(MSR_PERF_STATUS), bus_clock);
	for (i = est_fqlist->n - 1; i > 0; i--)
		if (MSR2MHZ(est_fqlist->table[i], bus_clock) >= fq)
			break;
	fq = MSR2MHZ(est_fqlist->table[i], bus_clock);
	mcb.msr_read = true;
	mcb.msr_type = MSR_PERF_CTL;
	mcb.msr_mask = 0xffffULL;
	mcb.msr_value = est_fqlist->table[i];
	msr_cpu_broadcast(&mcb);

	/* Display raw VID voltages */
	/*rnode->sysctl_data =  &phc_string_vids;*/

	return 0;
}

static int
est_sysctl_helper(SYSCTLFN_ARGS)
{
	struct msr_cpu_broadcast mcb;
	struct sysctlnode	node;
	int			fq, oldfq, error;

	if (est_fqlist == NULL)
		return EOPNOTSUPP;

	node = *rnode;
	node.sysctl_data = &fq;

	oldfq = 0;
	if (rnode->sysctl_num == est_node_target)
		fq = oldfq = MSR2MHZ(rdmsr(MSR_PERF_CTL), bus_clock);
	else if (rnode->sysctl_num == est_node_current)
		fq = MSR2MHZ(rdmsr(MSR_PERF_STATUS), bus_clock);
	else
		return EOPNOTSUPP;

	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	/* support writing to ...frequency.target */
	if (rnode->sysctl_num == est_node_target && fq != oldfq) {
		int		i;

		for (i = est_fqlist->n - 1; i > 0; i--)
			if (MSR2MHZ(est_fqlist->table[i], bus_clock) >= fq)
				break;
		fq = MSR2MHZ(est_fqlist->table[i], bus_clock);
		mcb.msr_read = true;
		mcb.msr_type = MSR_PERF_CTL;
		mcb.msr_mask = 0xffffULL;
		mcb.msr_value = est_fqlist->table[i];
		msr_cpu_broadcast(&mcb);
	}

	return 0;
}

static int
est_init_once(void)
{
	est_init_main(lvendor);
	return 0;
}

void
est_init(int vendor)
{
	int error;
	static ONCE_DECL(est_initialized);

	lvendor = vendor;

	error = RUN_ONCE(&est_initialized, est_init_once);
	if (__predict_false(error != 0))
		return;
}

static void
est_init_main(int vendor)
{
#ifdef __i386__
	const struct fqlist	*fql;
#endif
	const struct sysctlnode	*node, *estnode, *freqnode;
	uint64_t		msr;
	uint16_t		cur, idhi, idlo;
	uint8_t			crhi, crlo, crcur;
	int			i, mv, rc;
	size_t			len, freq_len;
	char			*freq_names;
	const char *cpuname;
	const struct sysctlnode	*voltnode;
	size_t			vids_len,fids_len;
	char			*phc_original_vids,*phc_fids;

	cpuname	= device_xname(curcpu()->ci_dev);

	if (CPUID2FAMILY(curcpu()->ci_signature) == 15)
		bus_clock = p4_get_bus_clock(curcpu());
	else if (CPUID2FAMILY(curcpu()->ci_signature) == 6) {
		if (vendor == CPUVENDOR_IDT)
			bus_clock = via_get_bus_clock(curcpu());
		else
			bus_clock = p3_get_bus_clock(curcpu());
	}

	if (bus_clock == 0) {
		aprint_debug("%s: unknown system bus clock\n", __func__);
		return;
	}

	msr = rdmsr(MSR_PERF_STATUS);
	idhi = (msr >> 32) & 0xffff;
	idlo = (msr >> 48) & 0xffff;
	cur = msr & 0xffff;
	crhi = (idhi  >> 8) & 0xff;
	crlo = (idlo  >> 8) & 0xff;
	crcur = (cur >> 8) & 0xff;

#ifdef __i386__
	if (idhi == 0 || idlo == 0 || cur == 0 ||
	    ((cur >> 8) & 0xff) < ((idlo >> 8) & 0xff) ||
	    ((cur >> 8) & 0xff) > ((idhi >> 8) & 0xff)) {
		aprint_debug("%s: strange msr value 0x%016llx\n", __func__, msr);
		return;
	}
#endif

#ifdef __amd64__
	if (crlo == 0 || crhi == crlo) {
		aprint_debug("%s: crlo == 0 || crhi == crlo\n", __func__);
		return;
	}

	if (crhi == 0 || crcur == 0 || crlo > crhi ||
	    crcur < crlo || crcur > crhi) {
		/*
		 * Do complain about other weirdness, because we first want to
		 * know about it, before we decide what to do with it
		 */
		aprint_debug("%s: strange msr value 0x%" PRIu64 "\n",
		    __func__, msr);
		return;
	}
#endif

	msr = rdmsr(MSR_PERF_STATUS);
	mv = MSR2MV(msr);

#ifdef __i386__
	/*
	 * Find an entry which matches (vendor, bus_clock, idhi, idlo)
	 */
	est_fqlist = NULL;
	for (i = 0; i < __arraycount(est_cpus); i++) {
		fql = &est_cpus[i];
		if (vendor == fql->vendor && bus_clock == BUS_CLK(fql) &&
		    idhi == fql->table[0] && idlo == fql->table[fql->n - 1]) {
			est_fqlist = fql;
			break;
		}
	}
#endif

	if (est_fqlist == NULL) {
		int j, tablesize, freq, volt;
		int minfreq, minvolt, maxfreq, maxvolt, freqinc, voltinc;

		/*
		 * Some CPUs report the same frequency in idhi and idlo,
		 * so do not run est on them.
		 */
		if (idhi == idlo) {
			aprint_debug("%s: idhi == idlo\n", __func__);
			return;
		}

#ifdef EST_DEBUG
		printf("%s: bus_clock = %d\n", __func__, bus_clock);
		printf("%s: idlo = 0x%x\n", __func__, idlo);
		printf("%s: lo  %4d mV, %4d MHz\n", __func__,
		    MSR2MV(idlo), MSR2MHZ(idlo, bus_clock));
		printf("%s: raw %4d   , %4d    \n", __func__,
		    (idlo & 0xff), ((idlo >> 8) & 0xff));
		printf("%s: idhi = 0x%x\n", __func__, idhi);
		printf("%s: hi  %4d mV, %4d MHz\n", __func__,
		    MSR2MV(idhi), MSR2MHZ(idhi, bus_clock));
		printf("%s: raw %4d   , %4d    \n", __func__,
		    (idhi & 0xff), ((idhi >> 8) & 0xff));
		printf("%s: cur  = 0x%x\n", __func__, cur);
#endif

                /*
                 * Generate a fake table with the power states we know,
		 * interpolating the voltages and frequencies between the
		 * high and low values.  The (milli)voltages are always
		 * rounded up when computing the table.
                 */
		minfreq = MSR2FREQINC(idlo);
		maxfreq = MSR2FREQINC(idhi);
		minvolt = MSR2VOLTINC(idlo);
		maxvolt = MSR2VOLTINC(idhi);
		freqinc = maxfreq - minfreq;
		voltinc = maxvolt - minvolt;

		/* Avoid diving by zero. */
		if (freqinc == 0)
			return;

		if (freqinc < voltinc || voltinc == 0) {
			tablesize = maxfreq - minfreq + 1;
			if (voltinc != 0)
				voltinc = voltinc * 100 / freqinc - 1;
			freqinc = 100;
		} else {
			tablesize = maxvolt - minvolt + 1;
			freqinc = freqinc * 100 / voltinc - 1;
			voltinc = 100;
		}

		fake_table = malloc(tablesize * sizeof(uint16_t), M_DEVBUF,
		    M_WAITOK);
		fake_fqlist.n = tablesize;

		/* The frequency/voltage table is highest frequency first */
		freq = maxfreq * 100;
		volt = maxvolt * 100;
		for (j = 0; j < tablesize; j++) {
			fake_table[j] = (((freq + 99) / 100) << 8) +
			    (volt + 99) / 100;
#ifdef EST_DEBUG
			printf("%s: fake entry %d: %4d mV, %4d MHz  "
			    "MSR*100 mV = %4d freq = %4d\n",
			    __func__, j, MSR2MV(fake_table[j]),
			    MSR2MHZ(fake_table[j], bus_clock),
			    volt, freq);
#endif /* EST_DEBUG */
			freq -= freqinc;
			volt -= voltinc;
		}
		fake_fqlist.vendor = vendor;
		fake_fqlist.table = fake_table;
		est_fqlist = &fake_fqlist;
	}
	else {
		/* PHC: Create a non-const fake table
		 * in order to modify volt values later */
		int tablesize = est_fqlist->n * sizeof(uint16_t);

#ifdef EST_DEBUG
		printf("PHC: replacing static const table\n");
#endif /* EST_DEBUG */

		printf("%s: bus_clock = %d\n", __func__, bus_clock);
		fake_table = malloc( tablesize, M_DEVBUF, M_WAITOK);
		memcpy( fake_table, est_fqlist->table, tablesize);
		fake_fqlist.table = fake_table;

		fake_fqlist.n = est_fqlist->n;
		fake_fqlist.vendor = est_fqlist->vendor;
		fake_fqlist.bus_clk = est_fqlist->bus_clk;

		est_fqlist = &fake_fqlist;
	}

	/* PHC: keep original setting in memory */
	memcpy( &phc_origin_table, &fake_table, sizeof(fake_table));

	/*
	 * OK, tell the user the available frequencies.
	 */
	freq_len = est_fqlist->n * (sizeof("9999 ")-1) + 1;
	freq_names = malloc(freq_len, M_SYSCTLDATA, M_WAITOK);
	freq_names[0] = '\0';
	len = 0;
	for (i = 0; i < est_fqlist->n; i++) {
		len += snprintf(freq_names + len, freq_len - len,
			"%d%s", MSR2MHZ(est_fqlist->table[i], bus_clock),
		    i < est_fqlist->n - 1 ? " " : "");
	}

	aprint_normal("%s: %s (%d mV) ", cpuname, est_desc, mv);
	aprint_normal("%d MHz\n", MSR2MHZ(msr, bus_clock));
	aprint_normal("%s: %s frequencies available (MHz): %s\n",
	    cpuname, est_desc, freq_names);

	/* PHC: create initial string representation of FIDs */
	fids_len = est_fqlist->n * 3 + 1;
	phc_fids = kmem_alloc(fids_len, KM_SLEEP);
	phc_fids[0] = '\0';
	len = 0;
	for (i = 0; i < est_fqlist->n && len < PHC_MAXLEN; i++) {
		len += snprintf(phc_fids + len, fids_len - len, "%d%s",
		    MSR2FREQINC(est_fqlist->table[i]),
		    i < est_fqlist->n - 1 ? " " : "");
	}
	aprint_normal("%s: %s frequences id used: %s\n",
	    cpuname, est_desc, phc_fids);

	/* PHC: create initial string representation of VIDs */
	vids_len = est_fqlist->n * 3 + 1;
	phc_original_vids = kmem_alloc(vids_len, KM_SLEEP);
	phc_original_vids[0] = '\0';
	len = 0;
	for (i = 0; i < est_fqlist->n && len < PHC_MAXLEN; i++) {
		len += snprintf(phc_original_vids + len, vids_len - len, "%d%s",
		    MSR2VOLTINC(est_fqlist->table[i]),
		    i < est_fqlist->n - 1 ? " " : "");
	}
	aprint_normal("%s: %s voltages id used: %s\n",
	    cpuname, est_desc, phc_original_vids);

	/* PHC: create initial VIDs by copying original ones */
	phc_string_vids = kmem_alloc( PHC_MAXLEN, KM_SLEEP);
	strlcpy( phc_string_vids, phc_original_vids, vids_len);

	/*
	 * Setup the sysctl sub-tree machdep.est.*
	 */
	if ((rc = sysctl_createv(NULL, 0, NULL, &node,
	    CTLFLAG_PERMANENT, CTLTYPE_NODE, "machdep", NULL,
	    NULL, 0, NULL, 0, CTL_MACHDEP, CTL_EOL)) != 0)
		goto err;

	if ((rc = sysctl_createv(NULL, 0, &node, &estnode,
	    0, CTLTYPE_NODE, "est", NULL,
	    NULL, 0, NULL, 0, CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	if ((rc = sysctl_createv(NULL, 0, &estnode, &freqnode,
	    0, CTLTYPE_NODE, "frequency", NULL,
	    NULL, 0, NULL, 0, CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	if ((rc = sysctl_createv(NULL, 0, &freqnode, &node,
	    EST_TARGET_CTLFLAG, CTLTYPE_INT, "target", NULL,
	    est_sysctl_helper, 0, NULL, 0, CTL_CREATE, CTL_EOL)) != 0)
		goto err;
	est_node_target = node->sysctl_num;

	if ((rc = sysctl_createv(NULL, 0, &freqnode, &node,
	    0, CTLTYPE_INT, "current", NULL,
	    est_sysctl_helper, 0, NULL, 0, CTL_CREATE, CTL_EOL)) != 0)
		goto err;
	est_node_current = node->sysctl_num;

	if ((rc = sysctl_createv(NULL, 0, &freqnode, &node,
	    0, CTLTYPE_STRING, "available", NULL,
	    NULL, 0, freq_names, freq_len, CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	/* PHC: Adding a voltage subtree */
	if ((rc = sysctl_createv(NULL, 0, &estnode, &voltnode,
	    0, CTLTYPE_NODE, "phc", NULL,
	    NULL, 0, NULL, 0, CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	if ((rc = sysctl_createv(NULL, 0, &voltnode, NULL,
	    0, CTLTYPE_STRING, "fids",
	    SYSCTL_DESCR("Frequence ID list"),
	    NULL, 0, phc_fids, fids_len,
	    CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	if ((rc = sysctl_createv(NULL, 0, &voltnode, NULL,
	    0, CTLTYPE_STRING, "vids_original",
	    SYSCTL_DESCR("Original voltage ID list"),
	    NULL, 0, phc_original_vids, vids_len,
	    CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	if ((rc = sysctl_createv(NULL, 0, &voltnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_STRING, "vids",
	    SYSCTL_DESCR("Custom voltage ID list"),
	    phc_est_sysctl_helper, 0, NULL, PHC_MAXLEN,
	    CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	return;

 err:
	free(freq_names, M_SYSCTLDATA);
	/*free(fake_table, M_SYSCTLDATA); (! shoud we ??? !) */
	kmem_free(phc_fids, fids_len);
	kmem_free(phc_original_vids, vids_len);
	aprint_error("%s: sysctl_createv failed (rc = %d)\n", __func__, rc);
}