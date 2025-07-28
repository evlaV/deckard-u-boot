#ifndef __DECKARD__H
#define __DECKARD__H

#define DECKARD_SHUTDOWN_TIMEOUT_S 10
#define DECKARD_SHUTDOWN_TIMEOUT_US (DECKARD_SHUTDOWN_TIMEOUT_S * 1000 * 1000)

const char *deckard_get_fw_part(void);
bool deckard_shutdown_condition(struct udevice *pmic);

#endif /* __DECKARD_H */
