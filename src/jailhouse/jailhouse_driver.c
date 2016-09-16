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
#define JAILHOUSEBINARY "jailhouse"

struct virJailhouseCell {
    int id;
    char name[NAMELENGTH+1];
    int state;
    virBitmapPtr assignedCPUs;
    virBitmapPtr failedCPUs; /* currently unused */
    unsigned char uuid[VIR_UUID_BUFLEN];
};
typedef struct virJailhouseCell virJailhouseCell;
typedef virJailhouseCell *virJailhouseCellPtr;

/*
 *  Because virCommandRunRegex Callback gets called every line
 */
struct virJailhouseCellCallbackData {
    size_t ncells;
    virJailhouseCellPtr cells;
};
typedef struct virJailhouseCellCallbackData virJailhouseCellCallbackData;
typedef virJailhouseCellCallbackData *virJailhouseCellCallbackDataPtr;

/*
 *  The driver requeries the cells on most calls, it stores the result of the
 *  last query, so it can copy the UUIDs in the new query if the cell is the
 *  same(otherwise it just generates a new one).
 *  not preserving the UUID results in a lot of bugs in libvirts clients.
 */
struct virJailhouseDriver {
    size_t lastQueryCellsCount;
    virJailhouseCellPtr lastQueryCells;
};
typedef struct virJailhouseDriver virJailhouseDriver;
typedef virJailhouseDriver *virJailhouseDriverPtr;

static int virJailhouseParseListOutputCallback(char **const groups, void *data)
{
    virJailhouseCellCallbackDataPtr celldata = (virJailhouseCellCallbackDataPtr) data;
    virJailhouseCellPtr cells = celldata->cells;
    size_t count = celldata->ncells;
    char* endptr = groups[0] + strlen(groups[0]) - 1;
    char* state = groups[2];
    if (VIR_EXPAND_N(cells, count, 1))
        return -1;
    celldata->ncells++;

    if (virStrToLong_i(groups[0], &endptr, 0, &cells[count-1].id))
        return -1;
    if (!virStrcpy(cells[count-1].name, groups[1], NAMELENGTH+1))
        return -1;
    if (STREQLEN(state, STATERUNNINGSTRING, STATELENGTH))
        cells[count-1].state = STATERUNNING;
    else if (STREQLEN(state, STATESHUTDOWNSTRING, STATELENGTH))
        cells[count-1].state = STATESHUTDOWN;
    else if (STREQLEN(state, STATERUNNINGLOCKEDSTRING, STATELENGTH))
        cells[count-1].state = STATERUNNINGLOCKED;
    else
        cells[count-1].state = STATEFAILED;
    if (groups[3][0] == '\0')
        cells[count-1].assignedCPUs = NULL;
    else virBitmapParse(groups[3], 0, &cells[count-1].assignedCPUs, VIR_DOMAIN_CPUMASK_LEN);
    if (groups[4][0] == '\0')
        cells[count-1].failedCPUs = NULL;
    else virBitmapParse(groups[4], 0, &cells[count-1].failedCPUs, VIR_DOMAIN_CPUMASK_LEN);
    celldata->cells = cells;
    return 0;
}

/*
 *  calls "jailhouse cell list" and parses the output in an array of virJailhouseCell
 *  example output:
 *  ID      Name                    State           Assigned CPUs           Failed CPUs
 *  0       QEMU-VM                 running         0-3
 */
static ssize_t
virJailhouseParseListOutput(virJailhouseCellPtr *parsedCells)
{
    int nvars[] = { 5 };
    virJailhouseCellCallbackData callbackData;
    const char *regex[] = { "([0-9]{1,8})\\s*([-0-9a-zA-Z]{1,24})\\s*([a-z/ ]{1,16})\\s*([0-9,-]{1,24})?\\s*([0-9,-]{1,24})?\\s*" };
    virCommandPtr cmd = virCommandNew(JAILHOUSEBINARY);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "list");
    virCommandAddEnvPassCommon(cmd);
    callbackData.cells = NULL;
    callbackData.ncells = 0;
    if (virCommandRunRegex(cmd, 1, regex, nvars, &virJailhouseParseListOutputCallback, &callbackData, NULL) < 0) {
        virCommandFree(cmd);
        return -1;
    }
    virCommandFree(cmd);
    *parsedCells = callbackData.cells;
    return callbackData.ncells;
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
    if (cell->state == STATERUNNING || cell->state == STATERUNNINGLOCKED)
        dom->id = cell->id;
    else dom->id = -1;
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
static int
virJailhouseGetCurrentCellList(virConnectPtr conn)
{
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    ssize_t count;
    size_t i;
    size_t lastCount = driver->lastQueryCellsCount;
    virJailhouseCellPtr lastCells = driver->lastQueryCells;
    virJailhouseCellPtr cells = NULL;

    count = virJailhouseParseListOutput(&cells);
    for (i = 0; i < count; i++)
        virJailhouseSetUUID(lastCells, lastCount, cells+i);
    for (i = 0; i < lastCount; i++) {
        virBitmapFree(lastCells[i].assignedCPUs);
        virBitmapFree(lastCells[i].failedCPUs);
    }
    VIR_FREE(lastCells);
    driver->lastQueryCells = cells;
    driver->lastQueryCellsCount = count;
    return count;
}

/*
 *  Converts libvirts virDomainPtr to the internal virJailhouseCell by parsing the "jailhouse cell list" output
 *  and looking up the name of the virDomainPtr, returns NULL if cell is no longer present
 */
static virJailhouseCellPtr
virDomainPtrToCell(virDomainPtr dom)
{
    virJailhouseDriverPtr driver = dom->conn->privateData;
    size_t cellsCount;
    size_t i;
    if (virJailhouseGetCurrentCellList(dom->conn) == -1)
        return NULL;
    cellsCount = driver->lastQueryCellsCount;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    for (i = 0; i < cellsCount; i++)
        if (!strncmp(cells[i].name, dom->name, NAMELENGTH+1))
                return cells+i;
    return NULL;
}

static virDrvOpenStatus
jailhouseConnectOpen(virConnectPtr conn, virConnectAuthPtr auth ATTRIBUTE_UNUSED, unsigned int flags)
{
    virCheckFlags(0, VIR_DRV_OPEN_ERROR);
    virJailhouseDriverPtr driver;
    if (conn->uri == NULL || conn->uri->scheme == NULL ||
            STRNEQ(conn->uri->scheme, "jailhouse"))
        return VIR_DRV_OPEN_DECLINED;
    if (conn->uri->path != NULL && STRNEQ(conn->uri->path, "/")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                        _("Unexpected Jailhouse URI path '%s', try jailhouse:///"),
                        conn->uri->path);
        return VIR_DRV_OPEN_ERROR;
    }
    virCommandPtr cmd = virCommandNew(JAILHOUSEBINARY);
    virCommandAddEnvPassCommon(cmd);
    virCommandAddArg(cmd, "--version");
    if (virCommandRun(cmd, NULL) < 0) {
        virCommandFree(cmd);
        return VIR_DRV_OPEN_ERROR;
    }
    virCommandFree(cmd);
    if (VIR_ALLOC(driver) < 0)
        return VIR_DRV_OPEN_ERROR;
    driver->lastQueryCells = NULL;
    driver->lastQueryCellsCount = 0;
    conn->privateData = driver;
    return VIR_DRV_OPEN_SUCCESS;
}

static int
jailhouseConnectClose(virConnectPtr conn)
{

    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    size_t i;
    size_t cellsCount = driver->lastQueryCellsCount;
    virJailhouseCellPtr cells = driver->lastQueryCells;
    for (i = 0; i < cellsCount; i++) {
        virBitmapFree(cells[i].assignedCPUs);
        virBitmapFree(cells[i].failedCPUs);
    }
    VIR_FREE(cells);
    VIR_FREE(driver);
    conn->privateData = NULL;
    return 0;
}

static int
jailhouseConnectNumOfDomains(virConnectPtr conn)
{
    if (virJailhouseGetCurrentCellList(conn) == -1)
        return -1;
    return ((virJailhouseDriverPtr)conn->privateData)->lastQueryCellsCount;
}

static int
jailhouseConnectListDomains(virConnectPtr conn, int * ids, int maxids)
{
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    virJailhouseCellPtr cells;
    size_t cellsCount;
    size_t i;
    size_t j = 0;
    if (virJailhouseGetCurrentCellList(conn) == -1)
        return -1;
    cellsCount = driver->lastQueryCellsCount;
    cells = driver->lastQueryCells;
    for (i = 0; i < maxids && i < cellsCount; i++) {
        if (cells[i].id != -1)
            ids[j++] = cells[i].id;
    }
    return j;
}

static int
jailhouseConnectListAllDomains(virConnectPtr conn, virDomainPtr ** domains, unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_ACTIVE, 0);
    virJailhouseDriverPtr driver = (virJailhouseDriverPtr)conn->privateData;
    virJailhouseCellPtr cells;
    size_t cellsCount;
    size_t i;
    if (virJailhouseGetCurrentCellList(conn) == -1)
        goto error;

    cellsCount = driver->lastQueryCellsCount;
    cells = driver->lastQueryCells;
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
    if (virJailhouseGetCurrentCellList(conn) == -1)
        return NULL;

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
    if (virJailhouseGetCurrentCellList(conn) == -1)
        return NULL;

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
    if (virJailhouseGetCurrentCellList(conn) == -1)
        return NULL;
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
    if (cell->assignedCPUs == NULL) info->nrVirtCpu = 0;
    else info->nrVirtCpu = virBitmapCountBits(cell->assignedCPUs);
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
jailhouseDomainDestroy(virDomainPtr domain)
{
    int resultcode;
    virCommandPtr cmd = virCommandNew(JAILHOUSEBINARY);
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

static int
jailhouseDomainCreate(virDomainPtr domain)
{
    int resultcode;
    virCommandPtr cmd = virCommandNew(JAILHOUSEBINARY);
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
    virCheckFlags(VIR_DOMAIN_XML_SECURE | VIR_DOMAIN_XML_INACTIVE, NULL);
    char* xml;
    virDomainDefPtr domainDef = virDomainDefNewFull(domain->name, domain->uuid, domain->id);
    xml = virDomainDefFormat(domainDef, 0);
    virDomainDefFree(domainDef);
    return xml;
}

static virHypervisorDriver jailhouseHypervisorDriver = {
    .name = "jailhouse",
    .connectOpen = jailhouseConnectOpen, /* 1.3.1 */
    .connectClose = jailhouseConnectClose, /* 1.3.1 */
    .connectGetCapabilities = jailhouseConnectGetCapabilities, /* 1.3.1 */
    .connectNumOfDomains = jailhouseConnectNumOfDomains, /* 1.3.1 */
    .connectListDomains = jailhouseConnectListDomains, /* 1.3.1 */
    .connectIsAlive = jailhouseConnectIsAlive, /* 1.3.1 */
    .connectListAllDomains = jailhouseConnectListAllDomains, /* 1.3.1 */
    .domainLookupByID = jailhouseDomainLookupByID, /* 1.3.1 */
    .domainLookupByName = jailhouseDomainLookupByName, /* 1.3.1 */
    .domainLookupByUUID = jailhouseDomainLookupByUUID, /* 1.3.1 */
    .domainGetInfo = jailhouseDomainGetInfo,  /* 1.3.1 */
    .domainGetState = jailhouseDomainGetState, /* 1.3.1 */
    .domainGetXMLDesc = jailhouseDomainGetXMLDesc, /* 1.3.1 */
    .domainDestroy = jailhouseDomainDestroy, /* 1.3.1 */
    .domainCreate = jailhouseDomainCreate,    /* 1.3.1 */
    .nodeGetInfo = jailhouseNodeGetInfo /* 1.3.1 */
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
