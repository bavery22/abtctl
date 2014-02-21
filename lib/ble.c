/*
 *  Android BLE Library -- Provides utility functions to control BLE features
 *
 *  Copyright (C) 2013 João Paulo Rechi Vita
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation; either version 2.1 of the
 *  the Free Software Foundation; either version 2 of the License, or
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <hardware/bluetooth.h>
#include <hardware/bt_gatt.h>
#include <hardware/bt_gatt_client.h>
#include <hardware/hardware.h>

#include "ble.h"

/* Internal representation of a GATT characteristic */
typedef struct ble_gatt_char ble_gatt_char_t;
struct ble_gatt_char {
    btgatt_srvc_id_t s;
    btgatt_char_id_t c;
};

/* Internal representation of a GATT descriptor */
typedef struct ble_gatt_desc ble_gatt_desc_t;
struct ble_gatt_desc {
    ble_gatt_char_t c;
    bt_uuid_t d;
};

typedef enum {
    BLE_GATT_ELEM_SERVICE,
    BLE_GATT_ELEM_CHARACTERISTIC,
    BLE_GATT_ELEM_DESCRIPTOR
} gatt_elem_t;

/* Internal representation of a BLE device */
typedef struct ble_device ble_device_t;
struct ble_device {
    bt_bdaddr_t bda;
    int conn_id;

    btgatt_srvc_id_t *srvcs;
    uint8_t srvc_count;
    ble_gatt_char_t *chars;
    uint8_t char_count;
    ble_gatt_desc_t *descs;
    uint8_t desc_count;

    uint8_t write_prepared;
    gatt_elem_t prep_write_type;
    uint8_t prep_write_id;

    ble_device_t *next;
};

/* Data that have to be acessable by the callbacks */
static struct libdata {
    ble_cbs_t cbs;

    uint8_t btiface_initialized;
    const bt_interface_t *btiface;
    const btgatt_interface_t *gattiface;
    int client;

    uint8_t adapter_state;
    uint8_t scan_state;
    ble_device_t *devices;
} data;

/* Called every time an advertising report is seen */
static void scan_result_cb(bt_bdaddr_t *bda, int rssi, uint8_t *adv_data) {
    if (data.cbs.scan_cb)
        data.cbs.scan_cb(bda->address, rssi, adv_data);
}

static int ble_scan(uint8_t start) {
    bt_status_t s;

    if (!data.client)
        return -1;

    if (data.gattiface == NULL)
        return -1;

    if (!data.adapter_state)
        return -1;

    if (data.scan_state == start)
        return 1;

    s = data.gattiface->client->scan(data.client, start);
    if (s != BT_STATUS_SUCCESS)
        return -s;

    data.scan_state = start;

    return 0;
}

int ble_start_scan() {
    return ble_scan(1);
}

int ble_stop_scan() {
    return ble_scan(0);
}

static ble_device_t *find_device_by_address(const uint8_t *address) {
    ble_device_t *dev;

    for (dev = data.devices; dev ; dev = dev->next)
        if (!memcmp(dev->bda.address, address, sizeof(dev->bda.address)))
            break;

    return dev;
}

/* Called every time a device gets connected */
static void connect_cb(int conn_id, int status, int client_if,
                       bt_bdaddr_t *bda) {
    ble_device_t *dev;

    dev = find_device_by_address(bda->address);
    if (!dev)
        return;

    dev->conn_id = conn_id;

    if (data.cbs.connect_cb)
        data.cbs.connect_cb(bda->address, conn_id, status);
}

int ble_connect(const uint8_t *address) {
    ble_device_t *dev;
    bt_status_t s;

    if (!data.client)
        return -1;

    if (!data.gattiface)
        return -1;

    if (!data.adapter_state)
        return -1;

    dev = find_device_by_address(address);
    if (!dev) {
        int i;

        dev = calloc(1, sizeof(ble_device_t));
        if (!dev)
            return -1;

        /* TODO: memcpy() */
        for (i = 0; i < 6; i++)
            dev->bda.address[i] = address[i];

        dev->next = data.devices;
        data.devices = dev;
    }

    s = data.gattiface->client->connect(data.client, &dev->bda, true);
    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

/* Called every time a device gets disconnected */
static void disconnect_cb(int conn_id, int status, int client_if,
                          bt_bdaddr_t *bda) {
    ble_device_t *dev;

    dev = find_device_by_address(bda->address);
    if (!dev)
        return;

    dev->conn_id = 0;

    if (data.cbs.disconnect_cb)
        data.cbs.disconnect_cb(bda->address, conn_id, status);
}

int ble_disconnect(const uint8_t *address) {
    ble_device_t *dev;
    bt_status_t s;

    if (!data.client)
        return -1;

    if (!data.gattiface)
        return -1;

    if (!data.adapter_state)
        return -1;

    if (!address)
        return -1;

    dev = find_device_by_address(address);
    if (!dev)
        return -1;

    s = data.gattiface->client->disconnect(data.client, &dev->bda,
                                           dev->conn_id);
    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

/* Called every time the bond state with a device changes */
static void bond_state_changed_cb(bt_status_t status, bt_bdaddr_t *bda,
                                  bt_bond_state_t state) {
    ble_bond_state_t s;
    ble_device_t *dev;

    switch (state) {
        case BT_BOND_STATE_NONE:
            s = BLE_BOND_NONE;
            break;
        case BT_BOND_STATE_BONDING:
            s = BLE_BOND_BONDING;
            break;
        case BT_BOND_STATE_BONDED:
            s = BLE_BOND_BONDED;
            break;
        default:
            return;
    }

    dev = find_device_by_address(bda->address);
    if (dev && data.cbs.bond_state_cb)
        data.cbs.bond_state_cb(bda->address, s, status);
}

static int ble_pair_internal(const uint8_t *address, uint8_t operation) {
    ble_device_t *dev;
    bt_status_t s = BT_STATUS_UNSUPPORTED;

    if (!data.btiface)
        return -1;

    if (!data.adapter_state)
        return -1;

    dev = find_device_by_address(address);
    if (!dev) {
        int i;

        dev = calloc(1, sizeof(ble_device_t));
        if (!dev)
            return -1;

        /* TODO: memcpy() */
        for (i = 0; i < 6; i++)
            dev->bda.address[i] = address[i];

        dev->next = data.devices;
        data.devices = dev;
    }

    switch (operation) {
        case 0: /* Pair */
            s = data.btiface->create_bond(&dev->bda);
            break;
        case 1: /* Cancel pairing */
            s = data.btiface->cancel_bond(&dev->bda);
            break;
        case 2: /* Remove bond */
            s = data.btiface->remove_bond(&dev->bda);
            break;
    }

    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

int ble_pair(const uint8_t *address) {
    return ble_pair_internal(address, 0);
}

int ble_cancel_pairing(const uint8_t *address) {
    return ble_pair_internal(address, 1);
}

int ble_remove_bond(const uint8_t *address) {
    return ble_pair_internal(address, 2);
}

static ble_device_t *find_device_by_conn_id(int conn_id) {
    ble_device_t *dev;

    for (dev = data.devices; dev ; dev = dev->next)
        if (dev->conn_id == conn_id)
            break;

    return dev;
}

/* Called in response of a read remote RSSI operation */
void read_remote_rssi_cb(int client_if, bt_bdaddr_t *bda, int rssi,
                         int status) {
    ble_device_t *dev;
    int conn_id = -1;

    if (!status) {
        dev = find_device_by_address(bda->address);
        if (dev)
            conn_id = dev->conn_id;
    }

    if (data.cbs.rssi_cb)
        data.cbs.rssi_cb(conn_id, rssi, status);
}

int ble_read_remote_rssi(int conn_id) {
    ble_device_t *dev;
    bt_status_t s;

    if (!data.client)
        return -1;

    if (conn_id <= 0)
        return -1;

    if (!data.gattiface)
        return -1;

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return -1;

    s = data.gattiface->client->read_remote_rssi(data.client, &dev->bda);
    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

static int find_service(ble_device_t *dev, btgatt_srvc_id_t *srvc_id) {
    int id;

    for (id = 0; id < dev->srvc_count; id++)
        if (!memcmp(&dev->srvcs[id], srvc_id, sizeof(btgatt_srvc_id_t)))
            return id;

    return -1;
}

/* Called when the service discovery finishes */
void service_discovery_complete_cb(int conn_id, int status) {
    if (data.cbs.srvc_finished_cb)
        data.cbs.srvc_finished_cb(conn_id, status);
}

/* Called for each service discovery result */
void service_discovery_result_cb(int conn_id, btgatt_srvc_id_t *srvc_id) {
    int id;
    ble_device_t *dev;

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return;

    id = find_service(dev, srvc_id);
    if (id < 0) {
        id = dev->srvc_count++;
        dev->srvcs = realloc(dev->srvcs,
                             dev->srvc_count * sizeof(btgatt_srvc_id_t));
        memcpy(&dev->srvcs[id], srvc_id, sizeof(btgatt_srvc_id_t));
    }

    if (data.cbs.srvc_found_cb)
        data.cbs.srvc_found_cb(conn_id, id, srvc_id->id.uuid.uu,
                               srvc_id->is_primary);
}

int ble_gatt_discover_services(int conn_id, const uint8_t *uuid) {
    bt_status_t s;
    bt_uuid_t uu, *u = NULL;

    if (conn_id <= 0)
        return -1;

    if (!data.gattiface)
        return -1;

    if (uuid) {
        memcpy(uu.uu, uuid, 16 * sizeof(uint8_t));
        u = &uu;
    }

    s = data.gattiface->client->search_service(conn_id, u);
    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

static void get_included_service_cb(int conn_id, int status, btgatt_srvc_id_t *srvc_id, btgatt_srvc_id_t *incl_srvc_id) {
    ble_device_t *dev;
    int id;

    if (status != 0)
        return;

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return;

    id = find_service(dev, incl_srvc_id);
    if (id < 0)
        return;

    if (data.cbs.srvc_found_cb)
        data.cbs.srvc_found_cb(conn_id, id, incl_srvc_id->id.uuid.uu,
                               incl_srvc_id->is_primary);

    data.gattiface->client->get_included_service(conn_id, srvc_id,
                                                 incl_srvc_id);
}

int ble_gatt_get_included_services(int conn_id, int service_id) {
    ble_device_t *dev;
    bt_status_t s;

    if (conn_id <= 0)
        return -1;

    if (!data.gattiface)
        return -1;

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return -1;

    if (dev->srvc_count <= 0)
        return -1;

    if (service_id < 0 || service_id >= dev->srvc_count)
        return -1;

    s = data.gattiface->client->get_included_service(conn_id,
                                                     &dev->srvcs[service_id],
                                                     NULL);
    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

static int find_characteristic(ble_device_t *dev, btgatt_srvc_id_t *srvc_id,
                               btgatt_char_id_t *char_id) {
    int id;

    for (id = 0; id < dev->char_count; id++)
        if (!memcmp(&dev->chars[id].c, char_id, sizeof(btgatt_char_id_t)) &&
            !memcmp(&dev->chars[id].s, srvc_id, sizeof(btgatt_srvc_id_t)))
            return id;

    return -1;
}

/* Called for each characteristic discovery result */
static void characteristic_discovery_cb(int conn_id, int status,
                                        btgatt_srvc_id_t *srvc_id,
                                        btgatt_char_id_t *char_id,
                                        int char_prop) {
    ble_device_t *dev;
    int id;
    bt_status_t s;

    if (status != 0) {
        if (data.cbs.char_finished_cb)
            data.cbs.char_finished_cb(conn_id, status);
        return;
    }

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return;

    id = find_characteristic(dev, srvc_id, char_id);
    if (id < 0) {
        id = dev->char_count++;
        dev->chars = realloc(dev->chars,
                             dev->char_count * sizeof(ble_gatt_char_t));
        memcpy(&dev->chars[id].s, srvc_id, sizeof(btgatt_srvc_id_t));
        memcpy(&dev->chars[id].c, char_id, sizeof(btgatt_char_id_t));
    }

    if (data.cbs.char_found_cb)
        data.cbs.char_found_cb(conn_id, id, char_id->uuid.uu, char_prop);

    /* Get next characteristic */
    s = data.gattiface->client->get_characteristic(conn_id, srvc_id, char_id);
    if (s != BT_STATUS_SUCCESS)
        if (data.cbs.char_finished_cb)
            data.cbs.char_finished_cb(conn_id, status);
}

int ble_gatt_discover_characteristics(int conn_id, int service_id) {
    ble_device_t *dev;
    bt_status_t s;

    if (conn_id <= 0){
      fprintf(stderr,"%s:Bad conn_id\n",__FUNCTION__);
      return -1;
    }
    if (!data.gattiface){
      fprintf(stderr,"%s:Bad gatt_iface\n",__FUNCTION__);
      return -1;
    }
    dev = find_device_by_conn_id(conn_id);
    if (!dev){
      fprintf(stderr,"%s:Bad dev\n",__FUNCTION__);
      return -1;
    }

    if (dev->srvc_count <= 0){
      fprintf(stderr,"%s:Bad srvc_count <= 0\n",__FUNCTION__);
      return -1;
    }

    if (service_id < 0 || service_id >= dev->srvc_count){
      fprintf(stderr,"%s:Bad service_id=%d \n",__FUNCTION__,service_id);
        return -1;
    }
    s = data.gattiface->client->get_characteristic(conn_id,
                                                   &dev->srvcs[service_id],
                                                   NULL);
    if (s != BT_STATUS_SUCCESS){
      fprintf(stderr,"%s:Bad return from client->get_char return = %d\n",__FUNCTION__,s);
      return -s;
    }
    return 0;
}

static int find_descriptor(ble_device_t *dev, btgatt_srvc_id_t *srvc_id,
                           btgatt_char_id_t *char_id, bt_uuid_t *descr_id) {
    int id;

    for (id = 0; id < dev->desc_count; id++)
        if (!memcmp(&dev->descs[id].d, descr_id, sizeof(bt_uuid_t)) &&
            !memcmp(&dev->descs[id].c.c, char_id, sizeof(btgatt_char_id_t)) &&
            !memcmp(&dev->descs[id].c.s, srvc_id, sizeof(btgatt_srvc_id_t)))
            return id;

    return -1;
}

/* Called for each descriptor discovery result */
static void descriptor_discovery_cb(int conn_id, int status,
                                    btgatt_srvc_id_t *srvc_id,
                                    btgatt_char_id_t *char_id,
                                    bt_uuid_t *descr_id) {
    ble_device_t *dev;
    int id;
    bt_status_t s;

    if (status != 0) {
        if (data.cbs.desc_finished_cb)
            data.cbs.desc_finished_cb(conn_id, status);
        return;
    }

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return;

    id = find_descriptor(dev, srvc_id, char_id, descr_id);
    if (id < 0) {
        id = dev->desc_count++;
        dev->descs = realloc(dev->descs,
                             dev->desc_count * sizeof(ble_gatt_desc_t));
        memcpy(&dev->descs[id].c.s, srvc_id, sizeof(btgatt_srvc_id_t));
        memcpy(&dev->descs[id].c.c, char_id, sizeof(btgatt_char_id_t));
        memcpy(&dev->descs[id].d, descr_id, sizeof(bt_uuid_t));
    }

    if (data.cbs.desc_found_cb)
      data.cbs.desc_found_cb(conn_id, id, descr_id->uu, char_id->uuid.uu,0);

    /* Get next descriptor */
    s = data.gattiface->client->get_descriptor(conn_id, srvc_id, char_id,
                                               descr_id);
    if (s != BT_STATUS_SUCCESS)
        if (data.cbs.desc_finished_cb)
            data.cbs.desc_finished_cb(conn_id, status);
}

int ble_gatt_discover_descriptors(int conn_id, int char_id) {
    ble_device_t *dev;
    bt_status_t s;

    if (conn_id <= 0)
        return -1;

    if (!data.gattiface)
        return -1;

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return -1;

    if (dev->char_count <= 0)
        return -1;

    if (char_id < 0 || char_id >= dev->char_count)
        return -1;

    s = data.gattiface->client->get_descriptor(conn_id, &dev->chars[char_id].s,
                                               &dev->chars[char_id].c, NULL);
    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

/* Called when a GATT read characteristic operation returns */
void read_characteristic_cb(int conn_id, int status,
                            btgatt_read_params_t *p_data) {
    ble_device_t *dev;
    int id = -1;

    dev = find_device_by_conn_id(conn_id);
    if (dev)
        id = find_characteristic(dev, &p_data->srvc_id, &p_data->char_id);

    if (data.cbs.char_read_cb)
        data.cbs.char_read_cb(conn_id, id, p_data->value.value,
                              p_data->value.len, p_data->value_type, status);
}

/* Called when a GATT read descriptor operation returns */
static void read_descriptor_cb(int conn_id, int status,
                               btgatt_read_params_t *p_data) {
    ble_device_t *dev;
    int id = -1;

    dev = find_device_by_conn_id(conn_id);
    if (dev)
        id = find_descriptor(dev, &p_data->srvc_id, &p_data->char_id,
                             &p_data->descr_id);

    if (data.cbs.desc_read_cb)
        data.cbs.desc_read_cb(conn_id, id, p_data->value.value,
                              p_data->value.len, p_data->value_type, status);
}

/* Called when a GATT write characteristic operation returns */
static void write_characteristic_cb(int conn_id, int status,
                                    btgatt_write_params_t *p_data) {
    ble_device_t *dev;
    int id = -1;

    dev = find_device_by_conn_id(conn_id);
    if (dev)
        id = find_characteristic(dev, &p_data->srvc_id, &p_data->char_id);

    if (data.cbs.char_write_cb)
        data.cbs.char_write_cb(conn_id, id, NULL, 0, 0, status);
}

/* Called when a GATT write descriptor operation returns */
static void write_descriptor_cb(int conn_id, int status,
                                btgatt_write_params_t *p_data) {
    ble_device_t *dev;
    int id = -1;

    dev = find_device_by_conn_id(conn_id);
    if (dev)
        id = find_descriptor(dev, &p_data->srvc_id, &p_data->char_id,
                             &p_data->descr_id);

    if (data.cbs.desc_write_cb)
        data.cbs.desc_write_cb(conn_id, id, NULL, 0, 0, status);
}

static void execute_write_cb(int conn_id, int status) {
    ble_device_t *dev;

    dev = find_device_by_conn_id(conn_id);
    if (!dev || !dev->write_prepared)
        return;

    if (dev->prep_write_type == BLE_GATT_ELEM_CHARACTERISTIC &&
        data.cbs.char_write_cb)
        data.cbs.char_write_cb(conn_id, dev->prep_write_id, NULL, 0, 0, status);
    else if (dev->prep_write_type == BLE_GATT_ELEM_DESCRIPTOR &&
        data.cbs.desc_write_cb)
        data.cbs.desc_write_cb(conn_id, dev->prep_write_id, NULL, 0, 0, status);
}

static int ble_gatt_op(int operation, int conn_id, int id, int auth,
                       const char *value, int len) {
    ble_device_t *dev;
    bt_status_t s = BT_STATUS_UNSUPPORTED;

    if (id < 0)
        return -1;

    if (conn_id <= 0)
        return -1;

    if (!data.gattiface)
        return -1;

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return -1;

    switch (operation) {
        case 0: /* Read characteristic */
            if (dev->char_count <= 0)
                return -1;
            if (id >= dev->char_count)
                return -1;

            s = data.gattiface->client->read_characteristic(conn_id,
	                                                    &dev->chars[id].s,
	                                                    &dev->chars[id].c,
                                                            auth);
            break;

        case 1: /* Read descriptor */
            if (dev->desc_count <= 0)
                return -1;
            if (id >= dev->desc_count)
                return -1;

	    s = data.gattiface->client->read_descriptor(conn_id,
	                                                &dev->descs[id].c.s,
							&dev->descs[id].c.c,
							&dev->descs[id].d,
                                                        auth);
            break;

        case 4: /* Write characteristic with prepare write */
            dev->write_prepared = 1;
            dev->prep_write_type = BLE_GATT_ELEM_CHARACTERISTIC;
            dev->prep_write_id = id;
            /* pass-through */

        case 2: /* Write characteristic with write command */
        case 3: /* Write characteristic with write request */
            if (dev->char_count <= 0 || id >= dev->char_count)
                return -1;

            s = data.gattiface->client->write_characteristic(conn_id,
	                                                     &dev->chars[id].s,
	                                                     &dev->chars[id].c,
                                                             operation-1, len,
                                                             auth,
                                                             (char *) value);
            break;

        case 7: /* Write descriptor with prepare write */
            dev->write_prepared = 1;
            dev->prep_write_type = BLE_GATT_ELEM_DESCRIPTOR;
            dev->prep_write_id = id;
            /* pass-through */

        case 5: /* Write descriptor with write command */
        case 6: /* Write descriptor with write request */
            if (dev->desc_count <= 0 || id >= dev->desc_count)
                return -1;

	    s = data.gattiface->client->write_descriptor(conn_id,
	                                                 &dev->descs[id].c.s,
							 &dev->descs[id].c.c,
							 &dev->descs[id].d,
                                                         operation-4, len,
                                                         auth, (char *) value);
            break;

        case 8:
            if (id == 0) /* Cancel prepared write */
                dev->write_prepared = 0;
            s = data.gattiface->client->execute_write(conn_id, id);
            break;
    }

    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

int ble_gatt_read_char(int conn_id, int char_id, int auth) {
    return ble_gatt_op(0, conn_id, char_id, auth, NULL, 0);
}

int ble_gatt_read_desc(int conn_id, int desc_id, int auth) {
    return ble_gatt_op(1, conn_id, desc_id, auth, NULL, 0);
}

int ble_gatt_write_cmd_char(int conn_id, int char_id, int auth,
                            const char *value, int len) {
    return ble_gatt_op(2, conn_id, char_id, auth, value, len);
}

int ble_gatt_write_req_char(int conn_id, int char_id, int auth,
                            const char *value, int len) {
    return ble_gatt_op(3, conn_id, char_id, auth, value, len);
}

int ble_gatt_write_cmd_desc(int conn_id, int desc_id, int auth,
                            const char *value, int len) {
    return ble_gatt_op(5, conn_id, desc_id, auth, value, len);
}

int ble_gatt_write_req_desc(int conn_id, int desc_id, int auth,
                            const char *value, int len) {
    return ble_gatt_op(6, conn_id, desc_id, auth, value, len);
}

int ble_gatt_prep_write_char(int conn_id, int char_id, int auth,
                             const char *value, int len) {
    return ble_gatt_op(4, conn_id, char_id, auth, value, len);
}

int ble_gatt_prep_write_desc(int conn_id, int desc_id, int auth,
                             const char *value, int len) {
    return ble_gatt_op(7, conn_id, desc_id, auth, value, len);
}

int ble_gatt_execute_write(int conn_id, int execute) {
    return ble_gatt_op(8, conn_id, execute, 0, NULL, 0);
}

/* Called when the registration for notifications on a char finishes */
static void register_for_notification_cb(int conn_id, int registered,
                                         int status,
                                         btgatt_srvc_id_t *srvc_id,
                                         btgatt_char_id_t *char_id) {
    int id = 0;

    if (data.cbs.char_notification_register_cb)
        data.cbs.char_notification_register_cb(conn_id, id, registered, status);
}

/* Called when notifications of a characteristic are received */
void notify_cb(int conn_id, btgatt_notify_params_t *p_data) {
    int id = 0;

    if (data.cbs.char_notification_cb)
      data.cbs.char_notification_cb(conn_id, id, p_data->char_id.uuid.uu,p_data->value, p_data->len,
                                      !p_data->is_notify);
}

static int ble_gatt_char_notification(uint8_t operation, int conn_id,
                                      int char_id) {
    ble_device_t *dev;
    bt_status_t s = BT_STATUS_UNSUPPORTED;
    btgatt_srvc_id_t *srvc;
    btgatt_char_id_t *ch;

    if (char_id < 0)
        return -1;

    if (!data.client)
        return -1;

    if (!data.gattiface)
        return -1;

    if (!data.adapter_state)
        return -1;

    dev = find_device_by_conn_id(conn_id);
    if (!dev)
        return -1;

    if (dev->char_count <= 0 || char_id >= dev->char_count)
        return -1;

    srvc = &dev->chars[char_id].s;
    ch = &dev->chars[char_id].c;

    fprintf(stderr,"operation = %d\n",operation);
    switch (operation) {
        case 0:
            s = data.gattiface->client->register_for_notification(data.client,
                                                                  &dev->bda,
                                                                  srvc, ch);
            break;
        case 1:
            s = data.gattiface->client->deregister_for_notification(data.client,
                                                                    &dev->bda,
                                                                    srvc, ch);
            break;
    }

    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}

int ble_gatt_register_char_notification(int conn_id, int char_id) {
    return ble_gatt_char_notification(0, conn_id, char_id);
}

int ble_gatt_unregister_char_notification(int conn_id, int char_id) {
    return ble_gatt_char_notification(1, conn_id, char_id);
}

/* Called when the client registration is finished */
static void register_client_cb(int status, int client_if, bt_uuid_t *app_uuid) {
    if (status == BT_STATUS_SUCCESS) {
        data.client = client_if;
        if (data.cbs.enable_cb)
            data.cbs.enable_cb();
    } else
        data.client = 0;
}

/* GATT client interface callbacks */
static const btgatt_client_callbacks_t gattccbs = {
    register_client_cb,
    scan_result_cb,
    connect_cb,
    disconnect_cb,
    service_discovery_complete_cb,
    service_discovery_result_cb,
    characteristic_discovery_cb,
    descriptor_discovery_cb,
    get_included_service_cb,
    register_for_notification_cb,
    notify_cb,
    read_characteristic_cb,
    write_characteristic_cb,
    read_descriptor_cb,
    write_descriptor_cb,
    execute_write_cb,
    read_remote_rssi_cb
};

/* GATT interface callbacks */
static const btgatt_callbacks_t gattcbs = {
    sizeof(btgatt_callbacks_t),
    &gattccbs,
    NULL  /* btgatt_server_callbacks_t */
};

/* Called every time the adapter state changes */
static void adapter_state_changed_cb(bt_state_t state) {
    /* Arbitrary UUID used to identify this application with the GATT profile */
    bt_uuid_t app_uuid = { .uu = { 0x1b, 0x1c, 0xb9, 0x2e, 0x0d, 0x2e, 0x4c,
                                   0x45, 0xbb, 0xb8, 0xf4, 0x1b, 0x46, 0x39,
                                   0x23, 0x36 } };

    data.adapter_state = state == BT_STATE_ON ? 1 : 0;
    if (data.cbs.adapter_state_cb)
        data.cbs.adapter_state_cb(data.adapter_state);

    if (data.adapter_state) {
        bt_status_t s = data.gattiface->client->register_client(&app_uuid);
        if (s != BT_STATUS_SUCCESS)
            data.btiface->disable();
    } else
        /* Cleanup the Bluetooth interface */
        data.btiface->cleanup();
}

/* This callback is called when the stack finishes initialization / shutdown */
static void thread_event_cb(bt_cb_thread_evt event) {
    bt_status_t s;

    if (event == DISASSOCIATE_JVM) {
        data.btiface_initialized = 0;
        return;
    }

    data.btiface_initialized = 1;

    data.gattiface = data.btiface->get_profile_interface(BT_PROFILE_GATT_ID);
    if (data.gattiface != NULL) {
        s = data.gattiface->init(&gattcbs);
        if (s != BT_STATUS_SUCCESS)
            data.gattiface = NULL;
    }

    /* Enable the adapter */
    s = data.btiface->enable();
    if (s != BT_STATUS_SUCCESS)
        data.btiface->cleanup();
}

/* Bluetooth interface callbacks */
static bt_callbacks_t btcbs = {
    sizeof(bt_callbacks_t),
    adapter_state_changed_cb,
    NULL, /* adapter_properties_cb */
    NULL, /* remote_device_properties_callback */
    NULL, /* device_found_cb */
    NULL, /* discovery_state_changed_cb */
    NULL, /* pin_request_cb */
    NULL, /* ssp_request_cb */
    bond_state_changed_cb,
    NULL, /* acl_state_changed_callback */
    thread_event_cb,
    NULL, /* dut_mode_recv_callback */
    NULL, /* le_test_mode_callback */
};

int ble_enable(ble_cbs_t cbs) {
    int status;
    bt_status_t s;
    hw_module_t *module;
    hw_device_t *hwdev;
    bluetooth_device_t *btdev;

    memset(&data, 0, sizeof(data));

    /* Get the Bluetooth module from libhardware */
    status = hw_get_module(BT_STACK_MODULE_ID, (hw_module_t const**) &module);
    if (status < 0)
        return status;

    /* Get the Bluetooth hardware device */
    status = module->methods->open(module, BT_STACK_MODULE_ID, &hwdev);
    if (status < 0)
        return status;

    /* Get the Bluetooth interface */
    btdev = (bluetooth_device_t *) hwdev;
    data.btiface = btdev->get_bluetooth_interface();
    if (data.btiface == NULL)
        return -1;

    /* Init the Bluetooth interface, setting a callback for each operation */
    s = data.btiface->init(&btcbs);
    if (s != BT_STATUS_SUCCESS && s != BT_STATUS_DONE)
        return -s;

    /* Store the user callbacks */
    data.cbs = cbs;

    return 0;
}

static void remove_all_devices() {
    ble_device_t *dev, *next;

    dev = data.devices;
    while (dev) {
        next = dev->next;

        free(dev->srvcs);
        free(dev->chars);
        free(dev->descs);
        free(dev);

        dev = next;
    }
}

int ble_disable() {
    bt_status_t s;

    if (!data.adapter_state)
        return -1;

    if (!data.btiface)
        return -1;

    if (!data.client)
        return -1;

    s = data.gattiface->client->unregister_client(data.client);
    if (s != BT_STATUS_SUCCESS)
        return -s;

    s = data.btiface->disable();
    if (s != BT_STATUS_SUCCESS)
        return -s;

    return 0;
}
