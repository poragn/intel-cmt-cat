/*
 * BSD LICENSE
 *
 * Copyright(c) 2014-2015 Intel Corporation. All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.O
 *
 */

/**
 * @brief Host implementation of PQoS API / capabilities.
 *
 * This module is responsible for PQoS management and capability
 * functionalities.
 *
 * Management functions include:
 * - management includes initializing and shutting down all other sub-modules
 *   including: monitoring, allocation, log, cpuinfo and machine
 * - provide functions for safe access to PQoS API - this is required for
 *   allocation and monitoring modules which also implement PQoS API
 *
 * Capability functions:
 * - monitoring detection, this is to discover all monitoring event types.
 *   LLC occupancy is only supported now.
 * - LLC allocation detection, this is to discover last level cache
 *   allocation feature.
 * - A new targeted function has to be implemented to discover new allocation
 *   technology.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "pqos.h"

#include "host_cap.h"
#include "host_allocation.h"
#include "host_monitoring.h"

#include "cpuinfo.h"
#include "machine.h"
#include "types.h"
#include "log.h"

/**
 * ---------------------------------------
 * Local macros
 * ---------------------------------------
 */

/**
 * Available types of allocation resource IDs.
 * (matches CPUID enumeration)
 */
#define PQOS_RES_ID_L3_ALLOCATION    1              /**< L3 cache allocation */

#define PQOS_CPUID_CAT_CDP_BIT       2              /**< CDP supported bit */

#define PQOS_MSR_L3_QOS_CFG          0xC81          /**< CAT config register */
#define PQOS_MSR_L3_QOS_CFG_CDP_EN   1ULL           /**< CDP enable bit */

#define PQOS_MSR_L3CA_MASK_START     0xC90          /**< CAT class 0 register */
#define PQOS_MSR_ASSOC               0xC8F          /**< CAT class to core association register */
#define PQOS_MSR_ASSOC_QECOS_SHIFT   32
#define PQOS_MSR_ASSOC_QECOS_MASK    0xffffffff00000000ULL

/**
 * ---------------------------------------
 * Local data types
 * ---------------------------------------
 */


/**
 * ---------------------------------------
 * Local data structures
 * ---------------------------------------
 */

/**
 * This pointer is allocated and initialized in this module.
 * Then other sub-modules get this pointer in order to retrieve
 * capability information.
 */
static struct pqos_cap *m_cap = NULL;

/**
 * This gets allocated and initialized in this module.
 * This hold information about CPU topology in PQoS format.
 */
static struct pqos_cpuinfo *m_cpu = NULL;

/**
 * Library initialization status.
 */
static int m_init_done = 0;

/**
 * API thread safe access is secured through this mutex.
 */
static pthread_mutex_t m_apilock = PTHREAD_MUTEX_INITIALIZER;

/**
 * ---------------------------------------
 * Functions for safe multi-threading
 * ---------------------------------------
 */

void _pqos_api_lock(void)
{
        int ret = 0;

        ret = pthread_mutex_lock(&m_apilock);
        ASSERT(ret == 0);
        if (ret != 0)
                LOG_ERROR("API lock failed!\n");
}

void _pqos_api_unlock(void)
{
        int ret = 0;

        ret = pthread_mutex_unlock(&m_apilock);
        ASSERT(ret == 0);
        if (ret != 0)
                LOG_ERROR("API unlock failed!\n");
}

/**
 * ---------------------------------------
 * Function for library initialization
 * ---------------------------------------
 */

int _pqos_check_init(const int expect)
{
        if (m_init_done && (!expect)) {
                LOG_ERROR("PQoS library already initialized\n");
                return PQOS_RETVAL_INIT;
        }

        if ((!m_init_done) && expect) {
                LOG_ERROR("PQoS library not initialized\n");
                return PQOS_RETVAL_INIT;
        }

        return PQOS_RETVAL_OK;
}

/**
 * =======================================
 * =======================================
 *
 * Capability discovery routines
 *
 * =======================================
 * =======================================
 */

/**
 * @brief Detects LLC size and number of ways
 *
 * Retrieves information about L3 cache
 * and calculates its size. Uses CPUID.0x04.0x03
 *
 * @param p_num_ways place to store number of detected cache ways
 * @param p_size_in_bytes place to store size of LLC in bytes
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK success
 */
static int
get_l3_cache_info(unsigned *p_num_ways,
                  unsigned *p_size_in_bytes)
{
        unsigned num_ways = 0, line_size = 0, num_partitions = 0,
                num_sets = 0, size_in_bytes = 0;
        struct cpuid_out res;
        int ret = PQOS_RETVAL_OK;

        if (p_num_ways == NULL && p_size_in_bytes == NULL)
                return PQOS_RETVAL_PARAM;

        ret = lcpuid(0x4, 0x3, &res);
        if (ret != MACHINE_RETVAL_OK)
                return PQOS_RETVAL_ERROR;

        num_ways = (res.ebx>>22) + 1;
        if (p_num_ways != NULL)
                *p_num_ways = num_ways;
        line_size = (res.ebx & 0xfff) + 1;
        num_partitions = ((res.ebx >> 12) & 0x3ff) + 1;
        num_sets = res.ecx + 1;
        size_in_bytes = num_ways*num_partitions*line_size*num_sets;
        if (p_size_in_bytes != NULL)
                *p_size_in_bytes = size_in_bytes;

        return ret;
}

/**
 * @brief Adds new event type to \a mon monitoring structure
 *
 * @param mon Monitoring structure which is to be updated with the new event type
 * @param res_id resource id
 * @param event_type event type
 * @param max_rmid max RMID for the event
 * @param scale_factor event specific scale factor
 * @param max_num_events maximum number of events that \a mon can accommodate
 */
static void
add_monitoring_event(struct pqos_cap_mon *mon,
                     const unsigned res_id,
                     const int event_type,
                     const unsigned max_rmid,
                     const uint32_t scale_factor,
                     const unsigned max_num_events)
{
        if (mon->num_events >= max_num_events) {
                LOG_WARN("%s() no space for event type %d (resource id %u)!\n",
                          __func__, event_type, res_id);
                return;
        }

        LOG_INFO("Adding monitoring event: resource ID %u, "
                 "type %d to table index %u\n",
                 res_id, event_type, mon->num_events);

        mon->events[mon->num_events].type = (enum pqos_mon_event) event_type;
        mon->events[mon->num_events].max_rmid = max_rmid;
        mon->events[mon->num_events].scale_factor = scale_factor;
        mon->num_events++;
}

/**
 * @brief Discovers monitoring capabilities
 *
 * Runs series of CPUID instructions to discover system CMT
 * capabilities.
 * Allocates memory for monitoring structure and
 * returns it through \a r_mon to the caller.
 *
 * @param r_mon place to store created monitoring structure
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK success
 */
static int
discover_monitoring(struct pqos_cap_mon **r_mon)
{
        struct cpuid_out res;
        int ret = PQOS_RETVAL_OK;
        unsigned sz = 0, max_rmid = 0,
                l3_size = 0, num_events = 0;
        struct pqos_cap_mon *mon = NULL;

        ASSERT(r_mon != NULL);

        /**
         * Run CPUID.0x7.0 to check
         * for quality monitoring capability (bit 12 of ebx)
         */
        ret = lcpuid(0x7, 0x0, &res);
        if (ret != MACHINE_RETVAL_OK)
                return PQOS_RETVAL_ERROR;

        if (!(res.ebx&(1<<12))) {
                LOG_WARN("Cache monitoring "
                          "capability not supported!\n");
                return PQOS_RETVAL_ERROR;
        }

        /**
         * We can go to CPUID.0xf.0 for further
         * exploration of monitoring capabilities
         */
        ret = lcpuid(0xf, 0x0, &res);
        if (ret != MACHINE_RETVAL_OK)
                return PQOS_RETVAL_ERROR;

        /**
         * MAX_RMID for the socket
         */
        max_rmid = (unsigned) res.ebx + 1;
        ret = get_l3_cache_info(NULL, &l3_size);   /**< L3 cache size */
        if (ret != PQOS_RETVAL_OK)
                return ret;

        /**
         * Check number of monitoring events to allocate memory for
         */
        if (res.edx&(1<<1)) {
                /**
                 * Bit 1 resource monitoring available
                 */
                struct cpuid_out tmp_res;

                ret = lcpuid(0xf, 1, &tmp_res); /**< query resource
                                                   monitoring */
                if (ret != MACHINE_RETVAL_OK)
                        return PQOS_RETVAL_ERROR;
                if (tmp_res.edx&1)
                        num_events++; /**< LLC occupancy */
                if (tmp_res.edx&2)
                        num_events++; /**< local memory bandwidth event */
                if (tmp_res.edx&4)
                        num_events++; /**< total memory bandwidth event */
                if ((tmp_res.edx&2) && (tmp_res.edx&4))
                        num_events++; /**< remote memory bandwidth
                                         virtual event */
        }

        if (!num_events)
                return PQOS_RETVAL_ERROR;

        sz = (num_events * sizeof(struct pqos_monitor)) + sizeof(*mon);
        mon = (struct pqos_cap_mon *) malloc(sz);
        if (mon == NULL)
                return PQOS_RETVAL_RESOURCE;

        memset(mon, 0, sz);
        mon->mem_size = sz;
        mon->max_rmid = max_rmid;
        mon->l3_size = l3_size;

        if (res.edx&(1<<1)) {
                /**
                 * Bit 1 resource monitoring available
                 */
                struct cpuid_out tmp_res;

                ret = lcpuid(0xf, 1, &tmp_res); /**< query resource
                                                   monitoring */
                if (ret != MACHINE_RETVAL_OK) {
                        free(mon);
                        return PQOS_RETVAL_ERROR;
                }

                if (tmp_res.edx&1)
                        add_monitoring_event(mon, 1,
                                             PQOS_MON_EVENT_L3_OCCUP,
                                             tmp_res.ecx+1,
                                             tmp_res.ebx,
                                             num_events);
                if (tmp_res.edx&2)
                        add_monitoring_event(mon, 1,
                                             PQOS_MON_EVENT_LMEM_BW,
                                             tmp_res.ecx+1,
                                             tmp_res.ebx,
                                             num_events);
                if (tmp_res.edx&4)
                        add_monitoring_event(mon, 1,
                                             PQOS_MON_EVENT_TMEM_BW,
                                             tmp_res.ecx+1,
                                             tmp_res.ebx,
                                             num_events);

                if ((tmp_res.edx&2) && (tmp_res.edx&4))
                        add_monitoring_event(mon, 1,
                                             PQOS_MON_EVENT_RMEM_BW,
                                             tmp_res.ecx+1,
                                             tmp_res.ebx,
                                             num_events);
        }

        (*r_mon) = mon;
        return PQOS_RETVAL_OK;
}

/**
 * @brief Retrieves & allocates memory for list of socket ID's
 *
 * @param cpu detected CPU topology
 * @param number place to store number of sockets
 * @param table place to put pointer to allocated table wit socket ID's
 *
 * @return Operations status
 * @retval PQOS_RETVAL_OK on success
 */
static int
get_cpu_sockets(const struct pqos_cpuinfo *cpu,
		unsigned *number,
		unsigned **table)
{
        unsigned *sockets = NULL;
        unsigned sockets_num = 0, sockets_count = 0;
        int ret = PQOS_RETVAL_OK;

        ASSERT(cpu != NULL);
        ASSERT(number != NULL);
        ASSERT(table != NULL);

	*number = 0;
	*table = NULL;

        ret = pqos_cpu_get_num_sockets(cpu, &sockets_count);
        if (ret != PQOS_RETVAL_OK)
		return ret;

	sockets = malloc(sizeof(sockets[0]) * sockets_count);
	if (sockets == NULL)
		return PQOS_RETVAL_RESOURCE;

        ret = pqos_cpu_get_sockets(cpu, sockets_count,
				   &sockets_num, sockets);
        if (ret == PQOS_RETVAL_OK) {
		*number = sockets_num;
		*table = sockets;
	}

	return ret;
}

/**
 * @brief Resets CAT configuration at lower level than \a pqos_l3ca_reset
 *
 * Low level CAT reset is required for CDP in scenarios when
 * - CDP is ON and it is requested to turn it OFF
 * - CDP is OFF and it is requested to turn it ON
 *
 * @param cap detected CAT capabilities
 * @param cpu detected CPU topology
 *
 * @return Operations status
 * @retval PQOS_RETVAL_OK on success
 */
static int
cat_reset(const struct pqos_cap_l3ca *cap,
          const struct pqos_cpuinfo *cpu)
{
        unsigned *sockets = NULL;
        unsigned sockets_num = 0;
        int ret = PQOS_RETVAL_OK;
        unsigned j;

        ASSERT(cap != NULL && cpu != NULL);
        if (cap == NULL || cpu == NULL)
                return PQOS_RETVAL_PARAM;

        LOG_INFO("Resetting CAT configuration across all sockets...\n");

        /**
         * Get list of socket id's through another API
         */
	ret = get_cpu_sockets(cpu, &sockets_num, &sockets);
        if (ret != PQOS_RETVAL_OK)
                return ret;

	ASSERT(sockets_num > 0);
	ASSERT(sockets != NULL);

        /**
         * Change COS definition on all sockets
         * so that each COS allows for access to all cache ways
         */
        for (j = 0; j < sockets_num; j++) {
                unsigned i = 0, core = 0, count = 0;
                const uint64_t ways_mask = (1ULL << cap->num_ways) - 1ULL;

                ret = pqos_cpu_get_cores(cpu, sockets[j], 1, &count, &core);
                if (ret != PQOS_RETVAL_OK)
			goto cat_reset_exit;

                for (i = 0; i < cap->num_classes; i++) {
                        uint32_t reg = i + PQOS_MSR_L3CA_MASK_START;
                        uint64_t val = ways_mask;
                        int retval = MACHINE_RETVAL_OK;

                        retval = msr_write(core, reg, val);
                        if (retval != MACHINE_RETVAL_OK) {
                                ret = PQOS_RETVAL_ERROR;
				goto cat_reset_exit;
			}
                }
        }

        /**
         * Associate all cores with COS0
         */
        for (j = 0; j < cpu->num_cores; j++) {
                const unsigned class_id = 0;
                uint32_t reg = PQOS_MSR_ASSOC;
                uint64_t val = 0;
                int retval = MACHINE_RETVAL_OK;

                retval = msr_read(cpu->cores[j].lcore, reg, &val);
                if (retval != MACHINE_RETVAL_OK) {
			ret = PQOS_RETVAL_ERROR;
			goto cat_reset_exit;
		}

                val &= (~PQOS_MSR_ASSOC_QECOS_MASK);
                val |= (((uint64_t) class_id) << PQOS_MSR_ASSOC_QECOS_SHIFT);

                retval = msr_write(cpu->cores[j].lcore, reg, val);
                if (retval != MACHINE_RETVAL_OK) {
			ret = PQOS_RETVAL_ERROR;
			goto cat_reset_exit;
		}
        }

cat_reset_exit:
	free(sockets);
        return ret;
}

/**
 * @brief Checks CDP enable status across all CPU sockets
 *
 * It also validates if CDP enabling is consistent across
 * CPU sockets.
 * At the moment, such scenario is considered as error
 * that requires system reboot.
 *
 * @param cpu detected CPU topology
 * @param enabled place to store CDP enabling status
 *
 * @return Operations status
 * @retval PQOS_RETVAL_OK on success
 */
static int
cdp_is_enabled(const struct pqos_cpuinfo *cpu,
               int *enabled)
{
        unsigned *sockets = NULL;
        unsigned sockets_num = 0, j = 0;
        unsigned enabled_num = 0, disabled_num = 0;
        int ret = PQOS_RETVAL_OK;

        ASSERT(enabled != NULL && cpu != NULL);
        if (enabled == NULL || cpu == NULL)
                return PQOS_RETVAL_PARAM;

        *enabled = 0;

        /**
         * Get list of socket id's
         */
	ret = get_cpu_sockets(cpu, &sockets_num, &sockets);
        if (ret != PQOS_RETVAL_OK)
                return ret;

        for (j = 0; j < sockets_num; j++) {
                uint64_t reg = 0;
                unsigned core = 0, count = 0;

                ret = pqos_cpu_get_cores(cpu, sockets[j], 1, &count, &core);
                if (ret != PQOS_RETVAL_OK)
			goto cdp_is_enabled_exit;

                if (msr_read(core, PQOS_MSR_L3_QOS_CFG, &reg) !=
                    MACHINE_RETVAL_OK) {
			ret = PQOS_RETVAL_ERROR;
			goto cdp_is_enabled_exit;
		}

                if (reg & PQOS_MSR_L3_QOS_CFG_CDP_EN)
                        enabled_num++;
                else
                        disabled_num++;
        }

        if (disabled_num > 0 && enabled_num > 0) {
                LOG_ERROR("Inconsistent CDP settings across sockets."
                          "Please reboot your system!\n");
                ret = PQOS_RETVAL_ERROR;
		goto cdp_is_enabled_exit;
        }

        if (enabled_num > 0)
                *enabled = 1;

        LOG_INFO("CDP is %s\n",
                 (*enabled) ? "enabled" : "disabled");

cdp_is_enabled_exit:
	free(sockets);
        return ret;
}

/**
 * @brief Enables or disables CDP across all CPU sockets
 *
 * @param cpu detected CPU topology
 * @param enable CDP enable/disable flag, 1 - enable, 0 - disable
 *
 * @return Operations status
 * @retval PQOS_RETVAL_OK on success
 */
static int
cdp_enable(const struct pqos_cpuinfo *cpu,
           const int enable)
{
        unsigned *sockets = NULL;
        unsigned sockets_num = 0, j = 0;
        int ret = PQOS_RETVAL_OK;

        LOG_INFO("%s CDP across all sockets...\n",
                 (enable) ? "Enabling" : "Disabling");

        ASSERT(cpu != NULL);
        if (cpu == NULL)
                return PQOS_RETVAL_PARAM;

        /**
         * Get list of socket id's
         */
	ret = get_cpu_sockets(cpu, &sockets_num, &sockets);
        if (ret != PQOS_RETVAL_OK)
                return ret;

        for (j = 0; j < sockets_num; j++) {
                uint64_t reg = 0;
                unsigned core = 0, count = 0;

                ret = pqos_cpu_get_cores(cpu, sockets[j], 1, &count, &core);
                if (ret != PQOS_RETVAL_OK)
                        goto cdp_enable_exit;

                if (msr_read(core, PQOS_MSR_L3_QOS_CFG, &reg) !=
                    MACHINE_RETVAL_OK) {
                        ret = PQOS_RETVAL_ERROR;
			goto cdp_enable_exit;
		}

                if (enable)
                        reg |= PQOS_MSR_L3_QOS_CFG_CDP_EN;
                else
                        reg &= ~PQOS_MSR_L3_QOS_CFG_CDP_EN;

                if (msr_write(core, PQOS_MSR_L3_QOS_CFG, reg) !=
                    MACHINE_RETVAL_OK) {
                        ret = PQOS_RETVAL_ERROR;
			goto cdp_enable_exit;
		}
        }

cdp_enable_exit:
	free(sockets);
        return ret;
}

/**
 * @brief Detects presence of CAT based on brand string.
 *
 * If CPUID.0x7.0 doesn't report CAT feature
 * platform may still support it:
 * - we need to check brand string vs known ones
 * - use CPUID.0x4.0x3 to get number of cache ways
 *
 * @param cap CAT structure to be initialized
 * @param cdp_cfg CAT CDP configuration
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK success
 * @retval PQOS_RETVAL_RESOURCE technology not supported
 */
static int
discover_alloc_llc_brandstr(struct pqos_cap_l3ca *cap,
                            const enum pqos_cdp_config cdp_cfg)
{
#define CPUID_LEAF_BRAND_START 0x80000002UL
#define CPUID_LEAF_BRAND_END   0x80000004UL
#define CPUID_LEAF_BRAND_NUM   (CPUID_LEAF_BRAND_END-CPUID_LEAF_BRAND_START+1)
#define MAX_BRAND_STRING_LEN   (CPUID_LEAF_BRAND_NUM*4*sizeof(uint32_t))
       static const char * const supported_brands[] = {
                "E5-2658 v3",
                "E5-2648L v3", "E5-2628L v3",
                "E5-2618L v3", "E5-2608L v3",
                "E5-2658A v3", "E3-1258L v4",
                "E3-1278L v4"
        };
        struct cpuid_out res;
        int ret = PQOS_RETVAL_OK,
                match_found = 0;
        char brand_str[MAX_BRAND_STRING_LEN+1];
        uint32_t *brand_u32 = (uint32_t *)brand_str;
        unsigned i = 0;

        /**
         * Assume \a cap is not NULL at this stage.
         * Adequate check has to be done in the caller.
         */
        ASSERT(cap != NULL);

        switch (cdp_cfg) {
        case PQOS_REQUIRE_CDP_OFF:
        case PQOS_REQUIRE_CDP_ANY:
                break;
        case PQOS_REQUIRE_CDP_ON:
                LOG_ERROR("CDP requested but not supported by the platform!\n");
                return PQOS_RETVAL_PARAM;
        default:
                LOG_ERROR("Unrecognized CDP configuration setting %d!\n",
                          (int) cdp_cfg);
                return PQOS_RETVAL_PARAM;
        }

        ret = lcpuid(0x80000000, 0, &res);
        if (ret != MACHINE_RETVAL_OK) {
                LOG_ERROR("CPUID.0x80000000.0 error!\n");
                return PQOS_RETVAL_ERROR;
        }

        if (res.eax < CPUID_LEAF_BRAND_END) {
                LOG_ERROR("Brand string CPU-ID extended functions "
                           "not supported\n");
                return PQOS_RETVAL_ERROR;
        }

        memset(brand_str, 0, sizeof(brand_str));

        for (i = 0; i < CPUID_LEAF_BRAND_NUM; i++) {
                ret = lcpuid((unsigned)CPUID_LEAF_BRAND_START + i, 0, &res);
                if (ret != MACHINE_RETVAL_OK) {
                        LOG_ERROR("CPUID.0x%x.0 error!\n",
                                  (unsigned)CPUID_LEAF_BRAND_START + i);
                        return PQOS_RETVAL_ERROR;
                }
                *brand_u32++ = res.eax;
                *brand_u32++ = res.ebx;
                *brand_u32++ = res.ecx;
                *brand_u32++ = res.edx;
        }

        LOG_INFO("CPU brand string '%s'\n", brand_str);

        /**
         * match brand against supported ones
         */
        for (i = 0; i < DIM(supported_brands); i++)
                if (strstr(brand_str, supported_brands[i]) != NULL) {
                        LOG_INFO("Cache allocation detected for model name "
                                 "'%s'\n", brand_str);
                        match_found = 1;
                        break;
                }

        if (!match_found) {
		LOG_WARN("Cache allocation not supported on model name '%s'!\n",
                         brand_str);
                return PQOS_RETVAL_RESOURCE;
        }

        /**
         * Figure out number of ways and CBM (1:1)
         * using CPUID.0x4.0x3
         */
        cap->num_classes = 4;

        return ret;
}

/**
 * @brief Detects presence of CAT based on CPUID
 *
 * @param cap CAT structure to be initialized
 * @param cdp_cfg CAT CDP configuration
 * @param cpu CPU topology structure
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK success
 * @retval PQOS_RETVAL_RESOURCE technology not supported
 */
static int
discover_alloc_llc_cpuid(struct pqos_cap_l3ca *cap,
                         const enum pqos_cdp_config cdp_cfg,
                         const struct pqos_cpuinfo *cpu)
{
        struct cpuid_out res;
        uint32_t res_id = 0;
        int detected = 0;
        int ret = PQOS_RETVAL_OK;

        /**
         * We can go to CPUID.0x10.0 to explore
         * allocation capabilities
         */
        ret = lcpuid(0x10, 0x0, &res);
        if (ret != MACHINE_RETVAL_OK) {
                LOG_ERROR("CPUID 0x10.0 error!\n");
                return PQOS_RETVAL_ERROR;
        }

        res_id = res.ebx;

        if (res_id & (1 << PQOS_RES_ID_L3_ALLOCATION)) {
                /**
                 * L3 CAT detected
                 * - get more info about it
                 */
                ret = lcpuid(0x10, PQOS_RES_ID_L3_ALLOCATION, &res);
                if (ret != MACHINE_RETVAL_OK) {
                        LOG_ERROR("CPUID 0x10.%d error!\n",
                                  PQOS_RES_ID_L3_ALLOCATION);
                        return PQOS_RETVAL_ERROR;
                }

                cap->num_classes = res.edx+1;
                cap->num_ways = res.eax+1;
                cap->cdp = (res.ecx >> PQOS_CPUID_CAT_CDP_BIT) & 1;
                cap->cdp_on = 0;
                cap->way_contention = (uint64_t) res.ebx;

                if (cap->cdp) {
                        /**
                         * CDP is supported but is it on?
                         */
                        int cdp_on = 0;

                        ret = cdp_is_enabled(cpu, &cdp_on);
                        if (ret != PQOS_RETVAL_OK) {
                                LOG_ERROR("CDP detection error!\n");
                                return ret;
                        }
                        cap->cdp_on = cdp_on;
                }

                switch (cdp_cfg) {
                case PQOS_REQUIRE_CDP_ON:
                        if (!cap->cdp) {
                                LOG_ERROR("CAT/CDP requested but not supported "
                                          "by the platform!\n");
                                return PQOS_RETVAL_ERROR;
                        } else if (!cap->cdp_on) {
                                /**
                                 * Turn on CDP
                                 */
                                LOG_INFO("Turning CDP ON ...\n");
                                ret = cat_reset(cap, cpu);
                                if (ret != PQOS_RETVAL_OK) {
                                        LOG_ERROR("CAT reset error!\n");
                                        return ret;
                                }
                                ret = cdp_enable(cpu, 1);
                                if (ret != PQOS_RETVAL_OK) {
                                        LOG_ERROR("CDP enable error!\n");
                                        return ret;
                                }
                                cap->cdp_on = 1;
                        }
                        break;
                case PQOS_REQUIRE_CDP_OFF:
                        if (cap->cdp && cap->cdp_on) {
                                /**
                                 * Turn off CDP
                                 */
                                LOG_INFO("Turning CDP OFF ...\n");
                                ret = cat_reset(cap, cpu);
                                if (ret != PQOS_RETVAL_OK) {
                                        LOG_ERROR("CAT reset error!\n");
                                        return ret;
                                }
                                ret = cdp_enable(cpu, 0);
                                if (ret != PQOS_RETVAL_OK) {
                                        LOG_ERROR("CDP disable error!\n");
                                        return ret;
                                }
                                cap->cdp_on = 0;
                        }
                        break;
                case PQOS_REQUIRE_CDP_ANY:
                        break;
                default:
                        LOG_ERROR("Unrecognized CDP configuration %d!\n",
                                  cdp_cfg);
                        return PQOS_RETVAL_PARAM;
                        break;
                }

                if (cap->cdp_on) {
                        /**
                         * Divide number of classes by 2.
                         * This is because CDP needs 2 bit-masks per one class.
                         */
                        cap->num_classes = cap->num_classes / 2;
                }

                detected = 1;
                res_id &= ~(1 << PQOS_RES_ID_L3_ALLOCATION);
        }

        if (res_id != 0) {
                LOG_INFO("Unsupported resource ID's detected: "
                         "CPUID(0x10,0).ebx = 0x%x\n",
                         res_id);
        }

        if (!detected) {
                LOG_INFO("No resource allocation capabilities detected.\n");
                ret = PQOS_RETVAL_RESOURCE;
        }

        return ret;
}

/**
 * @brief Discovers CAT
 *
 * First it tries to detects CAT through CPUID.0x7.0
 * if this fails then falls into brand string check.
 *
 * Function allocates memory for CAT capabilities
 * and returns it to the caller through \a r_cap.
 *
 * \a config and \a cpu are only needed because of CDP.
 * Namely, this may be required to reset CAT on all sockets
 * in order to turn on/off CDP. For this operation
 * CPU topology and library config context are needed.
 *
 * @param r_cap place to store CAT capabilities structure
 * @param config library config passed from an application
 * @param cpu detected cpu topology
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK success
 */
static int
discover_alloc_llc(struct pqos_cap_l3ca **r_cap,
                   const struct pqos_config *config,
                   const struct pqos_cpuinfo *cpu)
{
        struct cpuid_out res;
        struct pqos_cap_l3ca *cap = NULL;
        const unsigned sz = sizeof(*cap);
        int ret = PQOS_RETVAL_OK;

        cap = (struct pqos_cap_l3ca *)malloc(sz);
        if (cap == NULL)
                return PQOS_RETVAL_RESOURCE;

        ASSERT(cap != NULL);

        memset(cap, 0, sz);
        cap->mem_size = sz;

        /**
         * Run CPUID.0x7.0 to check
         * for allocation capability (bit 15 of ebx)
         */
        ret = lcpuid(0x7, 0x0, &res);
        if (ret != MACHINE_RETVAL_OK) {
                free(cap);
                return PQOS_RETVAL_ERROR;
        }

        if (res.ebx & (1 << 15)) {
                /**
                 * Use CPUID method
                 */
                LOG_INFO("CPUID.0x7.0: CAT supported\n");
                ret = discover_alloc_llc_cpuid(cap, config->cdp_cfg, cpu);
        } else {
                /**
                 * Use brand string matching method
                 */
                LOG_INFO("CPUID.0x7.0: CAT not supported. "
			 "Check brand string.\n");
                ret = discover_alloc_llc_brandstr(cap, config->cdp_cfg);
        }

        if (ret == PQOS_RETVAL_OK) {
                LOG_INFO("CAT details: CDP support=%d, CDP on=%d, "
                         "#COS=%u, #ways=%u, ways contention bit-mask 0x%x\n",
                         cap->cdp, cap->cdp_on, cap->num_classes,
                         cap->num_ways, cap->way_contention);
                /**
                 * Detect number of LLC ways and LLC size
                 * Calculate byte size of one cache way
                 */
                ret = get_l3_cache_info(&cap->num_ways, &cap->way_size);
                if (ret == PQOS_RETVAL_OK) {
                        LOG_INFO("LLC cache size %u bytes, %u ways\n",
                                 cap->way_size, cap->num_ways);
                        ASSERT(cap->num_ways > 0);
                        if (cap->num_ways > 0)
                                cap->way_size = cap->way_size / cap->num_ways;
                        LOG_INFO("LLC cache way size %u bytes\n",
                                 cap->way_size);
                }
        }

        if (ret == PQOS_RETVAL_OK)
                (*r_cap) = cap;
        else
                free(cap);

        return ret;
}

/**
 * @brief Runs detection of platform monitoring and allocation capabilities
 *
 * @param p_cap place to store allocated capabilities structure
 * @param config library configuration passed on by an application
 * @param cpu detected cpu topology
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK success
 */
static int
discover_capabilities(struct pqos_cap **p_cap,
                      const struct pqos_config *config,
                      const struct pqos_cpuinfo *cpu)
{
        struct pqos_cap_mon *det_mon = NULL;
        struct pqos_cap_l3ca *det_l3ca = NULL;
        struct pqos_cap *_cap = NULL;
        struct pqos_capability *item = NULL;
        unsigned sz = 0;
        int ret = PQOS_RETVAL_OK;

        /**
         * Monitoring init
         */
        ret = discover_monitoring(&det_mon);
        if (ret != PQOS_RETVAL_OK) {
                LOG_INFO("Monitoring capability not detected\n");
        } else {
                LOG_INFO("Monitoring capability detected\n");
                sz += sizeof(struct pqos_capability);
        }

        /**
         * Cache allocation init
         */
        ret = discover_alloc_llc(&det_l3ca, config, cpu);
        switch (ret) {
        case PQOS_RETVAL_OK:
                LOG_INFO("L3CA capability detected\n");
                sz += sizeof(struct pqos_capability);
                break;
        case PQOS_RETVAL_RESOURCE:
                LOG_INFO("L3CA capability not detected\n");
                break;
        default:
                LOG_ERROR("Fatal error encounter in CAT discovery!\n");
                ret = PQOS_RETVAL_ERROR;
                goto error_exit;
        }

        if (sz == 0) {
                LOG_ERROR("No Platform QoS capability discovered\n");
                ret = PQOS_RETVAL_ERROR;
                goto error_exit;
        }

        sz += sizeof(struct pqos_cap);
        _cap = (struct pqos_cap *)malloc(sz);
        if (_cap == NULL) {
                LOG_ERROR("Allocation error in %s()\n", __func__);
                ret = PQOS_RETVAL_ERROR;
                goto error_exit;
        }

        memset(_cap, 0, sz);
        _cap->mem_size = sz;
        _cap->version = PQOS_VERSION;
        item = &_cap->capabilities[0];

        if (det_mon != NULL) {
                _cap->num_cap++;
                item->type = PQOS_CAP_TYPE_MON;
                item->u.mon = det_mon;
                item++;
                ret = PQOS_RETVAL_OK;
        }

        if (det_l3ca != NULL) {
                _cap->num_cap++;
                item->type = PQOS_CAP_TYPE_L3CA;
                item->u.l3ca = det_l3ca;
                item++;
                ret = PQOS_RETVAL_OK;
        }

        (*p_cap) = _cap;

 error_exit:
        if (ret != PQOS_RETVAL_OK) {
                if (det_mon != NULL)
                        free(det_mon);
                if (det_l3ca != NULL)
                        free(det_l3ca);
        }

        return ret;
}

/**
 * @brief Calculates byte size of pqos_cpuinfo to accommodate \a num_cores
 *
 * @param num_cores number of cores
 *
 * @return Size of pqos_coreinfo structure in bytes
 */
static unsigned
pqos_cpuinfo_get_memsize(const unsigned num_cores)
{
        return (num_cores * sizeof(struct pqos_coreinfo)) +
                sizeof(struct pqos_cpuinfo);
}

/**
 * =======================================
 * =======================================
 *
 * initialize and shutdown
 *
 * =======================================
 * =======================================
 */
int
pqos_init(const struct pqos_config *config)
{
        int ret = PQOS_RETVAL_OK;
        unsigned i = 0, max_core = 0;
        int cat_init = 0, mon_init = 0;

        if (config == NULL)
                return PQOS_RETVAL_PARAM;

        _pqos_api_lock();

        ret = _pqos_check_init(0);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        ret = log_init(config->fd_log,
                       config->verbose ? LOG_OPT_VERBOSE : LOG_OPT_DEFAULT);
        if (ret != PQOS_RETVAL_OK) {
                LOG_ERROR("log_init() error %d\n", ret);
                goto init_error;
        }

        if (config->topology == NULL) {
                /**
                 * Topology not provided through config.
                 * CPU discovery done through internal mechanism.
                 */
                const struct cpuinfo_topology *topology = NULL;
                unsigned n, ms;

                if (cpuinfo_init(&topology) != CPUINFO_RETVAL_OK) {
                        LOG_ERROR("cpuinfo_init() error %d\n", ret);
                        ret = PQOS_RETVAL_ERROR;
                        goto log_init_error;
                }
                ASSERT(topology != NULL);
                ms = pqos_cpuinfo_get_memsize(topology->num_cores);
                m_cpu = (struct pqos_cpuinfo *)malloc(ms);
                if (m_cpu == NULL) {
                        LOG_ERROR("Memory allocation error\n");
                        ret = PQOS_RETVAL_ERROR;
                        goto log_init_error;
                }
                m_cpu->mem_size = ms;
                m_cpu->num_cores = topology->num_cores;
                for (n = 0; n < topology->num_cores; n++) {
                        m_cpu->cores[n].lcore = topology->cores[n].lcore;
                        m_cpu->cores[n].socket = topology->cores[n].socket;
                        m_cpu->cores[n].cluster = topology->cores[n].cluster;
                }
        } else {
                /**
                 * App provides CPU topology.
                 */
                unsigned n, ms;

                if (config->topology->num_cores == 0) {
                        LOG_ERROR("Provided CPU topology is empty!\n");
                        ret = PQOS_RETVAL_PARAM;
                        goto log_init_error;
                }

                ms = pqos_cpuinfo_get_memsize(config->topology->num_cores);
                m_cpu = (struct pqos_cpuinfo *)malloc(ms);
                if (m_cpu == NULL) {
                        LOG_ERROR("Memory allocation error\n");
                        ret = PQOS_RETVAL_ERROR;
                        goto log_init_error;
                }
                m_cpu->mem_size = ms;
                m_cpu->num_cores = config->topology->num_cores;
                for (n = 0; n < config->topology->num_cores; n++) {
                        m_cpu->cores[n].lcore =
                                config->topology->cores[n].lcore;
                        m_cpu->cores[n].socket =
                                config->topology->cores[n].socket;
                        m_cpu->cores[n].cluster =
                                config->topology->cores[n].cluster;
                }
        }
        ASSERT(m_cpu != NULL);

        /**
         * Find max core id in the topology
         */
        for (i = 0; i < m_cpu->num_cores; i++)
                if (m_cpu->cores[i].lcore > max_core)
                        max_core = m_cpu->cores[i].lcore;

        ret = machine_init(max_core);
        if (ret != PQOS_RETVAL_OK) {
                LOG_ERROR("machine_init() error %d\n", ret);
                goto cpuinfo_init_error;
        }

        ret = discover_capabilities(&m_cap, config, m_cpu);
        if (ret != PQOS_RETVAL_OK) {
                LOG_ERROR("discover_capabilities() error %d\n", ret);
                goto machine_init_error;
        }
        ASSERT(m_cap != NULL);

        /**
         * If monitoring capability has been discovered
         * then get max RMID supported by a CPU socket
         * and allocate memory for RMID table
         */
        ret = pqos_mon_init(m_cpu, m_cap, config);
        if (ret != PQOS_RETVAL_OK) {
                LOG_ERROR("monitoring init error %d\n", ret);
        } else {
                LOG_INFO("monitoring init OK\n");
                mon_init = 1;
        }

        ret = pqos_alloc_init(m_cpu, m_cap, config);
        if (ret != PQOS_RETVAL_OK) {
                LOG_ERROR("allocation init error %d\n", ret);
        } else {
                LOG_INFO("allocation init OK\n");
                cat_init = 1;
        }

        if (cat_init == 0 && mon_init == 0) {
                LOG_ERROR("None of detected capabilities could be "
                          "initialized!\n");
                ret = PQOS_RETVAL_ERROR;
        }

 machine_init_error:
        if (ret != PQOS_RETVAL_OK)
                (void) machine_fini();
 cpuinfo_init_error:
        if (ret != PQOS_RETVAL_OK)
                (void) cpuinfo_fini();
 log_init_error:
        if (ret != PQOS_RETVAL_OK)
                (void) log_fini();
 init_error:
        if (ret != PQOS_RETVAL_OK) {
                if (m_cpu != NULL)
                        free(m_cpu);
                if (m_cap != NULL)
                        free(m_cap);
                m_cpu = NULL;
                m_cap = NULL;
        }

        if (ret == PQOS_RETVAL_OK)
                m_init_done = 1;

        _pqos_api_unlock();

        return ret;
}

int
pqos_fini(void)
{
        int ret = PQOS_RETVAL_OK;
        int retval = PQOS_RETVAL_OK;
        unsigned i = 0;

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        pqos_mon_fini();
        pqos_alloc_fini();

        ret = cpuinfo_fini();
        if (ret != CPUINFO_RETVAL_OK) {
                retval = PQOS_RETVAL_ERROR;
                LOG_ERROR("cpuinfo_fini() error %d\n", ret);
        }

        ret = machine_fini();
        if (ret != PQOS_RETVAL_OK) {
                retval = ret;
                LOG_ERROR("machine_fini() error %d\n", ret);
        }

        ret = log_fini();
        if (ret != PQOS_RETVAL_OK)
                retval = ret;

        free((void *)m_cpu);
        m_cpu = NULL;

        for (i = 0; i < m_cap->num_cap; i++)
                free(m_cap->capabilities[i].u.generic_ptr);
        free((void *)m_cap);
        m_cap = NULL;

        m_init_done = 0;

        _pqos_api_unlock();
        return retval;
}

/**
 * =======================================
 * =======================================
 *
 * capabilities
 *
 * =======================================
 * =======================================
 */

int
pqos_cap_get(const struct pqos_cap **cap,
             const struct pqos_cpuinfo **cpu)
{
        int ret = PQOS_RETVAL_OK;

        if (cap == NULL && cpu == NULL)
                return PQOS_RETVAL_PARAM;

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        if (cap != NULL) {
                ASSERT(m_cap != NULL);
                *cap = m_cap;
        }

        if (cpu != NULL) {
                ASSERT(m_cpu != NULL);
                *cpu = m_cpu;
        }

        _pqos_api_unlock();
        return PQOS_RETVAL_OK;
}
