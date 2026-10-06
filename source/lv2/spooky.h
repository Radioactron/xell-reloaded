#ifndef XELL_SPOOKY_H
#define XELL_SPOOKY_H

/* Call after console_init(), then mark USB ready after usb_init(). */
void spooky_init(void);
void spooky_input_ready(void);
void spooky_poll(void);
struct controller_data_s;
/* libxenon's getter consumes each USB report. This broker preserves menu input
 * after the animation code has inspected Y in the same report. */
int spooky_get_controller_data(struct controller_data_s *ctrl, int port);

/* Exit the prank before handing control to a boot file or flash updater. */
void spooky_restore(void);
int spooky_prank_active(void);

#endif
