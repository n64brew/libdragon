#include <stdbool.h>
#include <string.h>
#include <libdragon.h>


#define GBA_ROM_PATH "rom:/gbarom_mb.gba"
#define GBA_CHALLENGE 0x64646464
#define GBA_RESPONSE 0x6BA006BA

#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

static volatile bool identify_pending = false;
static volatile bool multiboot_pending = false;
static volatile bool multiboot_done = false;
static timer_link_t *progress_timer;
static volatile int gba_port;
static volatile uint32_t gba_controller_held;
static int x, y;

typedef enum KEYPAD_BITS {
	GBA_KEY_A		=	(1<<0),	/*!< keypad A button */
	GBA_KEY_B		=	(1<<1),	/*!< keypad B button */
	GBA_KEY_SELECT	=	(1<<2),	/*!< keypad SELECT button */
	GBA_KEY_START	=	(1<<3),	/*!< keypad START button */
	GBA_KEY_RIGHT	=	(1<<4),	/*!< dpad RIGHT */
	GBA_KEY_LEFT	=	(1<<5),	/*!< dpad LEFT */
	GBA_KEY_UP		=	(1<<6),	/*!< dpad UP */
	GBA_KEY_DOWN	=	(1<<7),	/*!< dpad DOWN */
	GBA_KEY_R		=	(1<<8),	/*!< Right shoulder button */
	GBA_KEY_L		=	(1<<9),	/*!< Left shoulder button */
} KEYPAD_BITS;

static void gba_link_multiboot_callback(int port, gba_link_multiboot_status_t status, void *ctx) {
    multiboot_done = status == GBA_LINK_MULTIBOOT_OK;
    multiboot_pending = false;
    if (!multiboot_done) {
        printf("Multi Boot FAILED: %d.\n", status);
    } else {
        printf("Multi Boot done\n");
    }
    delete_timer(progress_timer);
    timer_close();
}

static void print_progress(int ovfl) {
    printf("Progress: %d %%\n", gba_link_multiboot_progress(gba_port));
}

static void gba_link_multiboot_detect_callback(int port, bool multiboot_ready, void *ctx) {
    printf("Port %d: Multi Boot ready: %d\n", port, multiboot_ready);
    if (multiboot_ready) {
        multiboot_pending = true;
        timer_init();
        progress_timer = new_timer(TIMER_TICKS(500*1000), TF_CONTINUOUS, print_progress);
        // Send ROM via Multi Boot, GBA logo blinking green to yellow
        gba_link_multiboot_start(port, GBA_ROM_PATH, GBA_LINK_MULTIBOOT_LOGO_COLOR_3_GREEN, GBA_LINK_MULTIBOOT_LOGO_SPEED_PLUS_1, gba_link_multiboot_callback, NULL);
        printf("Starting GBA Multi Boot on port: %d.\n", port);
    }
    identify_pending = false;
}

static void gba_link_read_callback(int port, const uint8_t data[JOYBUS_GBA_LINK_CABLE_DATA_SIZE], uint8_t gba_status, gba_link_io_status_t io_status, void *ctx) {
    if (io_status == GBA_LINK_IO_STATUS_OK) {
        // Received controller status
        gba_controller_held = __builtin_bswap32(*(uint32_t*)data);
        //printf("GBA controller: 0x%08lx\n", gba_controller_held);
    }
}

static bool gba_challenge_response(int port) {
    uint32_t data = GBA_CHALLENGE;;
    uint8_t gba_status;
    gba_link_io_status_t io_status = gba_link_write(port, (uint8_t*)&data, &gba_status);
    if (io_status != GBA_LINK_IO_STATUS_OK) {
        return false;
    }
    for (int i=0; i<5; i++) {
        io_status = gba_link_read(port, (uint8_t*)&data, &gba_status);
        if (/*io_status == GBA_LINK_IO_STATUS_OK && */__builtin_bswap32(data) == GBA_RESPONSE) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    debug_init_emulog();
    debug_init_usblog();
    
    dfs_init(DFS_DEFAULT_LOCATION);

    asset_init_compression(2);

    console_init();
    console_set_render_mode(RENDER_MANUAL);
    console_set_debug(false);

    rdpq_init();
    rdpq_text_register_font(1, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_VAR));

    joypad_init();

    console_clear();
    printf("\n");
    printf("LibDragon GBA Link Cable Subsystem Test\n");
    printf("\n");
    console_render();

    // Detect if GBA is already listening
    for (gba_port = JOYPAD_PORT_2; gba_port <= JOYPAD_PORT_4; gba_port++) {
        if (gba_link_is_connected(gba_port)) {// && (gba_link_get_status(gba_port) & JOYBUS_IDENTIFY_STATUS_GBA_SENDING)) {
            printf("Found GBA Link Cable on port %d\n", gba_port);
            console_render();
            
            // Challenge-Response with GBA rom
            if (!gba_challenge_response(gba_port)) {
                printf("Failed challenge-response\n\n");
                console_render();
                break;
            }

            // Wait for player to press A on the GBA
            printf("Press A on GBA\n");
            console_render();
            uint32_t data;
            uint8_t gba_status;
            gba_link_io_status_t io_status;
            do {
                io_status = gba_link_read(gba_port, (uint8_t*)&data, &gba_status);
            } while ((io_status == GBA_LINK_IO_STATUS_OK || io_status == GBA_LINK_IO_STATUS_DATA_NOT_READY) && !(__builtin_bswap32(data) & GBA_KEY_A));

            if (__builtin_bswap32(data) & GBA_KEY_A) {
                // GBA already running, skip multiboot
                multiboot_done = true;
                printf("Skipping Multi Boot\n");
                console_render();
                break;
            }
        }
    }

    // Upload GBA rom over Multi Boot, if needed
    if (!multiboot_done) {
        gba_port = JOYPAD_PORT_1;

        printf("Plug the GBA via its Link Cable to controller port 2-4\n");
        printf("Power on the GBA with no game\n");
        printf("(or hold SELECT+START to prevent booting the cartridge game)\n");
        printf("Press A to detect GBA Link Cable and upload a Multi Boot rom\n");
        printf("\n");

        while (!multiboot_done) {
            joypad_poll();
            joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

            if (pressed.a && !identify_pending && !multiboot_pending) {
                gba_port++;
                if (gba_port > JOYPAD_PORT_4) {
                    gba_port = JOYPAD_PORT_2;
                }
                printf("Attempting GBA Multi Boot on port %d\n", gba_port);
                identify_pending = true;
                gba_link_multiboot_detect(gba_port, 3000, gba_link_multiboot_detect_callback, NULL);
            }

            console_render();
        }

        // Wait for GBA ROM to be ready
        while (!gba_challenge_response(gba_port)) {}
    }

    // Once GBA rom is sent via Multi Boot, read GBA controller status over JoyBus
    // GBA controls the text position
    display_close();
    display_init( RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED );
    x = 160;
    y = 120;
    while (1) {
        // Acquire a free framebuffer for rendering
        surface_t *disp = display_get();
        // Attach the framebuffer for use by rdpq
        rdpq_attach(disp, NULL);

        // Clear the framebuffer with black
        rdpq_clear((color_t){0, 0, 0, 0});

        // Read from GBA
        gba_link_read_async(gba_port, gba_link_read_callback, NULL);

        if (gba_controller_held & GBA_KEY_UP)
            y = CLAMP(y - 2, 40, 200);
        if (gba_controller_held & GBA_KEY_DOWN)
            y = CLAMP(y + 2, 40, 200);
        if (gba_controller_held & GBA_KEY_LEFT)
            x = CLAMP(x - 2, 20, 220);
        if (gba_controller_held & GBA_KEY_RIGHT)
            x = CLAMP(x + 2, 20, 220);

        rdpq_text_print(NULL, 1, x, y, "Move me from GBA");

        rdpq_detach_show();
    }
}
