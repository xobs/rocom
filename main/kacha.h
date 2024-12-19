#ifndef KACHA_H_
#define KACHA_H_

#include <esp_err.h>

esp_err_t configure_external_phy(void);
esp_err_t free_external_phy(void);
void reset_target_run(void);
void reset_target_bootloader(void);
void configure_reset_boot(void);
void tristate_usb(void);
void drive_usb(void);

#endif /* KACHA_H_ */
