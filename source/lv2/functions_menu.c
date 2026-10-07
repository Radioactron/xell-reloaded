#include <stdio.h>
#include <string.h>

#include <input/input.h>
#include <network/network.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <usb/usbmain.h>
#include <xenon_smc/xenon_smc.h>

#include "functions_menu.h"
#include "spooky.h"

#define MENU_ITEM_COUNT 2
#define BACK_HOLD_MS 1000

static const char *menu_items[MENU_ITEM_COUNT] = {
    "Reboot console",
    "Shutdown console"
};

static int read_controller(struct controller_data_s *ctrl)
{
    int port;

    spooky_poll();
    usb_do_poll();

    for (port = 0; port < 4; ++port) {
        if (spooky_get_controller_data(ctrl, port))
            return 1;
    }

    return 0;
}

static int controller_is_active(const struct controller_data_s *ctrl)
{
    return ctrl->a || ctrl->b || ctrl->x || ctrl->y || ctrl->up || ctrl->down ||
        ctrl->left || ctrl->right || ctrl->start || ctrl->back || ctrl->lb ||
        ctrl->rb || ctrl->logo || ctrl->lt || ctrl->rt || ctrl->s1_z ||
        ctrl->s2_z;
}

static void wait_for_buttons_release(void)
{
    struct controller_data_s ctrl;
    uint64_t start = mftb();

    while (tb_diff_msec(mftb(), start) < 300) {
        if (read_controller(&ctrl) && controller_is_active(&ctrl))
            start = mftb();

        network_poll();
        mdelay(10);
    }
}

static void shutdown_if_back_held(struct controller_data_s *ctrl, uint64_t *back_start)
{
    if (ctrl->back) {
        if (!*back_start)
            *back_start = mftb();

        if (tb_diff_msec(mftb(), *back_start) >= BACK_HOLD_MS) {
            printf("\nBack held - shutting down console...\n");
            spooky_restore();
            xenon_smc_power_shutdown();
            for (;;)
                mdelay(1000);
        }
    } else {
        *back_start = 0;
    }
}

static void draw_menu(int selected)
{
    int i;

    printf("\nFunctions\n");

    for (i = 0; i < MENU_ITEM_COUNT; ++i)
        printf(" %c %s\n", (i == selected) ? '>' : ' ', menu_items[i]);

    printf("\nD-pad Up/Down to move, A to select, B to go back, hold Back to shut down. Y toggles the prank.\n");
}

static void run_selected_function(int selected)
{
    if (selected == 0) {
        printf("\nRebooting console...\n");
        spooky_restore();
        xenon_smc_power_reboot();
        for (;;)
            mdelay(1000);
    }

    if (selected == 1) {
        printf("\nShutting down console...\n");
        spooky_restore();
        xenon_smc_power_shutdown();
        for (;;)
            mdelay(1000);
    }
}

static void functions_menu(void)
{
    struct controller_data_s ctrl;
    struct controller_data_s old_ctrl;
    int selected = 0;
    int redraw = 1;
    uint64_t back_start = 0;

    memset(&old_ctrl, 0, sizeof(old_ctrl));
    wait_for_buttons_release();

    for (;;) {
        if (redraw) {
            draw_menu(selected);
            redraw = 0;
        }

        if (read_controller(&ctrl)) {
            shutdown_if_back_held(&ctrl, &back_start);

            if (spooky_prank_active()) {
                old_ctrl=ctrl;
                network_poll();
                mdelay(10);
                continue;
            }

            if (ctrl.b > old_ctrl.b) {
                printf("\nReturning to file/TFTP loop...\n");
                wait_for_buttons_release();
                return;
            }

            if (ctrl.a > old_ctrl.a) {
                run_selected_function(selected);
                return;
            }

            if (ctrl.up > old_ctrl.up && selected > 0) {
                selected--;
                redraw = 1;
            }

            if (ctrl.down > old_ctrl.down && selected < MENU_ITEM_COUNT - 1) {
                selected++;
                redraw = 1;
            }

            old_ctrl = ctrl;
        }

        network_poll();
        mdelay(10);
    }
}

void xell_functions_poll(void)
{
    struct controller_data_s ctrl;
    struct controller_data_s old_ctrl;
    uint64_t back_start = 0;

    memset(&old_ctrl, 0, sizeof(old_ctrl));

    if (!read_controller(&ctrl))
        return;

    printf("\nController Detected! Click A to access functions or click B to continue this loop.\n");
    printf("Hold Back to shut down the console. Y toggles the fake red-ring prank.\n");

    old_ctrl = ctrl;

    for (;;) {
        if (read_controller(&ctrl)) {
            shutdown_if_back_held(&ctrl, &back_start);

            if (spooky_prank_active()) {
                old_ctrl=ctrl;
                network_poll();
                mdelay(10);
                continue;
            }

            if (ctrl.a > old_ctrl.a) {
                functions_menu();
                return;
            }

            if (ctrl.b > old_ctrl.b) {
                printf("\nContinuing file/TFTP loop...\n");
                wait_for_buttons_release();
                return;
            }

            old_ctrl = ctrl;
        }

        network_poll();
        mdelay(10);
    }
}
