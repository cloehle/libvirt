/*
 * virJailhouseDriver.c: hypervisor driver for managing Jailhouse cells
 *
 * Copyright (C) 2015 Linutronix GmbH
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Christian Loehle
 */

#include <config.h>
#include <string.h>
#include "jailhouse_driver.h"
#include "datatypes.h"
#include "virerror.h"
#include "viralloc.h"
#include "virlog.h"
#include "vircommand.h"
#include "virxml.h"
#include "configmake.h"
#include "virfile.h"
#include "virtypedparam.h"
#include "virstring.h"
#include "nodeinfo.h"
#include "capabilities.h"
#include "domain_conf.h"

#define VIR_FROM_THIS VIR_FROM_JAILHOUSE

#define IDLENGTH 8
#define NAMELENGTH 24
#define STATELENGTH 16
#define CPULENGTH 24
#define STATERUNNING 0
#define STATERUNNINGSTRING          "running         "
#define STATERUNNINGLOCKED 1
#define STATERUNNINGLOCKEDSTRING    "running/locked  "
#define STATESHUTDOWN 2
#define STATESHUTDOWNSTRING         "shut down       "
#define STATEFAILED 3
#define STATEFAILEDSTRING           "failed          "
#define JAILHOUSEVERSIONOUTPUT      "Jailhouse management tool"

/*
 *  CPUs are currently unused but this might change
 */
struct virJailhouseCell {
    int id;
    char name[NAMELENGTH+1];
    int state;
    int *assignedCPUs;
    int assignedCPUsLength;
    int *failedCPUs;
    int failedCPUsLength;
    unsigned char uuid[VIR_UUID_BUFLEN];
};
typedef struct virJailhouseCell virJailhouseCell;
typedef virJailhouseCell *virJailhouseCellPtr;

/*
 *  The driver requeries the cells on most calls, it stores the result of the
 *  last query, so it can copy the UUIDs in the new query if the cell is the
 *  same(otherwise it just generates a new one).
 *  not preserving the UUID results in a lot of bugs in libvirts clients.
 */
struct virJailhouseDriver {
    char *binary;
    size_t lastQueryCellsCount;
    virJailhouseCellPtr lastQueryCells;
};
typedef struct virJailhouseDriver virJailhouseDriver;
typedef virJailhouseDriver *virJailhouseDriverPtr;

/*
 *  Takes a string in the format of "jailhouse cell list" as input,
 *  allocates an int array in which every CPU is explicitly listed and saves a pointer in cpusptr
 */
static size_t
virJailhouseParseCPUs(const char* output, int **cpusptr)
{
    const char *current = output;
    char *endptr;
    size_t count = 0;
    int number;
    int nextNumber;
    int* cpus;
    size_t i = 0;
    if (*current == ' ') { // no CPUs assigned/failed, not an error
        *cpusptr = NULL;
        return 0;
    }
    while (current <= output+CPULENGTH && *current != ' ') {
        if (virStrToLong_i(current, &endptr, 0, &number))
            goto error;
        current = endptr;
        count++;
        if (*current == '-') {
            current++;
            if (virStrToLong_i(current, &endptr, 0, &nextNumber))
                goto error;
            count += nextNumber - number;
            current = endptr;
        }
        current++;
    }
    if (VIR_ALLOC_N(cpus, count) < 0)
        goto error;
    current = output;
    while (i < count) {
        if (virStrToLong_i(current, &endptr, 0, &number))
            goto error;
        current = endptr;
        cpus[i++] = number;
        if (*current == '-') {
            current++;
            if (virStrToLong_i(current, &endptr, 0, &nextNumber))
                goto error;
            current = endptr;
            for (; number < nextNumber; number++)
                cpus[i++] = number+1;
        }
        current++;
    }
    *cpusptr = cpus;
    return count;
    error:
    VIR_FREE(cpus);
    *cpusptr = NULL;
    return 0;
}

/*
 *  calls "jailhouse cell list" and parses the output in an array of virJailhouseCell
 *  example output:
 *  ID      Name                    State           Assigned CPUs           Failed CPUs
 *  0       QEMU-VM                 running         0-3
 */
static size_t
virJailhouseParseListOutput(virConnectPtr conn, virJailhouseCellPtr *parsedOutput)
{
    char *output;
    size_t count = -1; //  Don't count table header line
    size_t i = 0;
    size_t j;
    size_t k;
    char c;
    virCommandPtr cmd = virCommandNew(((virJailhouseDriverPtr)conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "list");
    virCommandAddEnvPassCommon(cmd);
    virCommandSetOutputBuffer(cmd, &output);
    if (virCommandRun(cmd, NULL) < 0)
        goto error;
    while (output[i] != '\0') {
        if (output[i] == '\n') count++;
        i++;
    }
    if (VIR_ALLOC_N(*parsedOutput, count) < 0)
        goto error;
    i = 0;
    while (output[i++] != '\n'); //  Skip table header line
    for (j = 0; j < count; j++) {
        for (k = 0; k <= IDLENGTH; k++) // char after number needs to be NUL for virStrToLong
            if (output[i+k] == ' ') {
                output[i+k] = '\0';
                break;
            }
        c = output[i+IDLENGTH];
        output[i+IDLENGTH] = '\0'; //   in case ID is 8 chars long, so beginning of name won't get parsed
        if (virStrToLong_i(output+i, NULL, 0, &(*parsedOutput)[j].id))
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Failed to parse id to long: %s"), output+i);
        output[i+IDLENGTH] = c;
        i += IDLENGTH;
        if (virStrncpy((*parsedOutput)[j].name, output+i, NAMELENGTH, NAMELENGTH+1) == NULL)
            // should never happen
            goto error;
        (*parsedOutput)[j].name[NAMELENGTH] = '\0';
        for (k = 0; k < NAMELENGTH; k++)
            if ((*parsedOutput)[j].name[k] == ' ')
                    break;
        (*parsedOutput)[j].name[k] = '\0';
        i += NAMELENGTH;
        if (STREQLEN(output+i, STATERUNNINGSTRING, STATELENGTH)) (*parsedOutput)[j].state = STATERUNNING;
        else if (STREQLEN(output+i, STATESHUTDOWNSTRING, STATELENGTH)) (*parsedOutput)[j].state = STATESHUTDOWN;
        else if (STREQLEN(output+i, STATEFAILEDSTRING, STATELENGTH)) (*parsedOutput)[j].state = STATEFAILED;
        else if (STREQLEN(output+i, STATERUNNINGLOCKEDSTRING, STATELENGTH)) (*parsedOutput)[j].state = STATERUNNINGLOCKED;
        i += STATELENGTH;
        (*parsedOutput)[j].assignedCPUsLength = virJailhouseParseCPUs(output+i, &((*parsedOutput)[j].assignedCPUs));
        i += CPULENGTH;
        (*parsedOutput)[j].failedCPUsLength = virJailhouseParseCPUs(output+i, &((*parsedOutput)[j].failedCPUs));
        i += CPULENGTH;
        i++; // skip \n
    }
    VIR_FREE(output);
    virCommandFree(cmd);
    return count;
    error:
    for (i = 0; i < count; i++) {
        VIR_FREE((*parsedOutput)[i].assignedCPUs);
        VIR_FREE((*parsedOutput)[i].failedCPUs);
    }
    VIR_FREE(*parsedOutput);
    *parsedOutput = NULL;
    VIR_FREE(output);
    output = NULL;
    virCommandFree(cmd);
    return -1;
}

/*
 *  Returns the libvirts equivalent of the cell state passed to it
 */
static virDomainState
virJailhouseCellToState(virJailhouseCellPtr cell)
{
    switch (cell->state) {
        case STATERUNNING: return VIR_DOMAIN_RUNNING;
        case STATERUNNINGLOCKED: return VIR_DOMAIN_RUNNING;
        case STATESHUTDOWN: return VIR_DOMAIN_SHUTOFF;
        case STATEFAILED: return VIR_DOMAIN_CRASHED;
        default: return VIR_DOMAIN_NOSTATE;
    }
}

/*
 *  Returns a new virDomainPtr filled with the data of the virJailhouseCell
 */
static virDomainPtr
virJailhouseCellToDomainPtr(virConnectPtr conn,  virJailhouseCellPtr cell)
{
    virDomainPtr dom = virGetDomain(conn, cell->name, cell->uuid);
    dom->id = cell->id;
    return dom;
}

/*
 *  Check cells for cell and copies UUID if found, otherwise generates a new one, this is to preserve UUID in libvirt
 */
static void virJailhouseSetUUID(virJailhouseCellPtr cells, size_t count, virJailhouseCellPtr cell)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (strncmp(cells[i].name, cell->name, NAMELENGTH+1))
            continue;
        memcpy(cell->uuid, cells[i].uuid, VIR_UUID_BUFLEN);
        return;
    }
    virUUIDGenerate(cell->uuid);
}

/*
 *  Frees the old list of cells, gets the new one and preserves UUID if cells were present in the old
 */
static void
virJailhouseGetCurrentCellList(virConnectPtr conn)
{
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    size_t count;
    size_t i;
    size_t lastCount = driver->lastQueryCellsCount;
    virJailhouseCellPtr lastCells = driver->lastQueryCells;
    virJailhouseCellPtr cells = NULL;
    count = virJailhouseParseListOutput(conn, &cells);
    for (i = 0; i < count; i++)
        virJailhouseSetUUID(lastCells, lastCount, cells+i);
    for (i = 0; i < lastCount; i++) {
        VIR_FREE(lastCells[i].assignedCPUs);
        VIR_FREE(lastCells[i].failedCPUs);
    }
    VIR_FREE(lastCells);
    driver->lastQueryCells = cells;
    driver->lastQueryCellsCount = count;
}

/*
 *  Converts libvirts virDomainPtr to the internal virJailhouseCell by parsing the "jailhouse cell list" output
 *  and looking up the name of the virDomainPtr, returns NULL if cell is no longer present
 */
static virJailhouseCellPtr
virDomainPtrToCell(virDomainPtr dom)
{
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)dom->conn->privateData;
    size_t cellsCount;
    size_t i;
    virJailhouseGetCurrentCellList(dom->conn);
    cellsCount = driver->lastQueryCellsCount;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    for (i = 0; i < cellsCount; i++)
        if (dom->id == cells[i].id)
                return cells+i;
    return NULL;
}

static virDrvOpenStatus
jailhouseConnectOpen(virConnectPtr conn, virConnectAuthPtr auth ATTRIBUTE_UNUSED, unsigned int flags)
{
    virCheckFlags(0, VIR_DRV_OPEN_ERROR);
    char* binary;
    char *output;
    if (conn->uri->scheme == NULL ||
            STRNEQ(conn->uri->scheme, "jailhouse"))
            return VIR_DRV_OPEN_DECLINED;
    if (conn->uri->path == NULL) {
        if (VIR_STRDUP(binary, "jailhouse") != 1)
            return VIR_DRV_OPEN_ERROR;
    } else {
        if (!virFileIsExecutable(conn->uri->path)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Path '%s', is not a valid executable file."),
                           conn->uri->path);
            return VIR_DRV_OPEN_ERROR;
        }
        if (VIR_STRDUP(binary, conn->uri->path) != 1)
            return VIR_DRV_OPEN_ERROR;
    }
    virCommandPtr cmd = virCommandNew(binary);
    virCommandAddArg(cmd, "--version");
    virCommandAddEnvPassCommon(cmd);
    virCommandSetOutputBuffer(cmd, &output);
    if (virCommandRun(cmd, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Executing '%s --version' failed."),
                       conn->uri->path);
        goto error;
    }
    if (STRNEQLEN(JAILHOUSEVERSIONOUTPUT, output, strlen(JAILHOUSEVERSIONOUTPUT))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("%s doesn't seem to be a correct Jailhouse binary."),
                       conn->uri->path);
        goto error;
    }
    VIR_FREE(output);
    virCommandFree(cmd);
    virJailhouseDriverPtr driver;
    if (VIR_ALLOC(driver) < 0)
        return VIR_DRV_OPEN_ERROR;
    driver->binary = binary;
    driver->lastQueryCells = NULL;
    driver->lastQueryCellsCount = 0;
    conn->privateData = driver;
    return VIR_DRV_OPEN_SUCCESS;
    error:
    VIR_FREE(output);
    virCommandFree(cmd);
    return VIR_DRV_OPEN_ERROR;
}

static int
jailhouseConnectClose(virConnectPtr conn)
{

    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    size_t i;
    size_t cellsCount = driver->lastQueryCellsCount;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    for (i = 0; i < cellsCount; i++) {
        VIR_FREE(cells[i].assignedCPUs);
        VIR_FREE(cells[i].failedCPUs);
    }
    VIR_FREE(cells);
    VIR_FREE(driver->binary);
    VIR_FREE(driver);
    conn->privateData = NULL;
    return 0;
}

static int
jailhouseConnectNumOfDomains(virConnectPtr conn)
{
    virJailhouseGetCurrentCellList(conn);
    return ((virJailhouseDriverPtr)conn->privateData)->lastQueryCellsCount;
}

static int
jailhouseConnectListDomains(virConnectPtr conn, int * ids, int maxids)
{
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    size_t cellsCount;
    size_t i;
    virJailhouseGetCurrentCellList(conn);
    cellsCount = driver->lastQueryCellsCount;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    for (i = 0; i < maxids && i < cellsCount; i++)
        ids[i] = cells[i].id;
    return i;
}

static int
jailhouseConnectListAllDomains(virConnectPtr conn, virDomainPtr ** domains, unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_ACTIVE, 0);
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    size_t cellsCount;
    size_t i;
    virJailhouseGetCurrentCellList(conn);
    cellsCount = driver->lastQueryCellsCount;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    if (cellsCount == -1)
        goto error;
    if (VIR_ALLOC_N(*domains, cellsCount+1) < 0)
        goto error;
    for (i = 0; i < cellsCount; i++)
        (*domains)[i] = virJailhouseCellToDomainPtr(conn, cells+i);
    (*domains)[cellsCount] = NULL;
    return cellsCount;
    error:
    *domains = NULL;
    return -1;
}

static virDomainPtr
jailhouseDomainLookupByID(virConnectPtr conn, int id)
{
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    size_t cellsCount;
    size_t i;
    virJailhouseGetCurrentCellList(conn);
    cellsCount = driver->lastQueryCellsCount;
    if (cellsCount == -1)
        return NULL;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    for (i = 0; i < cellsCount; i++)
        if (cells[i].id == id)
            return virJailhouseCellToDomainPtr(conn, cells+i);
    virReportError(VIR_ERR_NO_DOMAIN, NULL);
    return NULL;
}

static virDomainPtr
jailhouseDomainLookupByName(virConnectPtr conn, const char *lookupName)
{
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    size_t cellsCount;
    size_t i;
    virJailhouseGetCurrentCellList(conn);
    cellsCount = driver->lastQueryCellsCount;
    if (cellsCount == -1)
        return NULL;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    for (i = 0; i < cellsCount; i++)
        if (STREQ(cells[i].name, lookupName))
            return virJailhouseCellToDomainPtr(conn, cells+i);
    virReportError(VIR_ERR_NO_DOMAIN, NULL);
    return NULL;
}

static virDomainPtr
jailhouseDomainLookupByUUID(virConnectPtr conn, const unsigned char * uuid)
{
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    size_t cellsCount;
    size_t i;
    virJailhouseGetCurrentCellList(conn);
    cellsCount = driver->lastQueryCellsCount;
    if (cellsCount == -1)
        return NULL;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    for (i = 0; i < cellsCount; i++)
        if (memcmp(cells[i].uuid, (const char*)uuid, VIR_UUID_BUFLEN) == 0)
            return virJailhouseCellToDomainPtr(conn, cells+i);
    virReportError(VIR_ERR_NO_DOMAIN, NULL);
    return NULL;
}

/*
 *  There currently is no straightforward way for the driver to retrieve those,
 *  so maxMem, memory and cpuTime have dummy values
 */
static int
jailhouseDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
    virJailhouseCellPtr cell = virDomainPtrToCell(domain);
    if (cell == NULL)
        return -1;
    info->state = virJailhouseCellToState(cell);
    info->maxMem = 0;
    info->memory = 0;
    info->nrVirtCpu = cell->assignedCPUsLength;
    info->cpuTime = 0;
    return 0;
}

static int
jailhouseDomainGetState(virDomainPtr domain, int *state,
                        int *reason ATTRIBUTE_UNUSED, unsigned int flags)
{
    virCheckFlags(0, 0);
    virJailhouseCellPtr cell = virDomainPtrToCell(domain);
    if (cell == NULL)
        return -1;
    *state = virJailhouseCellToState(cell);
    return 0;
}

static int
jailhouseDomainShutdown(virDomainPtr domain)
{
    int resultcode;
    virCommandPtr cmd = virCommandNew(((virJailhouseDriverPtr)domain->conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "shutdown");
    virCommandAddArgFormat(cmd, "%d", domain->id);
    virCommandAddEnvPassCommon(cmd);
    resultcode = virCommandRun(cmd, NULL);
    virCommandFree(cmd);
    if (resultcode < 0)
        return -1;
    return 0;
}

/*
 *  CAREFUL, this is the Jailhouse destroy, not the libvirt destroy, cell will be deleted and would need to be created and loaded again.
 *  This is implemented anyway, so libvirt clients have an option to use jailhouse destroy too.
 */
static int
jailhouseDomainDestroy(virDomainPtr domain)
{
    int resultcode;
    virCommandPtr cmd = virCommandNew(((virJailhouseDriverPtr)domain->conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "destroy");
    virCommandAddArgFormat(cmd, "%d", domain->id);
    virCommandAddEnvPassCommon(cmd);
    resultcode = virCommandRun(cmd, NULL);
    virCommandFree(cmd);
    if (resultcode < 0)
        return -1;
    return 0;
}

static int
jailhouseDomainCreate(virDomainPtr domain)
{
    int resultcode;
    virCommandPtr cmd = virCommandNew(((virJailhouseDriverPtr)domain->conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "start");
    virCommandAddArgFormat(cmd, "%d", domain->id);
    virCommandAddEnvPassCommon(cmd);
    resultcode = virCommandRun(cmd, NULL);
    virCommandFree(cmd);
    if (resultcode < 0)
        return -1;
    return 0;
}

/*
 * There currently is no reason why it shouldn't be
 */
static int
jailhouseConnectIsAlive(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 1;
}

static int
jailhouseNodeGetInfo(virConnectPtr conn ATTRIBUTE_UNUSED, virNodeInfoPtr info)
{
    return nodeGetInfo(NULL, info);
}

/*
 *  Returns a dummy capabilities XML for virt-manager
 */
static char *
jailhouseConnectGetCapabilities(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    virCapsPtr caps = virCapabilitiesNew(VIR_ARCH_NONE, false, false);
    char* xml = virCapabilitiesFormatXML(caps);
    virObjectUnref(caps);
    return xml;
}

static char *
jailhouseDomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    virCheckFlags(0, NULL);
    char* xml;
    virDomainDefPtr domainDef = virDomainDefNewFull(domain->name, domain->uuid, domain->id);
    xml = virDomainDefFormat(domainDef, 0);
    virObjectUnref(domainDef);
    return xml;
}

static virHypervisorDriver jailhouseHypervisorDriver = {
    .name = "jailhouse",
    .connectOpen = jailhouseConnectOpen, /* 1.2.22 */
    .connectClose = jailhouseConnectClose, /* 1.2.22 */
    .connectGetCapabilities = jailhouseConnectGetCapabilities, /* 1.2.22 */
    .connectNumOfDomains = jailhouseConnectNumOfDomains, /* 1.2.22 */
    .connectListDomains = jailhouseConnectListDomains, /* 1.2.22 */
    .connectIsAlive = jailhouseConnectIsAlive, /* 1.2.22 */
    .connectListAllDomains = jailhouseConnectListAllDomains, /* 1.2.22 */
    .domainLookupByID = jailhouseDomainLookupByID, /* 1.2.22 */
    .domainLookupByName = jailhouseDomainLookupByName, /* 1.2.22 */
    .domainLookupByUUID = jailhouseDomainLookupByUUID, /* 1.2.22 */
    .domainGetInfo = jailhouseDomainGetInfo,  /* 1.2.22 */
    .domainGetState = jailhouseDomainGetState, /* 1.2.22 */
    .domainGetXMLDesc = jailhouseDomainGetXMLDesc, /* 1.2.22 */
    .domainShutdown = jailhouseDomainShutdown, /* 1.2.22 */
    .domainDestroy = jailhouseDomainDestroy, /* 1.2.22 */
    .domainCreate = jailhouseDomainCreate,    /* 1.2.22 */
    .nodeGetInfo = jailhouseNodeGetInfo /* 1.2.22 */
};

static virConnectDriver jailhouseConnectDriver = {
    .hypervisorDriver = &jailhouseHypervisorDriver,
};

int
jailhouseRegister(void)
{
    return virRegisterConnectDriver(&jailhouseConnectDriver,
                                    false);
}
