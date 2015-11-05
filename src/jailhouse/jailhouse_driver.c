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
#include <dirent.h>
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
#include "nodeinfo.h"
#include <stdio.h>

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
    int lastQueryCellsCount;
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
 *  helper function that returns the number as an int and sets i to be the first char after the number
 */
static int
charsToInt(char* chars, int *i)
{
    int result = 0;
    while(chars[*i] != ',' && chars[*i] != '-' && chars[*i] != ' '){
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
static int
parseCPUs(char* output, int **cpusptr)
{
    int i;
    int count = 1;
    int number;
    int* cpus;
    if(output[0] == ' '){
        *cpusptr = NULL;
        return 0;
    }
    for(i = 0; i<CPULENGTH; i++){
        number = charsToInt(output, &i);
        if(output[i] == ',') count++;
        else if(output[i] == '-') {
            i++;
            count += charsToInt(output, &i) - number;
       }
    }
    cpus = malloc(sizeof(int)*count);
    int j = 0;
    i = 0;
    while(output[i] != ' '){
        number = charsToInt(output, &i);
        if(output[i] == ',' || output[i] == ' ') cpus[j++] = number;
        else if(output[i] == '-'){
            i++;
            int nextNumber = charsToInt(output, &i);
            for(; number <= nextNumber; number++) cpus[j++] = number;
        }
        i++;
    }
    *cpusptr = cpus;
    return count;
}

/*
 *  calls "jailhouse cell list" and parses the output in an array of jailhouse_cell
 */
static int
parseListOutput(virConnectPtr conn, struct jailhouse_cell **parsedOutput)
{
    virCommandPtr cmd = virCommandNew(((struct jailhouse_driver *)conn->privateData)->binary);
    virCommandAddArg(cmd, "cell");
    virCommandAddArg(cmd, "list");
    virCommandAddEnvPassCommon(cmd);
    char *output;
    virCommandSetOutputBuffer(cmd, &output);
    if(virCommandRun(cmd, NULL) < 0)
    {
        return -1;
    }
    int i = 0;
    int count = -1; //  Don't count table header line
    while(output[i] != '\0'){
        if(output[i] == '\n') count++;
        i++;
    }
    *parsedOutput = malloc(count*sizeof(struct jailhouse_cell));
    if(*parsedOutput == NULL){ free(output); return 0; }
    i = 0;
    int j;
    while(output[i++] != '\n'); //  Skip table header line
    for(j = 0; j < count; j++){
        char c = output[i+IDLENGTH];
        output[i+IDLENGTH] = '\0'; //   in case ID is 8 chars long, so beginning of name won't get parsed
        (*parsedOutput)[j].id = atoi(output+i);
        output[i+IDLENGTH] = c;
        i += IDLENGTH;
        strncpy((*parsedOutput)[j].name, output+i, NAMELENGTH);
        int k;
        for(k = 0; k < NAMELENGTH; k++)
            if ((*parsedOutput)[j].name[k] == ' ')
                    break;
        (*parsedOutput)[j].name[k] = '\0';
        i += NAMELENGTH;
        if (strncmp(output+i, STATERUNNINGSTRING, STATELENGTH) == 0) (*parsedOutput)[j].state = STATERUNNING;
        else if (strncmp(output+i, STATESHUTDOWNSTRING, STATELENGTH) == 0) (*parsedOutput)[j].state = STATESHUTDOWN;
        else if (strncmp(output+i, STATEFAILEDSTRING, STATELENGTH) == 0) (*parsedOutput)[j].state = STATEFAILED;
        else if (strncmp(output+i, STATERUNNINGLOCKEDSTRING, STATELENGTH) == 0) (*parsedOutput)[j].state = STATERUNNINGLOCKED;
        i += STATELENGTH;
        (*parsedOutput)[j].assignedCPUsLength = parseCPUs(output+i, &((*parsedOutput)[j].assignedCPUs));
        i += CPULENGTH;
        (*parsedOutput)[j].failedCPUsLength = parseCPUs(output+i, &((*parsedOutput)[j].failedCPUs));
        i += CPULENGTH;
        i++; // skip \n
    }
    free(output);
    return count;
}

/*
 *  Returns the libvirts equivalent of the cell state passed to it
 */
static virDomainState
cellToVirDomainState(struct jailhouse_cell *cell)
{
    switch(cell->state){
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
 *  Check lastQueryCells for cell and copies uuid if found, otherwise generates a new one, this is to preserve UUID in libvirt
 */
static void setUUID(struct jailhouse_cell *cells, int count, struct jailhouse_cell* cell){
    int i;
    for (i = 0; i < count; i++){
        if(strncmp(cells[i].name, cell->name, NAMELENGTH+1))
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
    
    int lastCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *lastCells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    struct jailhouse_cell *cells = NULL;
    int i;
    int count = parseListOutput(conn, &cells);
    for(i = 0; i < count; i++)
        setUUID(lastCells, lastCount, cells+i);
    if(lastCells != NULL){
        for(i = 0; i < lastCount; i++){
            if(lastCells[i].assignedCPUs != NULL) free(lastCells[i].assignedCPUs);
            if(lastCells[i].failedCPUs != NULL) free(lastCells[i].failedCPUs);
        }
        free(lastCells);
    }
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
    int cellsCount = ((struct jailhouse_driver *)dom->conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)dom->conn->privateData)->lastQueryCells;
    int i;
    for(i = 0; i < cellsCount; i++)
        if (dom->id == cells[i].id)
                return cells+i;
    return NULL;
}

static virDrvOpenStatus
jailhouseConnectOpen(virConnectPtr conn, virConnectAuthPtr auth ATTRIBUTE_UNUSED, unsigned int flags ATTRIBUTE_UNUSED)
{
    if (conn->uri->scheme == NULL ||
            STRNEQ(conn->uri->scheme, "jailhouse"))
            return VIR_DRV_OPEN_DECLINED;
    char* binary;
    if(conn->uri->path == NULL) binary = strdup("jailhouse");
    else {
        if (!virFileIsExecutable(conn->uri->path)){
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Path '%s', is not a valid executable file."),
                           conn->uri->path);
            return VIR_DRV_OPEN_ERROR;
        }
        binary = strdup(conn->uri->path);
    }
    virCommandPtr cmd = virCommandNew(binary);
    virCommandAddArg(cmd, "--version");
    virCommandAddEnvPassCommon(cmd);
    char *output;
    virCommandSetOutputBuffer(cmd, &output);
    if(virCommandRun(cmd, NULL) < 0){
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Executing '%s --version' failed."),
                       conn->uri->path);
        return VIR_DRV_OPEN_ERROR;
    }
    if(strncmp(JAILHOUSEVERSIONOUTPUT, output, strlen(JAILHOUSEVERSIONOUTPUT))){
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("%s doesn't seem to be a correct Jailhouse binary."),
                       conn->uri->path);
        return VIR_DRV_OPEN_ERROR;
    }
    struct jailhouse_driver *driver = malloc(sizeof(struct jailhouse_driver));
    driver->binary = binary;
    driver->lastQueryCells = NULL;
    driver->lastQueryCellsCount = 0;
    conn->privateData = driver;
    return VIR_DRV_OPEN_SUCCESS;
}

static int
jailhouseConnectClose(virConnectPtr conn)
{
    int i;
    int cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    if(cells != NULL){
        for(i = 0; i < cellsCount; i++){
            if(cells[i].assignedCPUs != NULL) free(cells[i].assignedCPUs);
            if(cells[i].failedCPUs != NULL) free(cells[i].failedCPUs);
        }
        free(cells);
    }
    free(((struct jailhouse_driver *)conn->privateData)->binary);
    free(conn->privateData);
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
    int cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    int i;
    for(i = 0; i < maxids && i < cellsCount; i++)
        ids[i] = cells[i].id;
    return i;
}

static int
jailhouseConnectListAllDomains(virConnectPtr conn, virDomainPtr ** domains, unsigned int flags ATTRIBUTE_UNUSED)
{
    getCurrentCellList(conn);
    int cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    *domains = malloc(sizeof(virDomainPtr) * (cellsCount+1));
    int i;
    for(i = 0;i < cellsCount; i++)
        (*domains)[i] = cellToVirDomainPtr(conn, cells+i);
    (*domains)[cellsCount] = NULL;
    return cellsCount;
}

static virDomainPtr
jailhouseDomainLookupByID(virConnectPtr conn, int id)
{
    getCurrentCellList(conn);
    int cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    int i;
    for(i = 0; i < cellsCount; i++)
        if(cells[i].id == id)
            return cellToVirDomainPtr(conn, cells+i);
    return NULL;
}

static virDomainPtr
jailhouseDomainLookupByName(virConnectPtr conn, const char *lookupName)
{
    getCurrentCellList(conn);
    int cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    int i;
    for(i = 0; i < cellsCount; i++)
        if(strcmp(cells[i].name, lookupName) == 0)
            return cellToVirDomainPtr(conn, cells+i);
    return NULL;
}

static virDomainPtr
jailhouseDomainLookupByUUID(virConnectPtr conn, const unsigned char * uuid)
{
    getCurrentCellList(conn);
    int cellsCount = ((struct jailhouse_driver *)conn->privateData)->lastQueryCellsCount;
    struct jailhouse_cell *cells = ((struct jailhouse_driver *)conn->privateData)->lastQueryCells;
    int i;
    for(i = 0; i < cellsCount; i++)
        if(memcmp(cells[i].uuid, (const char*)uuid, VIR_UUID_BUFLEN) == 0)
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
    if(cell == NULL)
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
                        int *reason ATTRIBUTE_UNUSED, unsigned int flags ATTRIBUTE_UNUSED)
{
    struct jailhouse_cell *cell = virDomainPtrToCell(domain);
    if(cell == NULL)
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
    sprintf(buf, "%d", domain->id);
    virCommandAddArg(cmd, buf);
    virCommandAddEnvPassCommon(cmd);
    if(virCommandRun(cmd, NULL) < 0)
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
    sprintf(buf, "%d", domain->id);
    virCommandAddArg(cmd, buf);
    virCommandAddEnvPassCommon(cmd);
    if(virCommandRun(cmd, NULL) < 0)
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
    sprintf(buf, "%d", domain->id);
    virCommandAddArg(cmd, buf);
    virCommandAddEnvPassCommon(cmd);
    char *output = NULL, *errors = NULL;
    virCommandSetOutputBuffer(cmd, &output);
    virCommandSetErrorBuffer(cmd, &errors);
    if(virCommandRun(cmd, NULL) < 0)
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
    return strdup("<capabilities></capabilities>");
}

/*
 *  Returns a dummy XML for virt-manager
 */
static char *
jailhouseDomainGetXMLDesc(virDomainPtr domain, unsigned int flags ATTRIBUTE_UNUSED)
{
    char buf[200];
    char uuid[VIR_UUID_STRING_BUFLEN];
    virDomainGetUUIDString(domain, uuid);
    sprintf(buf, "<domain type =\"jailhouse\">\n\
            <name>%s</name>\n\
            <uuid>%s</uuid>\n\
            </domain>", domain->name, uuid);
    return strdup(buf);
}

static virHypervisorDriver jailhouseHypervisorDriver = {
    .name = "jailhouse",
    .connectOpen = jailhouseConnectOpen, /* 0.1.0 */
    .connectClose = jailhouseConnectClose, /* 0.1.0 */
    .connectGetCapabilities = jailhouseConnectGetCapabilities, /* 0.1.0 */
    .connectNumOfDomains = jailhouseConnectNumOfDomains, /* 0.1.0 */
    .connectListDomains = jailhouseConnectListDomains, /* 0.1.0 */
    .connectIsAlive = jailhouseConnectIsAlive, /* 0.1.0 */
    .connectListAllDomains = jailhouseConnectListAllDomains, /* 0.1.0 */
    .domainLookupByID = jailhouseDomainLookupByID, /* 0.1.0 */
    .domainLookupByName = jailhouseDomainLookupByName, /* 0.1.0 */
    .domainLookupByUUID = jailhouseDomainLookupByUUID, /* 0.1.0 */
    .domainGetInfo = jailhouseDomainGetInfo,  /* 0.1.0 */
    .domainGetState = jailhouseDomainGetState, /* 0.1.0 */
    .domainGetXMLDesc = jailhouseDomainGetXMLDesc, /* 0.1.0 */
    .domainShutdown = jailhouseDomainShutdown, /* 0.1.0 */
    .domainDestroy = jailhouseDomainDestroy, /* 0.1.0 */
    .domainCreate = jailhouseDomainCreate,    /* 0.1.0 */
    .nodeGetInfo = jailhouseNodeGetInfo /* 0.1.0 */
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
