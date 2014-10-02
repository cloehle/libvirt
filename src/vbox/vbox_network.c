/*
 * Copyright (C) 2014 Taowei Luo (uaedante@gmail.com)
 * Copyright (C) 2010-2014 Red Hat, Inc.
 * Copyright (C) 2008-2009 Sun Microsystems, Inc.
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
 */

#include <config.h>

#include "internal.h"
#include "datatypes.h"
#include "domain_conf.h"
#include "domain_event.h"
#include "virlog.h"
#include "virstring.h"
#include "viralloc.h"
#include "network_conf.h"

#include "vbox_common.h"
#include "vbox_uniformed_api.h"

#define VIR_FROM_THIS VIR_FROM_VBOX

VIR_LOG_INIT("vbox.vbox_network");

#define RC_SUCCEEDED(rc) NS_SUCCEEDED(rc.resultCode)
#define RC_FAILED(rc) NS_FAILED(rc.resultCode)

#define VBOX_UTF16_FREE(arg)                                            \
    do {                                                                \
        if (arg) {                                                      \
            gVBoxAPI.UPFN.Utf16Free(data->pFuncs, arg);                 \
            (arg) = NULL;                                               \
        }                                                               \
    } while (0)

#define VBOX_UTF8_FREE(arg)                                             \
    do {                                                                \
        if (arg) {                                                      \
            gVBoxAPI.UPFN.Utf8Free(data->pFuncs, arg);                  \
            (arg) = NULL;                                               \
        }                                                               \
    } while (0)

#define VBOX_UTF16_TO_UTF8(arg1, arg2)  gVBoxAPI.UPFN.Utf16ToUtf8(data->pFuncs, arg1, arg2)
#define VBOX_UTF8_TO_UTF16(arg1, arg2)  gVBoxAPI.UPFN.Utf8ToUtf16(data->pFuncs, arg1, arg2)

#define VBOX_RELEASE(arg)                                                     \
    do {                                                                      \
        if (arg) {                                                            \
            gVBoxAPI.nsUISupports.Release((void *)arg);                       \
            (arg) = NULL;                                                     \
        }                                                                     \
    } while (0)

#define vboxIIDUnalloc(iid)                     gVBoxAPI.UIID.vboxIIDUnalloc(data, iid)
#define vboxIIDToUUID(iid, uuid)                gVBoxAPI.UIID.vboxIIDToUUID(data, iid, uuid)
#define vboxIIDFromUUID(iid, uuid)              gVBoxAPI.UIID.vboxIIDFromUUID(data, iid, uuid)
#define vboxIIDIsEqual(iid1, iid2)              gVBoxAPI.UIID.vboxIIDIsEqual(data, iid1, iid2)
#define DEBUGIID(msg, iid)                      gVBoxAPI.UIID.DEBUGIID(msg, iid)
#define vboxIIDFromArrayItem(iid, array, idx) \
    gVBoxAPI.UIID.vboxIIDFromArrayItem(data, iid, array, idx)

#define VBOX_IID_INITIALIZE(iid)                gVBoxAPI.UIID.vboxIIDInitialize(iid)

#define ARRAY_GET_MACHINES \
    (gVBoxAPI.UArray.handleGetMachines(data->vboxObj))

static vboxUniformedAPI gVBoxAPI;

/**
 * The Network Functions here on
 */

virDrvOpenStatus vboxNetworkOpen(virConnectPtr conn,
                                 virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                 unsigned int flags)
{
    vboxGlobalData *data = conn->privateData;

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (STRNEQ(conn->driver->name, "VBOX"))
        goto cleanup;

    if (!data->pFuncs || !data->vboxObj || !data->vboxSession)
        goto cleanup;

    VIR_DEBUG("network initialized");
    /* conn->networkPrivateData = some network specific data */
    return VIR_DRV_OPEN_SUCCESS;

 cleanup:
    return VIR_DRV_OPEN_DECLINED;
}

int vboxNetworkClose(virConnectPtr conn)
{
    VIR_DEBUG("network uninitialized");
    conn->networkPrivateData = NULL;
    return 0;
}

int vboxConnectNumOfNetworks(virConnectPtr conn)
{
    vboxGlobalData *data = conn->privateData;
    vboxArray networkInterfaces = VBOX_ARRAY_INITIALIZER;
    IHost *host = NULL;
    size_t i = 0;
    int ret = -1;

    if (!data->vboxObj)
        return ret;

    gVBoxAPI.UIVirtualBox.GetHost(data->vboxObj, &host);
    if (!host)
        return ret;

    gVBoxAPI.UArray.vboxArrayGet(&networkInterfaces, host,
                                 gVBoxAPI.UArray.handleHostGetNetworkInterfaces(host));

    ret = 0;
    for (i = 0; i < networkInterfaces.count; i++) {
        IHostNetworkInterface *networkInterface = networkInterfaces.items[i];
        PRUint32 status = HostNetworkInterfaceStatus_Unknown;
        PRUint32 interfaceType = 0;

        if (!networkInterface)
            continue;

        gVBoxAPI.UIHNInterface.GetInterfaceType(networkInterface, &interfaceType);
        if (interfaceType != HostNetworkInterfaceType_HostOnly)
            continue;

        gVBoxAPI.UIHNInterface.GetStatus(networkInterface, &status);

        if (status == HostNetworkInterfaceStatus_Up)
            ret++;
    }

    gVBoxAPI.UArray.vboxArrayRelease(&networkInterfaces);

    VBOX_RELEASE(host);

    VIR_DEBUG("numActive: %d", ret);
    return ret;
}

int vboxConnectListNetworks(virConnectPtr conn, char **const names, int nnames)
{
    vboxGlobalData *data = conn->privateData;
    vboxArray networkInterfaces = VBOX_ARRAY_INITIALIZER;
    IHost *host = NULL;
    size_t i = 0;
    int ret = -1;

    if (!data->vboxObj)
        return ret;

    gVBoxAPI.UIVirtualBox.GetHost(data->vboxObj, &host);
    if (!host)
        return ret;

    gVBoxAPI.UArray.vboxArrayGet(&networkInterfaces, host,
                                 gVBoxAPI.UArray.handleHostGetNetworkInterfaces(host));

    ret = 0;
    for (i = 0; (ret < nnames) && (i < networkInterfaces.count); i++) {
        IHostNetworkInterface *networkInterface = networkInterfaces.items[i];
        char *nameUtf8 = NULL;
        PRUnichar *nameUtf16 = NULL;
        PRUint32 interfaceType = 0;
        PRUint32 status = HostNetworkInterfaceStatus_Unknown;

        if (!networkInterface)
            continue;

        gVBoxAPI.UIHNInterface.GetInterfaceType(networkInterface, &interfaceType);

        if (interfaceType != HostNetworkInterfaceType_HostOnly)
            continue;

        gVBoxAPI.UIHNInterface.GetStatus(networkInterface, &status);

        if (status != HostNetworkInterfaceStatus_Up)
            continue;

        gVBoxAPI.UIHNInterface.GetName(networkInterface, &nameUtf16);
        VBOX_UTF16_TO_UTF8(nameUtf16, &nameUtf8);

        VIR_DEBUG("nnames[%d]: %s", ret, nameUtf8);
        if (VIR_STRDUP(names[ret], nameUtf8) >= 0)
            ret++;

        VBOX_UTF8_FREE(nameUtf8);
        VBOX_UTF16_FREE(nameUtf16);
    }

    gVBoxAPI.UArray.vboxArrayRelease(&networkInterfaces);

    VBOX_RELEASE(host);

    return ret;
}

int vboxConnectNumOfDefinedNetworks(virConnectPtr conn)
{
    vboxGlobalData *data = conn->privateData;
    vboxArray networkInterfaces = VBOX_ARRAY_INITIALIZER;
    IHost *host = NULL;
    size_t i = 0;
    int ret = -1;

    if (!data->vboxObj)
        return ret;

    gVBoxAPI.UIVirtualBox.GetHost(data->vboxObj, &host);
    if (!host)
        return ret;

    gVBoxAPI.UArray.vboxArrayGet(&networkInterfaces, host,
                                 gVBoxAPI.UArray.handleHostGetNetworkInterfaces(host));

    ret = 0;
    for (i = 0; i < networkInterfaces.count; i++) {
        IHostNetworkInterface *networkInterface = networkInterfaces.items[i];
        PRUint32 status = HostNetworkInterfaceStatus_Unknown;
        PRUint32 interfaceType = 0;

        if (!networkInterface)
            continue;

        gVBoxAPI.UIHNInterface.GetInterfaceType(networkInterface, &interfaceType);
        if (interfaceType != HostNetworkInterfaceType_HostOnly)
            continue;

        gVBoxAPI.UIHNInterface.GetStatus(networkInterface, &status);

        if (status == HostNetworkInterfaceStatus_Down)
            ret++;
    }

    gVBoxAPI.UArray.vboxArrayRelease(&networkInterfaces);

    VBOX_RELEASE(host);

    VIR_DEBUG("numActive: %d", ret);
    return ret;
}

int vboxConnectListDefinedNetworks(virConnectPtr conn, char **const names, int nnames)
{
    vboxGlobalData *data = conn->privateData;
    vboxArray networkInterfaces = VBOX_ARRAY_INITIALIZER;
    IHost *host = NULL;
    size_t i = 0;
    int ret = -1;

    if (!data->vboxObj)
        return ret;

    gVBoxAPI.UIVirtualBox.GetHost(data->vboxObj, &host);
    if (!host)
        return ret;

    gVBoxAPI.UArray.vboxArrayGet(&networkInterfaces, host,
                                 gVBoxAPI.UArray.handleHostGetNetworkInterfaces(host));

    ret = 0;
    for (i = 0; (ret < nnames) && (i < networkInterfaces.count); i++) {
        IHostNetworkInterface *networkInterface = networkInterfaces.items[i];
        PRUint32 interfaceType = 0;
        char *nameUtf8 = NULL;
        PRUnichar *nameUtf16 = NULL;
        PRUint32 status = HostNetworkInterfaceStatus_Unknown;

        if (!networkInterface)
            continue;

        gVBoxAPI.UIHNInterface.GetInterfaceType(networkInterface, &interfaceType);

        if (interfaceType != HostNetworkInterfaceType_HostOnly)
            continue;

        gVBoxAPI.UIHNInterface.GetStatus(networkInterface, &status);

        if (status != HostNetworkInterfaceStatus_Down)
            continue;

        gVBoxAPI.UIHNInterface.GetName(networkInterface, &nameUtf16);
        VBOX_UTF16_TO_UTF8(nameUtf16, &nameUtf8);

        VIR_DEBUG("nnames[%d]: %s", ret, nameUtf8);
        if (VIR_STRDUP(names[ret], nameUtf8) >= 0)
            ret++;

        VBOX_UTF8_FREE(nameUtf8);
        VBOX_UTF16_FREE(nameUtf16);
    }

    gVBoxAPI.UArray.vboxArrayRelease(&networkInterfaces);

    VBOX_RELEASE(host);

    return ret;
}

virNetworkPtr vboxNetworkLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
    vboxGlobalData *data = conn->privateData;
    PRUint32 interfaceType = 0;
    char *nameUtf8 = NULL;
    PRUnichar *nameUtf16 = NULL;
    IHostNetworkInterface *networkInterface = NULL;
    vboxIIDUnion iid;
    IHost *host = NULL;
    virNetworkPtr ret = NULL;

    if (!data->vboxObj)
        return ret;

    gVBoxAPI.UIVirtualBox.GetHost(data->vboxObj, &host);
    if (!host)
        return ret;

    VBOX_IID_INITIALIZE(&iid);
    vboxIIDFromUUID(&iid, uuid);

    /* TODO: "internal" networks are just strings and
     * thus can't do much with them
     */

    gVBoxAPI.UIHost.FindHostNetworkInterfaceById(host, &iid,
                                                 &networkInterface);
    if (!networkInterface)
        goto cleanup;

    gVBoxAPI.UIHNInterface.GetInterfaceType(networkInterface, &interfaceType);

    if (interfaceType != HostNetworkInterfaceType_HostOnly)
        goto cleanup;

    gVBoxAPI.UIHNInterface.GetName(networkInterface, &nameUtf16);
    VBOX_UTF16_TO_UTF8(nameUtf16, &nameUtf8);

    ret = virGetNetwork(conn, nameUtf8, uuid);

    VIR_DEBUG("Network Name: %s", nameUtf8);
    DEBUGIID("Network UUID", &iid);
    VBOX_UTF8_FREE(nameUtf8);
    VBOX_UTF16_FREE(nameUtf16);

 cleanup:
    VBOX_RELEASE(networkInterface);
    VBOX_RELEASE(host);
    vboxIIDUnalloc(&iid);
    return ret;
}

virNetworkPtr vboxNetworkLookupByName(virConnectPtr conn, const char *name)
{
    vboxGlobalData *data = conn->privateData;
    PRUnichar *nameUtf16 = NULL;
    IHostNetworkInterface *networkInterface = NULL;
    PRUint32 interfaceType = 0;
    unsigned char uuid[VIR_UUID_BUFLEN];
    vboxIIDUnion iid;
    IHost *host = NULL;
    virNetworkPtr ret = NULL;
    nsresult rc;

    if (!data->vboxObj)
        return ret;

    gVBoxAPI.UIVirtualBox.GetHost(data->vboxObj, &host);
    if (!host)
        return ret;

    VBOX_IID_INITIALIZE(&iid);
    VBOX_UTF8_TO_UTF16(name, &nameUtf16);

    gVBoxAPI.UIHost.FindHostNetworkInterfaceByName(host, nameUtf16, &networkInterface);

    if (!networkInterface)
        goto cleanup;

    gVBoxAPI.UIHNInterface.GetInterfaceType(networkInterface, &interfaceType);

    if (interfaceType != HostNetworkInterfaceType_HostOnly)
        goto cleanup;

    rc = gVBoxAPI.UIHNInterface.GetId(networkInterface, &iid);
    if (NS_FAILED(rc))
        goto cleanup;
    vboxIIDToUUID(&iid, uuid);
    ret = virGetNetwork(conn, name, uuid);

    VIR_DEBUG("Network Name: %s", name);
    DEBUGIID("Network UUID", &iid);
    vboxIIDUnalloc(&iid);

 cleanup:
    VBOX_RELEASE(networkInterface);
    VBOX_UTF16_FREE(nameUtf16);
    VBOX_RELEASE(host);
    return ret;
}

static PRUnichar *
vboxSocketFormatAddrUtf16(vboxGlobalData *data, virSocketAddrPtr addr)
{
    char *utf8 = NULL;
    PRUnichar *utf16 = NULL;

    utf8 = virSocketAddrFormat(addr);

    if (utf8 == NULL) {
        return NULL;
    }

    VBOX_UTF8_TO_UTF16(utf8, &utf16);
    VIR_FREE(utf8);

    return utf16;
}

static virNetworkPtr
vboxNetworkDefineCreateXML(virConnectPtr conn, const char *xml, bool start)
{
    vboxGlobalData *data = conn->privateData;
    PRUnichar *networkInterfaceNameUtf16 = NULL;
    char *networkInterfaceNameUtf8 = NULL;
    PRUnichar *networkNameUtf16 = NULL;
    char *networkNameUtf8 = NULL;
    IHostNetworkInterface *networkInterface = NULL;
    virNetworkDefPtr def = virNetworkDefParseString(xml);
    virNetworkIpDefPtr ipdef = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    vboxIIDUnion vboxnetiid;
    virSocketAddr netmask;
    IHost *host = NULL;
    virNetworkPtr ret = NULL;
    nsresult rc;

    if (!data->vboxObj)
        return ret;

    gVBoxAPI.UIVirtualBox.GetHost(data->vboxObj, &host);
    if (!host)
        return ret;

    VBOX_IID_INITIALIZE(&vboxnetiid);

    if ((!def) ||
        (def->forward.type != VIR_NETWORK_FORWARD_NONE) ||
        (def->nips == 0 || !def->ips))
        goto cleanup;

    /* Look for the first IPv4 IP address definition and use that.
     * If there weren't any IPv4 addresses, ignore the network (since it's
     * required below to have an IPv4 address)
    */
    ipdef = virNetworkDefGetIpByIndex(def, AF_INET, 0);
    if (!ipdef)
        goto cleanup;

    if (virNetworkIpDefNetmask(ipdef, &netmask) < 0)
        goto cleanup;

    /* the current limitation of hostonly network is that you can't
     * assign a name to it and it defaults to vboxnet*, for e.g:
     * vboxnet0, vboxnet1, etc. Also the UUID is assigned to it
     * automatically depending on the mac address and thus both
     * these paramters are ignored here for now.
     *
     * If the vbox is in 2.x and the def->name not equal to vboxnet0,
     * the function call will fail and the networkInterface set to
     * NULL. (We can't assign a new name to hostonly network, only
     * take the given name, say vboxnet0)
     */
    gVBoxAPI.UIHost.CreateHostOnlyNetworkInterface(data, host, def->name,
                                                   &networkInterface);

    if (!networkInterface)
        goto cleanup;

    gVBoxAPI.UIHNInterface.GetName(networkInterface, &networkInterfaceNameUtf16);
    if (!networkInterfaceNameUtf16)
        goto cleanup;

    VBOX_UTF16_TO_UTF8(networkInterfaceNameUtf16, &networkInterfaceNameUtf8);

    if (virAsprintf(&networkNameUtf8, "HostInterfaceNetworking-%s", networkInterfaceNameUtf8) < 0)
        goto cleanup;

    VBOX_UTF8_TO_UTF16(networkNameUtf8, &networkNameUtf16);

    /* Currently support only one dhcp server per network
     * with contigious address space from start to end
     */
    if ((ipdef->nranges >= 1) &&
        VIR_SOCKET_ADDR_VALID(&ipdef->ranges[0].start) &&
        VIR_SOCKET_ADDR_VALID(&ipdef->ranges[0].end)) {
        IDHCPServer *dhcpServer = NULL;

        gVBoxAPI.UIVirtualBox.FindDHCPServerByNetworkName(data->vboxObj,
                                                          networkNameUtf16,
                                                          &dhcpServer);
        if (!dhcpServer) {
            /* create a dhcp server */
            gVBoxAPI.UIVirtualBox.CreateDHCPServer(data->vboxObj,
                                                   networkNameUtf16,
                                                   &dhcpServer);
            VIR_DEBUG("couldn't find dhcp server so creating one");
        }
        if (dhcpServer) {
            PRUnichar *ipAddressUtf16 = NULL;
            PRUnichar *networkMaskUtf16 = NULL;
            PRUnichar *fromIPAddressUtf16 = NULL;
            PRUnichar *toIPAddressUtf16 = NULL;
            PRUnichar *trunkTypeUtf16 = NULL;

            ipAddressUtf16 = vboxSocketFormatAddrUtf16(data, &ipdef->address);
            networkMaskUtf16 = vboxSocketFormatAddrUtf16(data, &netmask);
            fromIPAddressUtf16 = vboxSocketFormatAddrUtf16(data, &ipdef->ranges[0].start);
            toIPAddressUtf16 = vboxSocketFormatAddrUtf16(data, &ipdef->ranges[0].end);

            if (ipAddressUtf16 == NULL || networkMaskUtf16 == NULL ||
                fromIPAddressUtf16 == NULL || toIPAddressUtf16 == NULL) {
                VBOX_UTF16_FREE(ipAddressUtf16);
                VBOX_UTF16_FREE(networkMaskUtf16);
                VBOX_UTF16_FREE(fromIPAddressUtf16);
                VBOX_UTF16_FREE(toIPAddressUtf16);
                VBOX_RELEASE(dhcpServer);
                goto cleanup;
            }

            VBOX_UTF8_TO_UTF16("netflt", &trunkTypeUtf16);

            gVBoxAPI.UIDHCPServer.SetEnabled(dhcpServer, PR_TRUE);

            gVBoxAPI.UIDHCPServer.SetConfiguration(dhcpServer,
                                                   ipAddressUtf16,
                                                   networkMaskUtf16,
                                                   fromIPAddressUtf16,
                                                   toIPAddressUtf16);

            if (start)
                gVBoxAPI.UIDHCPServer.Start(dhcpServer,
                                            networkNameUtf16,
                                            networkInterfaceNameUtf16,
                                            trunkTypeUtf16);

            VBOX_UTF16_FREE(ipAddressUtf16);
            VBOX_UTF16_FREE(networkMaskUtf16);
            VBOX_UTF16_FREE(fromIPAddressUtf16);
            VBOX_UTF16_FREE(toIPAddressUtf16);
            VBOX_UTF16_FREE(trunkTypeUtf16);
            VBOX_RELEASE(dhcpServer);
        }
    }

    if ((ipdef->nhosts >= 1) &&
        VIR_SOCKET_ADDR_VALID(&ipdef->hosts[0].ip)) {
        PRUnichar *ipAddressUtf16 = NULL;
        PRUnichar *networkMaskUtf16 = NULL;

        ipAddressUtf16 = vboxSocketFormatAddrUtf16(data, &ipdef->hosts[0].ip);
        networkMaskUtf16 = vboxSocketFormatAddrUtf16(data, &netmask);

        if (ipAddressUtf16 == NULL || networkMaskUtf16 == NULL) {
            VBOX_UTF16_FREE(ipAddressUtf16);
            VBOX_UTF16_FREE(networkMaskUtf16);
            goto cleanup;
        }

        /* Current drawback is that since EnableStaticIpConfig() sets
         * IP and enables the interface so even if the dhcpserver is not
         * started the interface is still up and running
         */
        gVBoxAPI.UIHNInterface.EnableStaticIPConfig(networkInterface,
                                                    ipAddressUtf16,
                                                    networkMaskUtf16);

        VBOX_UTF16_FREE(ipAddressUtf16);
        VBOX_UTF16_FREE(networkMaskUtf16);
    } else {
        gVBoxAPI.UIHNInterface.EnableDynamicIPConfig(networkInterface);
        gVBoxAPI.UIHNInterface.DHCPRediscover(networkInterface);
    }

    rc = gVBoxAPI.UIHNInterface.GetId(networkInterface, &vboxnetiid);
    if (NS_FAILED(rc))
        goto cleanup;
    vboxIIDToUUID(&vboxnetiid, uuid);
    DEBUGIID("Real Network UUID", &vboxnetiid);
    vboxIIDUnalloc(&vboxnetiid);
    ret = virGetNetwork(conn, networkInterfaceNameUtf8, uuid);

 cleanup:
    VIR_FREE(networkNameUtf8);
    VBOX_UTF16_FREE(networkNameUtf16);
    VBOX_RELEASE(networkInterface);
    VBOX_UTF8_FREE(networkInterfaceNameUtf8);
    VBOX_UTF16_FREE(networkInterfaceNameUtf16);
    VBOX_RELEASE(host);
    virNetworkDefFree(def);
    return ret;
}

virNetworkPtr vboxNetworkCreateXML(virConnectPtr conn, const char *xml)
{
    return vboxNetworkDefineCreateXML(conn, xml, true);
}

virNetworkPtr vboxNetworkDefineXML(virConnectPtr conn, const char *xml)
{
    return vboxNetworkDefineCreateXML(conn, xml, false);
}

static int
vboxNetworkUndefineDestroy(virNetworkPtr network, bool removeinterface)
{
    vboxGlobalData *data = network->conn->privateData;
    char *networkNameUtf8 = NULL;
    PRUnichar *networkInterfaceNameUtf16 = NULL;
    IHostNetworkInterface *networkInterface = NULL;
    PRUnichar *networkNameUtf16 = NULL;
    IDHCPServer *dhcpServer = NULL;
    PRUint32 interfaceType = 0;
    IHost *host = NULL;
    int ret = -1;

    if (!data->vboxObj)
        return ret;

    gVBoxAPI.UIVirtualBox.GetHost(data->vboxObj, &host);
    if (!host)
        return ret;

    /* Current limitation of the function for VirtualBox 2.2.* is
     * that you can't delete the default hostonly adaptor namely:
     * vboxnet0 and thus all this functions does is remove the
     * dhcp server configuration, but the network can still be used
     * by giving the machine static IP and also it will still
     * show up in the net-list in virsh
     */

    if (virAsprintf(&networkNameUtf8, "HostInterfaceNetworking-%s", network->name) < 0)
        goto cleanup;

    VBOX_UTF8_TO_UTF16(network->name, &networkInterfaceNameUtf16);

    gVBoxAPI.UIHost.FindHostNetworkInterfaceByName(host, networkInterfaceNameUtf16, &networkInterface);

    if (!networkInterface)
        goto cleanup;

    gVBoxAPI.UIHNInterface.GetInterfaceType(networkInterface, &interfaceType);

    if (interfaceType != HostNetworkInterfaceType_HostOnly)
        goto cleanup;

    if (gVBoxAPI.networkRemoveInterface && removeinterface) {
        vboxIIDUnion iid;
        IProgress *progress = NULL;
        nsresult rc;
        resultCodeUnion resultCode;

        VBOX_IID_INITIALIZE(&iid);
        rc = gVBoxAPI.UIHNInterface.GetId(networkInterface, &iid);

        if (NS_FAILED(rc))
            goto cleanup;

        gVBoxAPI.UIHost.RemoveHostOnlyNetworkInterface(host, &iid, &progress);
        vboxIIDUnalloc(&iid);

        if (!progress)
            goto cleanup;

        gVBoxAPI.UIProgress.WaitForCompletion(progress, -1);
        gVBoxAPI.UIProgress.GetResultCode(progress, &resultCode);
        if (RC_FAILED(resultCode)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Error while removing hostonly network interface, rc=%08x"),
                           resultCode.uResultCode);
            goto cleanup;
        }
        VBOX_RELEASE(progress);
    }

    VBOX_UTF8_TO_UTF16(networkNameUtf8, &networkNameUtf16);

    gVBoxAPI.UIVirtualBox.FindDHCPServerByNetworkName(data->vboxObj,
                                                      networkNameUtf16,
                                                      &dhcpServer);
    if (!dhcpServer)
        goto cleanup;

    gVBoxAPI.UIDHCPServer.SetEnabled(dhcpServer, PR_FALSE);
    gVBoxAPI.UIDHCPServer.Stop(dhcpServer);
    if (removeinterface)
        gVBoxAPI.UIVirtualBox.RemoveDHCPServer(data->vboxObj, dhcpServer);
    ret = 0;
    VBOX_RELEASE(dhcpServer);

 cleanup:
    VBOX_UTF16_FREE(networkNameUtf16);
    VBOX_RELEASE(networkInterface);
    VBOX_UTF16_FREE(networkInterfaceNameUtf16);
    VBOX_RELEASE(host);
    VIR_FREE(networkNameUtf8);
    return ret;
}

int vboxNetworkUndefine(virNetworkPtr network)
{
    return vboxNetworkUndefineDestroy(network, true);
}

int vboxNetworkDestroy(virNetworkPtr network)
{
    return vboxNetworkUndefineDestroy(network, false);
}