/*
 *  Android Bluetooth Control tool
 *
 *  Copyright (C) 2013 João Paulo Rechi Vita
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hardware/bluetooth.h>
#include <hardware/hardware.h>

#define MAX_LINE_SIZE 64

/* Data that have to be acessable by the callbacks */
struct userdata {
    const bt_interface_t *btiface;
    uint8_t btiface_initialized;
    uint8_t quit;
    bt_state_t adapter_state; /* The adapter is always OFF in the beginning */
    bt_discovery_state_t discovery_state;
} u;

/* Prints the command prompt */
static void cmd_prompt() {
    printf("> ");
    fflush(stdout);
}

/* Struct that defines a user command */
typedef struct cmd {
    const char *name;
    const char *description;
    void (*handler)(char *args);
} cmd_t;

/* Clean blanks until a non-blank is found */
static void line_skip_blanks(char **line) {
    while (**line == ' ')
        (*line)++;
}

/* Parses a sub-string out of a string */
static void line_get_str(char **line, char *str) {
  line_skip_blanks(line);

  while (**line != 0 && **line != ' ') {
        *str = **line;
        (*line)++;
        str++;
  }

  *str = 0;
}

static void cmd_quit(char *args) {
    u.quit = 1;
}

/* Called every time the adapter state changes */
static void adapter_state_change_cb(bt_state_t state) {
    u.adapter_state = state;
    printf("\nAdapter state changed: %i\n", state);
    cmd_prompt();
}

/* Enables the Bluetooth adapter */
static void cmd_enable(char *args) {
    int status;

    if (u.adapter_state == BT_STATE_ON) {
        printf("Bluetooth is already enabled\n");
        return;
    }

    status = u.btiface->enable();
    if (status != BT_STATUS_SUCCESS)
        printf("Failed to enable Bluetooth\n");
}

/* Disables the Bluetooth adapter */
static void cmd_disable(char *args) {
    int status;

    if (u.adapter_state == BT_STATE_OFF) {
        printf("Bluetooth is already disabled\n");
        return;
    }

    status = u.btiface->disable();
    if (status != BT_STATUS_SUCCESS)
        printf("Failed to disable Bluetooth\n");
}

static void device_found_cb(int num_properties, bt_property_t *properties) {

    printf("\nDevice found\n");

    while (num_properties--) {
        bt_property_t prop = properties[num_properties];

        switch (prop.type) {
            const char *addr;

            case BT_PROPERTY_BDNAME:
                printf("  name: %s\n", (const char *) prop.val);
                break;

            case BT_PROPERTY_BDADDR:
                addr = (const char *) prop.val;
                printf("  addr: %02X:%02X:%02X:%02X:%02X:%02X\n", addr[0],
                       addr[1], addr[2], addr[3], addr[4], addr[5]);
                break;

            case BT_PROPERTY_CLASS_OF_DEVICE:
                printf("  class: 0x%x\n", ((uint32_t *) prop.val)[0]);
                break;

            case BT_PROPERTY_TYPE_OF_DEVICE:
                switch ( ((bt_device_type_t *) prop.val)[0] ) {
                    case BT_DEVICE_DEVTYPE_BREDR:
                        printf("  type: BR/EDR only\n");
                        break;
                    case BT_DEVICE_DEVTYPE_BLE:
                        printf("  type: LE only\n");
                        break;
                    case BT_DEVICE_DEVTYPE_DUAL:
                        printf("  type: DUAL MODE\n");
                        break;
                }
                break;

            case BT_PROPERTY_REMOTE_FRIENDLY_NAME:
                printf("  alias: %s\n", (const char *) prop.val);
                break;

            case BT_PROPERTY_REMOTE_RSSI:
                printf("  rssi: %i\n", ((uint8_t *) prop.val)[0]);
                break;

            case BT_PROPERTY_REMOTE_VERSION_INFO:
                printf("  version info:\n");
                printf("    version: %d\n", ((bt_remote_version_t *) prop.val)->version);
                printf("    subversion: %d\n", ((bt_remote_version_t *) prop.val)->sub_ver);
                printf("    manufacturer: %d\n", ((bt_remote_version_t *) prop.val)->manufacturer);
                break;

            default:
                printf("  Unknown property type:%i len:%i val:%p\n", prop.type, prop.len, prop.val);
                break;
        }
    }

    cmd_prompt();
}

static void discovery_state_changed_cb(bt_discovery_state_t state) {
    u.discovery_state == state;
    printf("\nDiscovery state changed: %i\n", state);
    cmd_prompt();
}

static void cmd_discovery(char *args) {
    bt_status_t status;
    char arg[MAX_LINE_SIZE];

    line_get_str(&args, arg);

    if (arg[0] == 0 || strcmp(arg, "help") == 0) {
        printf("discovery -- Controls discovery of nearby devices\n");
        printf("Arguments:\n");
        printf("start   starts a new discovery session\n");
        printf("stop    interrupts an ongoing discovery session\n");

    } else if (strcmp(arg, "start") == 0) {

        if (u.adapter_state != BT_STATE_ON) {
            printf("Unable to start discovery: Adapter is down\n");
            return;
        }

        if (u.discovery_state == BT_DISCOVERY_STARTED) {
            printf("Discovery is already running\n");
            return;
        }

        status = u.btiface->start_discovery();
        if (status != BT_STATUS_SUCCESS)
            printf("Failed to start discovery\n");

    } else if (strcmp(arg, "stop") == 0) {

        if (u.discovery_state == BT_DISCOVERY_STOPPED) {
            printf("Unable to stop discovery: Discovery is not running\n");
            return;
        }

        status = u.btiface->cancel_discovery();
        if (status != BT_STATUS_SUCCESS)
            printf("Failed to stop discovery\n");

    } else
        printf("Invalid argument \"%s\"\n", arg);
}

/* List of available user commands */
static const cmd_t cmd_list[] = {
    { "quit", "        Exits", cmd_quit },
    { "enable", "      Enables the Bluetooth adapter", cmd_enable },
    { "disable", "     Disables the Bluetooth adapter", cmd_disable },
    { "discovery", "   Controls discovery of nearby devices", cmd_discovery },
    { NULL, NULL, NULL }
};

/* Parses a command and calls the respective handler */
static void cmd_process(char *line) {
    char cmd[MAX_LINE_SIZE];
    int i;

    if (line[0] == 0)
        return;

    line_get_str(&line, cmd);

    if (strcmp(cmd, "help") == 0) {
        for (i = 0; cmd_list[i].name != NULL; i++)
            printf("%s %s\n", cmd_list[i].name, cmd_list[i].description);
        return;
    }

    for (i = 0; cmd_list[i].name != NULL; i++)
        if (strcmp(cmd, cmd_list[i].name) == 0) {
            cmd_list[i].handler(line);
            return;
        }

    printf("%s: unknown command, use 'help' for a list of available commands\n", cmd);
}

/* This callback is used by the thread that handles Bluetooth interface (btif)
 * to send events for its users. At the moment there are two events defined:
 *
 *     ASSOCIATE_JVM: sinalizes the btif handler thread is ready;
 *     DISASSOCIATE_JVM: sinalizes the btif handler thread is about to exit.
 *
 * This events are used by the JNI to know when the btif handler thread should
 * be associated or dessociated with the JVM
 */
static void thread_event_cb(bt_cb_thread_evt event) {
    printf("\nBluetooth interface %s\n", event == ASSOCIATE_JVM ? "ready" : "finished");
    if (event == ASSOCIATE_JVM) {
        u.btiface_initialized = 1;
        cmd_prompt();
    } else
        u.btiface_initialized = 0;
}

/* Bluetooth interface callbacks */
static bt_callbacks_t btcbs = {
    sizeof(bt_callbacks_t),
    adapter_state_change_cb, /* Called every time the adapter state changes */
    NULL, /* adapter_properties_callback */
    NULL, /* remote_device_properties_callback */
    device_found_cb, /* Called for every device found */
    discovery_state_changed_cb, /* Called every time the discovery state changes */
    NULL, /* pin_request_callback */
    NULL, /* ssp_request_callback */
    NULL, /* bond_state_changed_callback */
    NULL, /* acl_state_changed_callback */
    thread_event_cb, /* Called when the JVM is associated / dissociated */
    NULL, /* dut_mode_recv_callback */
    NULL, /* le_test_mode_callback */
};

/* Initialize the Bluetooth stack */
static void bt_init() {
    int status;
    hw_module_t *module;
    hw_device_t *hwdev;
    bluetooth_device_t *btdev;

    u.btiface_initialized = 0;
    u.quit = 0;
    u.adapter_state = BT_STATE_OFF; /* The adapter is OFF in the beginning */

    /* Get the Bluetooth module from libhardware */
    status = hw_get_module(BT_STACK_MODULE_ID, (hw_module_t const**) &module);
    if (status < 0) {
        errno = status;
        err(1, "Failed to get the Bluetooth module");
    }
    printf("Bluetooth stack infomation:\n");
    printf("    id = %s\n", module->id);
    printf("    name = %s\n", module->name);
    printf("    author = %s\n", module->author);
    printf("    HAL API version = %d\n", module->hal_api_version);

    /* Get the Bluetooth hardware device */
    status = module->methods->open(module, BT_STACK_MODULE_ID, &hwdev);
    if (status < 0) {
        errno = status;
        err(2, "Failed to get the Bluetooth hardware device");
    }
    printf("Bluetooth device infomation:\n");
    printf("    API version = %d\n", hwdev->version);

    /* Get the Bluetooth interface */
    btdev = (bluetooth_device_t *) hwdev;
    u.btiface = btdev->get_bluetooth_interface();
    if (u.btiface == NULL)
        err(3, "Failed to get the Bluetooth interface");

    /* Init the Bluetooth interface, setting a callback for each operation */
    status = u.btiface->init(&btcbs);
    if (status != BT_STATUS_SUCCESS && status != BT_STATUS_DONE)
        err(4, "Failed to initialize the Bluetooth interface");
}

int main (int argc, char * argv[]) {
    char line[MAX_LINE_SIZE];

    printf("Android Bluetooth control tool version 0.1\n");

    bt_init();

    cmd_prompt();

    while (!u.quit && fgets(line, MAX_LINE_SIZE, stdin)) {
        /* remove linefeed */
        line[strlen(line)-1] = 0;

        cmd_process(line);

        if (!u.quit)
            cmd_prompt();
    }

    /* Cleanup the Bluetooth interface */
    printf("Processing Bluetooth interface cleanup");
    u.btiface->cleanup();
    while (u.btiface_initialized)
        usleep(10000);

    printf("\n");
    return 0;
}
