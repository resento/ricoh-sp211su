/*
 * SANE backend for Ricoh Aficio SP 211SU Multifunction Printer / Scanner
 * Native Primax 32-bit USB Bulk Protocol implementation over Interface 1.
 * Supports Grayscale (8-bit) and Full Color (24-bit interleaved RGB).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>
#include <sane/sane.h>
#include <sane/saneopts.h>

#define VENDOR_ID   0x05ca
#define PRODUCT_ID  0x044f

#define USB_INTERFACE 1
#define EP_OUT        0x02
#define EP_IN         0x82

#define MAX_WIDTH_MM  216.0
#define MAX_HEIGHT_MM 297.0

#define USB_CHUNK_SIZE 32768 /* 32 KB, multiple of 512 bytes */

/* Level correction constants (derived from CIS sensor calibration) */
#define BLACK_LEVEL       30
#define WHITE_LEVEL_GRAY  190
#define WHITE_LEVEL_R     193
#define WHITE_LEVEL_G     184
#define WHITE_LEVEL_B     205

static uint8_t lut_gray[256];
static uint8_t lut_r[256];
static uint8_t lut_g[256];
static uint8_t lut_b[256];
static int luts_initialized = 0;

static void init_luts(void)
{
    if (luts_initialized) return;
    for (int i = 0; i < 256; i++) {
        /* Gray */
        if (i <= BLACK_LEVEL) lut_gray[i] = 0;
        else if (i >= WHITE_LEVEL_GRAY) lut_gray[i] = 255;
        else lut_gray[i] = (uint8_t)(((i - BLACK_LEVEL) * 255 + (WHITE_LEVEL_GRAY - BLACK_LEVEL) / 2) / (WHITE_LEVEL_GRAY - BLACK_LEVEL));

        /* Red */
        if (i <= BLACK_LEVEL) lut_r[i] = 0;
        else if (i >= WHITE_LEVEL_R) lut_r[i] = 255;
        else lut_r[i] = (uint8_t)(((i - BLACK_LEVEL) * 255 + (WHITE_LEVEL_R - BLACK_LEVEL) / 2) / (WHITE_LEVEL_R - BLACK_LEVEL));

        /* Green */
        if (i <= BLACK_LEVEL) lut_g[i] = 0;
        else if (i >= WHITE_LEVEL_G) lut_g[i] = 255;
        else lut_g[i] = (uint8_t)(((i - BLACK_LEVEL) * 255 + (WHITE_LEVEL_G - BLACK_LEVEL) / 2) / (WHITE_LEVEL_G - BLACK_LEVEL));

        /* Blue */
        if (i <= BLACK_LEVEL) lut_b[i] = 0;
        else if (i >= WHITE_LEVEL_B) lut_b[i] = 255;
        else lut_b[i] = (uint8_t)(((i - BLACK_LEVEL) * 255 + (WHITE_LEVEL_B - BLACK_LEVEL) / 2) / (WHITE_LEVEL_B - BLACK_LEVEL));
    }
    luts_initialized = 1;
}

typedef enum {
    OPT_NUM_OPTS = 0,
    OPT_MODE_GROUP,
    OPT_MODE,
    OPT_RESOLUTION,
    OPT_GEOMETRY_GROUP,
    OPT_TL_X,
    OPT_TL_Y,
    OPT_BR_X,
    OPT_BR_Y,
    NUM_OPTIONS
} OptionIndex;

static const SANE_String_Const mode_list[] = {
    SANE_VALUE_SCAN_MODE_COLOR,
    SANE_VALUE_SCAN_MODE_GRAY,
    NULL
};

static const SANE_Int resolution_list[] = {
    5,    /* count */
    100,
    150,
    200,
    300,
    600
};

static const SANE_Range x_range = {
    SANE_FIX(0.0),
    SANE_FIX(MAX_WIDTH_MM),
    SANE_FIX(0.0)
};

static const SANE_Range y_range = {
    SANE_FIX(0.0),
    SANE_FIX(MAX_HEIGHT_MM),
    SANE_FIX(0.0)
};

typedef struct {
    libusb_context *usb_ctx;
    libusb_device_handle *dev_handle;

    SANE_Option_Descriptor opt[NUM_OPTIONS];

    char mode[32];
    SANE_Int resolution;
    SANE_Fixed tl_x;
    SANE_Fixed tl_y;
    SANE_Fixed br_x;
    SANE_Fixed br_y;

    SANE_Parameters params;
    size_t sensor_bytes_expected;
    size_t sensor_bytes_read;
    size_t frontend_bytes_expected;
    size_t frontend_bytes_read;
    int is_color;
    int scanning;
    int cancelled;

    /* USB FIFO buffer */
    uint8_t usb_buf[USB_CHUNK_SIZE];
    size_t usb_buf_pos;
    size_t usb_buf_len;
} RicohDevice;

static libusb_context *global_usb_ctx = NULL;

static SANE_Device s_sane_device = {
    "sp211su",
    "Ricoh",
    "SP 211SU",
    "flatbed scanner"
};

static const SANE_Device *s_device_list[2] = {
    &s_sane_device,
    NULL
};

static const SANE_Device *s_empty_device_list[1] = {
    NULL
};

void sane_ricoh_sp211su_cancel(SANE_Handle handle);

static void send_cleanup(RicohDevice *dev)
{
    if (!dev || !dev->dev_handle) return;
    /* Clean closing sequence: Finish (0x1E), Stop (5), DriverClose (0x0D), Unlock (0x24) */
    uint32_t cmds[] = { 0x1E, 5, 0x0D, 0x24 };
    int transferred = 0;
    for (int i = 0; i < 4; i++) {
        libusb_bulk_transfer(dev->dev_handle, EP_OUT, (unsigned char*)&cmds[i], 4, &transferred, 1000);
    }
    libusb_clear_halt(dev->dev_handle, EP_OUT);
    libusb_clear_halt(dev->dev_handle, EP_IN);
    libusb_release_interface(dev->dev_handle, USB_INTERFACE);
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_init(SANE_Int *version_code, SANE_Auth_Callback authorize)
{
    (void)authorize;
    if (version_code) {
        *version_code = SANE_VERSION_CODE(1, 0, 0);
    }
    if (!global_usb_ctx) {
        if (libusb_init(&global_usb_ctx) != 0) {
            return SANE_STATUS_IO_ERROR;
        }
    }
    init_luts();
    return SANE_STATUS_GOOD;
}

__attribute__((visibility("default")))
void sane_ricoh_sp211su_exit(void)
{
    if (global_usb_ctx) {
        libusb_exit(global_usb_ctx);
        global_usb_ctx = NULL;
    }
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_get_devices(const SANE_Device ***device_list, SANE_Bool local_only)
{
    (void)local_only;
    if (!device_list) return SANE_STATUS_INVAL;

    if (!global_usb_ctx) {
        if (libusb_init(&global_usb_ctx) != 0) {
            *device_list = s_empty_device_list;
            return SANE_STATUS_GOOD;
        }
    }

    libusb_device_handle *h = libusb_open_device_with_vid_pid(global_usb_ctx, VENDOR_ID, PRODUCT_ID);
    if (h) {
        libusb_close(h);
        *device_list = s_device_list;
    } else {
        *device_list = s_empty_device_list;
    }
    return SANE_STATUS_GOOD;
}

static void init_options(RicohDevice *dev)
{
    /* OPT_NUM_OPTS */
    dev->opt[OPT_NUM_OPTS].name = SANE_NAME_NUM_OPTIONS;
    dev->opt[OPT_NUM_OPTS].title = SANE_TITLE_NUM_OPTIONS;
    dev->opt[OPT_NUM_OPTS].desc = SANE_DESC_NUM_OPTIONS;
    dev->opt[OPT_NUM_OPTS].type = SANE_TYPE_INT;
    dev->opt[OPT_NUM_OPTS].unit = SANE_UNIT_NONE;
    dev->opt[OPT_NUM_OPTS].size = sizeof(SANE_Word);
    dev->opt[OPT_NUM_OPTS].cap = SANE_CAP_SOFT_DETECT;

    /* OPT_MODE_GROUP */
    dev->opt[OPT_MODE_GROUP].title = SANE_I18N("Scan Mode");
    dev->opt[OPT_MODE_GROUP].desc = SANE_I18N("Scan mode settings");
    dev->opt[OPT_MODE_GROUP].type = SANE_TYPE_GROUP;
    dev->opt[OPT_MODE_GROUP].cap = 0;

    /* OPT_MODE */
    dev->opt[OPT_MODE].name = SANE_NAME_SCAN_MODE;
    dev->opt[OPT_MODE].title = SANE_TITLE_SCAN_MODE;
    dev->opt[OPT_MODE].desc = SANE_DESC_SCAN_MODE;
    dev->opt[OPT_MODE].type = SANE_TYPE_STRING;
    dev->opt[OPT_MODE].unit = SANE_UNIT_NONE;
    dev->opt[OPT_MODE].size = 32;
    dev->opt[OPT_MODE].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    dev->opt[OPT_MODE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
    dev->opt[OPT_MODE].constraint.string_list = mode_list;

    /* OPT_RESOLUTION */
    dev->opt[OPT_RESOLUTION].name = SANE_NAME_SCAN_RESOLUTION;
    dev->opt[OPT_RESOLUTION].title = SANE_TITLE_SCAN_RESOLUTION;
    dev->opt[OPT_RESOLUTION].desc = SANE_DESC_SCAN_RESOLUTION;
    dev->opt[OPT_RESOLUTION].type = SANE_TYPE_INT;
    dev->opt[OPT_RESOLUTION].unit = SANE_UNIT_DPI;
    dev->opt[OPT_RESOLUTION].size = sizeof(SANE_Word);
    dev->opt[OPT_RESOLUTION].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    dev->opt[OPT_RESOLUTION].constraint_type = SANE_CONSTRAINT_WORD_LIST;
    dev->opt[OPT_RESOLUTION].constraint.word_list = resolution_list;

    /* OPT_GEOMETRY_GROUP */
    dev->opt[OPT_GEOMETRY_GROUP].title = SANE_I18N("Geometry");
    dev->opt[OPT_GEOMETRY_GROUP].desc = SANE_I18N("Scan area");
    dev->opt[OPT_GEOMETRY_GROUP].type = SANE_TYPE_GROUP;
    dev->opt[OPT_GEOMETRY_GROUP].cap = 0;

    /* OPT_TL_X */
    dev->opt[OPT_TL_X].name = SANE_NAME_SCAN_TL_X;
    dev->opt[OPT_TL_X].title = SANE_TITLE_SCAN_TL_X;
    dev->opt[OPT_TL_X].desc = SANE_DESC_SCAN_TL_X;
    dev->opt[OPT_TL_X].type = SANE_TYPE_FIXED;
    dev->opt[OPT_TL_X].unit = SANE_UNIT_MM;
    dev->opt[OPT_TL_X].size = sizeof(SANE_Word);
    dev->opt[OPT_TL_X].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    dev->opt[OPT_TL_X].constraint_type = SANE_CONSTRAINT_RANGE;
    dev->opt[OPT_TL_X].constraint.range = &x_range;

    /* OPT_TL_Y */
    dev->opt[OPT_TL_Y].name = SANE_NAME_SCAN_TL_Y;
    dev->opt[OPT_TL_Y].title = SANE_TITLE_SCAN_TL_Y;
    dev->opt[OPT_TL_Y].desc = SANE_DESC_SCAN_TL_Y;
    dev->opt[OPT_TL_Y].type = SANE_TYPE_FIXED;
    dev->opt[OPT_TL_Y].unit = SANE_UNIT_MM;
    dev->opt[OPT_TL_Y].size = sizeof(SANE_Word);
    dev->opt[OPT_TL_Y].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    dev->opt[OPT_TL_Y].constraint_type = SANE_CONSTRAINT_RANGE;
    dev->opt[OPT_TL_Y].constraint.range = &y_range;

    /* OPT_BR_X */
    dev->opt[OPT_BR_X].name = SANE_NAME_SCAN_BR_X;
    dev->opt[OPT_BR_X].title = SANE_TITLE_SCAN_BR_X;
    dev->opt[OPT_BR_X].desc = SANE_DESC_SCAN_BR_X;
    dev->opt[OPT_BR_X].type = SANE_TYPE_FIXED;
    dev->opt[OPT_BR_X].unit = SANE_UNIT_MM;
    dev->opt[OPT_BR_X].size = sizeof(SANE_Word);
    dev->opt[OPT_BR_X].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    dev->opt[OPT_BR_X].constraint_type = SANE_CONSTRAINT_RANGE;
    dev->opt[OPT_BR_X].constraint.range = &x_range;

    /* OPT_BR_Y */
    dev->opt[OPT_BR_Y].name = SANE_NAME_SCAN_BR_Y;
    dev->opt[OPT_BR_Y].title = SANE_TITLE_SCAN_BR_Y;
    dev->opt[OPT_BR_Y].desc = SANE_DESC_SCAN_BR_Y;
    dev->opt[OPT_BR_Y].type = SANE_TYPE_FIXED;
    dev->opt[OPT_BR_Y].unit = SANE_UNIT_MM;
    dev->opt[OPT_BR_Y].size = sizeof(SANE_Word);
    dev->opt[OPT_BR_Y].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    dev->opt[OPT_BR_Y].constraint_type = SANE_CONSTRAINT_RANGE;
    dev->opt[OPT_BR_Y].constraint.range = &y_range;
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_open(SANE_String_Const devicename, SANE_Handle *handle)
{
    (void)devicename;
    if (!handle) return SANE_STATUS_INVAL;

    if (!global_usb_ctx) {
        if (libusb_init(&global_usb_ctx) != 0) {
            return SANE_STATUS_IO_ERROR;
        }
    }

    libusb_device_handle *h = libusb_open_device_with_vid_pid(global_usb_ctx, VENDOR_ID, PRODUCT_ID);
    if (!h) {
        return SANE_STATUS_INVAL;
    }

    RicohDevice *dev = (RicohDevice*)calloc(1, sizeof(RicohDevice));
    if (!dev) {
        libusb_close(h);
        return SANE_STATUS_NO_MEM;
    }

    dev->usb_ctx = global_usb_ctx;
    dev->dev_handle = h;
    strncpy(dev->mode, SANE_VALUE_SCAN_MODE_COLOR, sizeof(dev->mode) - 1);
    dev->resolution = 300;
    dev->tl_x = SANE_FIX(0.0);
    dev->tl_y = SANE_FIX(0.0);
    dev->br_x = SANE_FIX(MAX_WIDTH_MM);
    dev->br_y = SANE_FIX(MAX_HEIGHT_MM);

    init_options(dev);

    *handle = (SANE_Handle)dev;
    return SANE_STATUS_GOOD;
}

__attribute__((visibility("default")))
void sane_ricoh_sp211su_close(SANE_Handle handle)
{
    RicohDevice *dev = (RicohDevice*)handle;
    if (!dev) return;

    if (dev->scanning) {
        sane_ricoh_sp211su_cancel(handle);
    }
    if (dev->dev_handle) {
        libusb_close(dev->dev_handle);
        dev->dev_handle = NULL;
    }
    free(dev);
}

__attribute__((visibility("default")))
const SANE_Option_Descriptor *sane_ricoh_sp211su_get_option_descriptor(SANE_Handle handle, SANE_Int option)
{
    RicohDevice *dev = (RicohDevice*)handle;
    if (!dev || option < 0 || option >= NUM_OPTIONS) return NULL;
    return &dev->opt[option];
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_control_option(SANE_Handle handle, SANE_Int option,
                                       SANE_Action action, void *val, SANE_Int *info)
{
    RicohDevice *dev = (RicohDevice*)handle;
    if (!dev) return SANE_STATUS_INVAL;

    if (info) *info = 0;

    if (action == SANE_ACTION_GET_VALUE) {
        switch (option) {
            case OPT_NUM_OPTS:
                *(SANE_Word*)val = NUM_OPTIONS;
                break;
            case OPT_MODE:
                strcpy((char*)val, dev->mode);
                break;
            case OPT_RESOLUTION:
                *(SANE_Word*)val = dev->resolution;
                break;
            case OPT_TL_X:
                *(SANE_Word*)val = dev->tl_x;
                break;
            case OPT_TL_Y:
                *(SANE_Word*)val = dev->tl_y;
                break;
            case OPT_BR_X:
                *(SANE_Word*)val = dev->br_x;
                break;
            case OPT_BR_Y:
                *(SANE_Word*)val = dev->br_y;
                break;
            default:
                return SANE_STATUS_INVAL;
        }
        return SANE_STATUS_GOOD;
    } else if (action == SANE_ACTION_SET_VALUE) {
        switch (option) {
            case OPT_MODE:
                strncpy(dev->mode, (char*)val, sizeof(dev->mode) - 1);
                if (info) *info |= SANE_INFO_RELOAD_PARAMS;
                break;
            case OPT_RESOLUTION:
                dev->resolution = *(SANE_Word*)val;
                if (info) *info |= SANE_INFO_RELOAD_PARAMS;
                break;
            case OPT_TL_X:
                dev->tl_x = *(SANE_Word*)val;
                if (info) *info |= SANE_INFO_RELOAD_PARAMS;
                break;
            case OPT_TL_Y:
                dev->tl_y = *(SANE_Word*)val;
                if (info) *info |= SANE_INFO_RELOAD_PARAMS;
                break;
            case OPT_BR_X:
                dev->br_x = *(SANE_Word*)val;
                if (info) *info |= SANE_INFO_RELOAD_PARAMS;
                break;
            case OPT_BR_Y:
                dev->br_y = *(SANE_Word*)val;
                if (info) *info |= SANE_INFO_RELOAD_PARAMS;
                break;
            default:
                return SANE_STATUS_INVAL;
        }
        return SANE_STATUS_GOOD;
    }
    return SANE_STATUS_INVAL;
}

static void compute_params(RicohDevice *dev, int *w_px, int *h_px, int *off_x, int *off_y)
{
    double mm_w = SANE_UNFIX(dev->br_x) - SANE_UNFIX(dev->tl_x);
    double mm_h = SANE_UNFIX(dev->br_y) - SANE_UNFIX(dev->tl_y);
    double mm_ox = SANE_UNFIX(dev->tl_x);
    double mm_oy = SANE_UNFIX(dev->tl_y);

    if (mm_w <= 0.0 || mm_w > MAX_WIDTH_MM) mm_w = MAX_WIDTH_MM;
    if (mm_h <= 0.0 || mm_h > MAX_HEIGHT_MM) mm_h = MAX_HEIGHT_MM;
    if (mm_ox < 0.0) mm_ox = 0.0;
    if (mm_oy < 0.0) mm_oy = 0.0;

    *w_px = (int)((mm_w * dev->resolution) / 25.4);
    *h_px = (int)((mm_h * dev->resolution) / 25.4);
    *off_x = (int)((mm_ox * dev->resolution) / 25.4);
    *off_y = (int)((mm_oy * dev->resolution) / 25.4);

    /* Width and offset must be multiple of 8 pixels for hardware CIS */
    *w_px = *w_px & ~7;
    *off_x = *off_x & ~7;
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_get_parameters(SANE_Handle handle, SANE_Parameters *params)
{
    RicohDevice *dev = (RicohDevice*)handle;
    if (!dev || !params) return SANE_STATUS_INVAL;

    int w, h, ox, oy;
    compute_params(dev, &w, &h, &ox, &oy);

    dev->is_color = (strcmp(dev->mode, SANE_VALUE_SCAN_MODE_COLOR) == 0);

    if (dev->is_color) {
        params->format = SANE_FRAME_RGB;
        params->depth = 8;
        params->pixels_per_line = w;
        params->bytes_per_line = w * 3;
    } else {
        params->format = SANE_FRAME_GRAY;
        params->depth = 8;
        params->pixels_per_line = w;
        params->bytes_per_line = w;
    }

    params->last_frame = SANE_TRUE;
    params->lines = h;

    dev->params = *params;
    return SANE_STATUS_GOOD;
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_start(SANE_Handle handle)
{
    RicohDevice *dev = (RicohDevice*)handle;
    if (!dev || !dev->dev_handle) return SANE_STATUS_INVAL;

    init_luts();

    int w, h, ox, oy;
    compute_params(dev, &w, &h, &ox, &oy);

    sane_ricoh_sp211su_get_parameters(handle, &dev->params);

    int channels = dev->is_color ? 3 : 1;

    dev->cancelled = 0;
    dev->sensor_bytes_expected = (size_t)w * h * channels;
    dev->sensor_bytes_read = 0;

    dev->frontend_bytes_expected = (size_t)w * h * channels;
    dev->frontend_bytes_read = 0;

    dev->usb_buf_pos = 0;
    dev->usb_buf_len = 0;

    libusb_claim_interface(dev->dev_handle, USB_INTERFACE);
    libusb_clear_halt(dev->dev_handle, EP_OUT);
    libusb_clear_halt(dev->dev_handle, EP_IN);

    int transferred = 0;
    uint32_t cmd;

    /* 1. Wake up scanner (ActiveScanner = 50 / 0x32) */
    cmd = 0x32;
    int r = libusb_bulk_transfer(dev->dev_handle, EP_OUT, (unsigned char*)&cmd, 4, &transferred, 2000);
    if (r != 0) {
        libusb_clear_halt(dev->dev_handle, EP_OUT);
        r = libusb_bulk_transfer(dev->dev_handle, EP_OUT, (unsigned char*)&cmd, 4, &transferred, 2000);
        if (r != 0) {
            libusb_release_interface(dev->dev_handle, USB_INTERFACE);
            return SANE_STATUS_IO_ERROR;
        }
    }

    /* 2. Poll GetStatus until Ready (st[3] == 0x01) with up to 6s timeout */
    int is_ready = 0;
    for (int retry = 0; retry < 30; retry++) {
        usleep(200000); /* 200ms */
        cmd = 1; /* GetStatus */
        if (libusb_bulk_transfer(dev->dev_handle, EP_OUT, (unsigned char*)&cmd, 4, &transferred, 2000) == 0) {
            unsigned char st[8];
            if (libusb_bulk_transfer(dev->dev_handle, EP_IN, st, 8, &transferred, 2000) == 0 && transferred == 8) {
                if (st[3] == 0x01) {
                    is_ready = 1;
                    break;
                }
            }
        }
    }

    if (!is_ready) {
        usleep(500000);
    }

    /* 3. Set Parameters (SetParameter = 3) */
    uint8_t param[32];
    memset(param, 0, 32);
    param[0] = 1; /* source: 1 = flatbed */
    param[1] = 1;
    param[2] = dev->is_color ? 2 : 1; /* mode: 1 = Gray, 2 = Color */
    *(uint16_t*)(param + 0x04) = (uint16_t)dev->resolution;
    *(uint16_t*)(param + 0x08) = (uint16_t)dev->resolution;
    *(uint32_t*)(param + 0x0C) = (uint32_t)w;
    *(uint32_t*)(param + 0x10) = (uint32_t)h;
    *(uint16_t*)(param + 0x14) = (uint16_t)ox;
    *(uint16_t*)(param + 0x18) = (uint16_t)oy;
    param[0x1C] = 8; /* 8 bits per channel */

    cmd = 3;
    r = libusb_bulk_transfer(dev->dev_handle, EP_OUT, (unsigned char*)&cmd, 4, &transferred, 2000);
    if (r != 0) {
        libusb_release_interface(dev->dev_handle, USB_INTERFACE);
        return SANE_STATUS_IO_ERROR;
    }
    r = libusb_bulk_transfer(dev->dev_handle, EP_OUT, param, 32, &transferred, 2000);
    if (r != 0) {
        libusb_release_interface(dev->dev_handle, USB_INTERFACE);
        return SANE_STATUS_IO_ERROR;
    }

    /* 4. Send tone curve (gamma): opcode 9 + 0x0400 length, then 3072 linear bytes */
    uint8_t gamma_hdr[6] = { 0x09, 0x00, 0x00, 0x00, 0x00, 0x04 };
    r = libusb_bulk_transfer(dev->dev_handle, EP_OUT, gamma_hdr, 6, &transferred, 2000);
    if (r == 0) {
        uint8_t linear_curve[3072];
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < 1024; i++) {
                linear_curve[c * 1024 + i] = (uint8_t)(i >> 2);
            }
        }
        libusb_bulk_transfer(dev->dev_handle, EP_OUT, linear_curve, 3072, &transferred, 2000);
    }

    usleep(500000); /* 500ms delay */

    /* 5. Start Scan (StartScan = 4) */
    cmd = 4;
    r = libusb_bulk_transfer(dev->dev_handle, EP_OUT, (unsigned char*)&cmd, 4, &transferred, 2000);
    if (r != 0) {
        libusb_release_interface(dev->dev_handle, USB_INTERFACE);
        return SANE_STATUS_IO_ERROR;
    }

    dev->scanning = 1;
    return SANE_STATUS_GOOD;
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_read(SANE_Handle handle, SANE_Byte *data, SANE_Int maxlen, SANE_Int *length)
{
    RicohDevice *dev = (RicohDevice*)handle;
    if (!dev || !dev->scanning) return SANE_STATUS_INVAL;

    *length = 0;

    if (dev->cancelled) {
        dev->scanning = 0;
        return SANE_STATUS_CANCELLED;
    }

    if (dev->frontend_bytes_read >= dev->frontend_bytes_expected) {
        dev->scanning = 0;
        send_cleanup(dev);
        return SANE_STATUS_EOF;
    }

    int bytes_produced = 0;

    while (bytes_produced < maxlen && dev->frontend_bytes_read < dev->frontend_bytes_expected) {
        /* If our USB FIFO buffer is empty, fetch another 32 KB chunk from EP_IN */
        if (dev->usb_buf_pos >= dev->usb_buf_len) {
            if (dev->sensor_bytes_read >= dev->sensor_bytes_expected) {
                break;
            }

            dev->usb_buf_pos = 0;
            dev->usb_buf_len = 0;

            int transferred = 0;
            int r = libusb_bulk_transfer(dev->dev_handle, EP_IN,
                                         dev->usb_buf, USB_CHUNK_SIZE,
                                         &transferred, 25000);

            if (r == LIBUSB_SUCCESS && transferred > 0) {
                dev->usb_buf_len = transferred;
                dev->sensor_bytes_read += transferred;
            } else if (r == LIBUSB_ERROR_TIMEOUT) {
                if (bytes_produced > 0) {
                    *length = bytes_produced;
                    return SANE_STATUS_GOOD;
                }
                continue;
            } else {
                if (bytes_produced > 0) {
                    *length = bytes_produced;
                    return SANE_STATUS_GOOD;
                }
                dev->scanning = 0;
                send_cleanup(dev);
                return SANE_STATUS_IO_ERROR;
            }
        }

        /* Calculate how many bytes we can consume right now */
        size_t avail = dev->usb_buf_len - dev->usb_buf_pos;
        size_t needed = maxlen - bytes_produced;
        size_t remaining_fe = dev->frontend_bytes_expected - dev->frontend_bytes_read;
        size_t to_copy = (avail < needed) ? avail : needed;
        if (to_copy > remaining_fe) to_copy = remaining_fe;

        if (to_copy == 0) break;

        /* Copy through level correction LUT */
        if (!dev->is_color) {
            for (size_t i = 0; i < to_copy; i++) {
                data[bytes_produced + i] = lut_gray[dev->usb_buf[dev->usb_buf_pos + i]];
            }
        } else {
            for (size_t i = 0; i < to_copy; i++) {
                uint8_t raw = dev->usb_buf[dev->usb_buf_pos + i];
                int ch = (dev->frontend_bytes_read + i) % 3;
                if (ch == 0) data[bytes_produced + i] = lut_r[raw];
                else if (ch == 1) data[bytes_produced + i] = lut_g[raw];
                else data[bytes_produced + i] = lut_b[raw];
            }
        }

        dev->usb_buf_pos += to_copy;
        bytes_produced += to_copy;
        dev->frontend_bytes_read += to_copy;
    }

    *length = bytes_produced;
    if (bytes_produced > 0) {
        return SANE_STATUS_GOOD;
    }

    if (dev->frontend_bytes_read >= dev->frontend_bytes_expected) {
        dev->scanning = 0;
        send_cleanup(dev);
        return SANE_STATUS_EOF;
    }

    return SANE_STATUS_GOOD;
}

__attribute__((visibility("default")))
void sane_ricoh_sp211su_cancel(SANE_Handle handle)
{
    RicohDevice *dev = (RicohDevice*)handle;
    if (!dev) return;

    dev->cancelled = 1;
    if (dev->dev_handle) {
        uint32_t cmd = 0x0C; /* Cancel opcode */
        int transferred = 0;
        libusb_bulk_transfer(dev->dev_handle, EP_OUT, (unsigned char*)&cmd, 4, &transferred, 1000);
        uint8_t drain[512];
        for (int i = 0; i < 10; i++) {
            if (libusb_bulk_transfer(dev->dev_handle, EP_IN, drain, sizeof(drain), &transferred, 50) != 0 || transferred == 0) {
                break;
            }
        }
        send_cleanup(dev);
    }
    dev->scanning = 0;
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_set_io_mode(SANE_Handle handle, SANE_Bool non_blocking)
{
    (void)handle;
    (void)non_blocking;
    return SANE_STATUS_UNSUPPORTED;
}

__attribute__((visibility("default")))
SANE_Status sane_ricoh_sp211su_get_select_fd(SANE_Handle handle, SANE_Int *fd)
{
    (void)handle;
    (void)fd;
    return SANE_STATUS_UNSUPPORTED;
}
