/*
 * jailhouse_driver.c: hypervisor driver for managing Jailhouse cells
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
 *  The driver requeries the cells on most calls, it stores the result of the last query, so it can copy the UUIDs in the new query if the cell is the same(otherwise it just generates a new one)
 *  not preserving the UUID results in a lot of bugs in libvirts clients.
 */
struct jailhouse_driver {
    char *binary;
    size_t lastQueryCellsCount;
    struct jailhouse_cell* lastQueryCells;
};

/*
 *  CPUs are currently unused but this might change
 */
struct jailhouse_cell {
    int id;
    char name[NAMELENGTH+1];
    int state;
    int *assignedCPUs; //Don't use cpumask because remote system might have different # of cpus
    int assignedCPUsLength;
    int *failedCPUs;
    int failedCPUsLength;
    unsigned char uuid[VIR_UUID_BUFLEN];
};

/*
 *  helper function that returns the number as an integer and sets i to be the first char after the number
 */
static int
charsToInt(char* chars, size_t *i)
{
    int result = 0;
    while (chars[*i] != ',' && chars[*i] != '-' && chars[*i] != ' ') {
        result *= 10;
        result += chars[*i] - '0';
        (*i)++;
    }
    return result;
}

/*
 *  Takes a string in the format of "jailhouse cell list" as input,
 *  allocates an int array in which every CPU is explicitly listed and saves a pointer in cpusptr
 */
static size_t
parseCPUs(char* output, int **cpusptr)
{
    size_t i;
    size_t count = 1;
    int number;
    int* cpus;
    if (output[0] == ' ') {
        *cpusptr = NULL;
        return 0;
    }
    for (i = 0; i<CPULENGTH; i++) {
        number = charsToInt(output, &i);
        if (output[i] == ',') {
            count++;
        } else if (output[i] == '-') {
            i++;
            count += charsToInt(output, &i) - number;
       }
    }
    if (VIR_ALLOC_N(cpus, count)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Failed to allocate CPUs array of size %zu"), count);
        return 0;
    }
    size_t j = 0;
    i = 0;
    while (output[i] != ' ') {
        number = charsToInt(output, &i);
        if (output[i] == ',' || output[i] == ' ') {
            cpus[j++] = number;
        } else if (output[i] == '-') {
            i++;
            int nextNumber = charsToInt(output, &i);
            for (; number <= nextNumber; number++) cpus[j++] = number;
        }
        i++;
    }
    *cpusptr = cpus;
    return count;
}

/*
 *  calls "jailhouse cell list" and parses the output in an array of jailhouse_cell
 */
static size_t
parseListOutput(virConnectPtr conn, struct jailhouse_cell **parsedOutput)
{
    virCommandPtr cmd = virCommandNew(((struct jailhouse_driver *)conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "list");
    virCommandAddEnvPassCommon(cmd);
    char *output;
    virCommandSetOutputBuffer(cmd, &output);
    if (virCommandRun(cmd, NULL) < 0)
        return -1;
    size_t i = 0;
    size_t count = -1; //  Don't count table header line
    while (output[i] != '\0') {
        if (output[i] == '\n') count++;
        i++;
    }
    if (VIR_ALLOC_N(*parsedOutput, count)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                  _("Failed to allocate jailhouse_cell array of size %zu"), count);
        return 0;
    }
    if (*parsedOutput == NULL) { VIR_FREE(output); return 0; }
    i = 0;
    size_t j;
    while (output[i++] != '\n'); //  Skip table header line
    for (j = 0; j < count; j++) {
        size_t k;
        for (k = 0; k <= IDLENGTH; k++) // char after number needs to be NUL for virStrToLong
            if (output[i+k] == ' ') {
                output[i+k] = '\0';
                break;
            }
        char c = output[i+IDLENGTH];
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
        (*parsedOutput)[j].assignedCPUsLength = parseCPUs(output+i, &((*parsedOutput)[j].assignedCPUs));
        i += CPULENGTH;
        (*parsedOutput)[j].failedCPUsLength = parseCPUs(output+i, &((*parsedOutput)[j].failedCPUs));
        i += CPULENGTH;
        i++; // skip \n
    }
    VIR_FREE(output);
    return count;
    error:
    for (i = 0; i < count; i++) {
        VIR_FREE((*parsedOutput)[i].assignedCPUs);
        VIR_FREE((*parsedOutput)[i].failedCPUs);
    }
    VIR_FREE(*parsedOutput);
    *parsedOutput = NULL;
    return 0;
}

/*
 *  Returns the libvirts equivalent of the cell state passed to it
 */
static virDomainState
cellToVirDomainState(struct jailhouse_cell *cell)
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
 *  Returns a new virDomainPtr filled with the data of the jailhouse_cell
 */
static virDomainPtr
cellToVirDomainPtr(virConnectPtr conn, struct jailhouse_cell *cell)
{
    virDomainPtr dom = virGetDomain(conn, cell->name, cell->uuid);
    dom->id = cell->id;
    return dom;
}

/*
 *  Check cells for cell and copies UUID if found, otherwise generates a new one, this is to preserve UUID in libvirt
 */
static void setUUID(struct jailhouse_cell *cells, size_t count, struct jailhouse_cell* cell) {
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
getCurrentCellList(virConnectPtr conn)
{
    size_t lastCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *lastCells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    struct jailhouse_cell *cells = NULL;
    size_t i;
    size_t count = parseListOutput(conn, &cells);
    for (i = 0; i < count; i++)
        setUUID(lastCells, lastCount, cells+i);
    for (i = 0; i < lastCount; i++) {
        VIR_FREE(lastCells[i].assignedCPUs);
        VIR_FREE(lastCells[i].failedCPUs);
    }
    VIR_FREE(lastCells);
    ((struct jailhouse_driver *)conn->privateData)->lastQueryCells = cells;
    ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount = count;
}

/*
 *  Converts libvirts virDomainPtr to the internal jailhouse_cell by parsing the "jailhouse cell list" output
 *  and looking up the name of the virDomainPtr, returns NULL if cell is no longer present
 */
static struct jailhouse_cell *
virDomainPtrToCell(virDomainPtr dom)
{
    getCurrentCellList(dom->conn);
    size_t cellsCount = ((struct jailhouse_driver *)dom->conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)dom->conn->privateData)->lastQueryCells;
    size_t i;
    for (i = 0; i < cellsCount; i++)
        if (dom->id == cells[i].id)
                return cells+i;
    return NULL;
}

static virDrvOpenStatus
jailhouseConnectOpen(virConnectPtr conn, virConnectAuthPtr auth ATTRIBUTE_UNUSED, unsigned int flags)
{
    virCheckFlags(0, VIR_DRV_OPEN_ERROR);
    if (conn->uri->scheme == NULL ||
            STRNEQ(conn->uri->scheme, "jailhouse"))
            return VIR_DRV_OPEN_DECLINED;
    char* binary;
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
    char *output;
    virCommandSetOutputBuffer(cmd, &output);
    if (virCommandRun(cmd, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Executing '%s --version' failed."),
                       conn->uri->path);
        return VIR_DRV_OPEN_ERROR;
    }
    if (STRNEQLEN(JAILHOUSEVERSIONOUTPUT, output, strlen(JAILHOUSEVERSIONOUTPUT))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("%s doesn't seem to be a correct Jailhouse binary."),
                       conn->uri->path);
        return VIR_DRV_OPEN_ERROR;
    }
    struct jailhouse_driver *driver;
    if (VIR_ALLOC(driver))
        return VIR_DRV_OPEN_ERROR;
    driver->binary = binary;
    driver->lastQueryCells = NULL;
    driver->lastQueryCellsCount = 0;
    conn->privateData = driver;
    return VIR_DRV_OPEN_SUCCESS;
}

static int
jailhouseConnectClose(virConnectPtr conn)
{
    size_t i;
    size_t cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    for (i = 0; i < cellsCount; i++) {
        VIR_FREE(cells[i].assignedCPUs);
        VIR_FREE(cells[i].failedCPUs);
    }
    VIR_FREE(cells);
    VIR_FREE(((struct jailhouse_driver *)conn->privateData)->binary);
    VIR_FREE(conn->privateData);
    conn->privateData = NULL;
    return 0;
}

static int
jailhouseConnectNumOfDomains(virConnectPtr conn)
{
    getCurrentCellList(conn);
    return ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
}

static int
jailhouseConnectListDomains(virConnectPtr conn, int * ids, int maxids)
{
    getCurrentCellList(conn);
    size_t cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    size_t i;
    for (i = 0; i < maxids && i < cellsCount; i++)
        ids[i] = cells[i].id;
    return i;
}

static int
jailhouseConnectListAllDomains(virConnectPtr conn, virDomainPtr ** domains, unsigned int flags)
{
    virCheckFlags(0, 0);
    getCurrentCellList(conn);
    size_t cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    if (VIR_ALLOC_N(*domains, cellsCount+1)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Failed to allocate virDomainPtr array of size %zu"), cellsCount+1);
        return 0;
    }
    size_t i;
    for (i = 0; i < cellsCount; i++)
        (*domains)[i] = cellToVirDomainPtr(conn, cells+i);
    (*domains)[cellsCount] = NULL;
    return cellsCount;
}

static virDomainPtr
jailhouseDomainLookupByID(virConnectPtr conn, int id)
{
    getCurrentCellList(conn);
    size_t cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    size_t i;
    for (i = 0; i < cellsCount; i++)
        if (cells[i].id == id)
            return cellToVirDomainPtr(conn, cells+i);
    return NULL;
}

static virDomainPtr
jailhouseDomainLookupByName(virConnectPtr conn, const char *lookupName)
{
    getCurrentCellList(conn);
    size_t cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    size_t i;
    for (i = 0; i < cellsCount; i++)
        if (STREQ(cells[i].name, lookupName))
            return cellToVirDomainPtr(conn, cells+i);
    return NULL;
}

static virDomainPtr
jailhouseDomainLookupByUUID(virConnectPtr conn, const unsigned char * uuid)
{
    getCurrentCellList(conn);
    size_t cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    size_t i;
    for (i = 0; i < cellsCount; i++)
        if (memcmp(cells[i].uuid, (const char*)uuid, VIR_UUID_BUFLEN) == 0)
            return cellToVirDomainPtr(conn, cells+i);
    return NULL;
}

/*
 *  There currently is no straightforward way for the driver to retrieve those,
 *  so maxMem, memory and cpuTime have dummy values
 */
static int
jailhouseDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
    struct jailhouse_cell *cell = virDomainPtrToCell(domain);
    if (cell == NULL)
        return -1;
    info->state = cellToVirDomainState(cell);
    info->maxMem = 1;
    info->memory = 1;
    info->nrVirtCpu = cell->assignedCPUsLength;
    info->cpuTime = 1;
    return 0;
}

static int
jailhouseDomainGetState(virDomainPtr domain, int *state,
                        int *reason ATTRIBUTE_UNUSED, unsigned int flags)
{
    virCheckFlags(0, 0);
    struct jailhouse_cell *cell = virDomainPtrToCell(domain);
    if (cell == NULL)
        return -1;
    *state = cellToVirDomainState(cell);
    return 0;
}

static int
jailhouseDomainShutdown(virDomainPtr domain)
{
    virCommandPtr cmd = virCommandNew(((struct jailhouse_driver *)domain->conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "shutdown");
    char buf[IDLENGTH+1];
    snprintf(buf, IDLENGTH+1, "%d", domain->id);
    virCommandAddArg(cmd, buf);
    virCommandAddEnvPassCommon(cmd);
    if (virCommandRun(cmd, NULL) < 0)
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
    virCommandPtr cmd = virCommandNew(((struct jailhouse_driver *)domain->conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "destroy");
    char buf[IDLENGTH+1];
    snprintf(buf, IDLENGTH+1, "%d", domain->id);
    virCommandAddArg(cmd, buf);
    virCommandAddEnvPassCommon(cmd);
    if (virCommandRun(cmd, NULL) < 0)
        return -1;
    return 0;
}

static int
jailhouseDomainCreate(virDomainPtr domain)
{
    virCommandPtr cmd = virCommandNew(((struct jailhouse_driver *)domain->conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "start");
    char buf[IDLENGTH+1];
    snprintf(buf, IDLENGTH+1, "%d", domain->id);
    virCommandAddArg(cmd, buf);
    virCommandAddEnvPassCommon(cmd);
    char *output = NULL, *errors = NULL;
    virCommandSetOutputBuffer(cmd, &output);
    virCommandSetErrorBuffer(cmd, &errors);
    if (virCommandRun(cmd, NULL) < 0)
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
    char* caps;
    if (VIR_STRDUP(caps, "<capabilities></capabilities>") != 1)
        return NULL;
    return caps;
}

/*
 *  Returns a dummy XML for virt-manager
 */
static char *
jailhouseDomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    virCheckFlags(0, NULL);
    char buf[200];
    char uuid[VIR_UUID_STRING_BUFLEN];
    virDomainGetUUIDString(domain, uuid);
    snprintf(buf, 200, "<domain type =\"jailhouse\">\n\
            <name>%s</name>\n\
            <uuid>%s</uuid>\n\
            </domain>", domain->name, uuid);
    char* result;
    if (VIR_STRDUP(result, buf) != 1)
        return NULL;
    return result;
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
