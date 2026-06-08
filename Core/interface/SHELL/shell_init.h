#ifndef SHELL_INIT_H
#define SHELL_INIT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void Shell_Init(void);
void Shell_RxCallback(uint8_t c);
#ifdef __cplusplus
}
#endif
#endif

