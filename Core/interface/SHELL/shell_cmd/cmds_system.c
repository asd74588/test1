#include "cmds.h"
#include <stdio.h>

int cmd_help(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    show_all_cmds();
    return 0;
}

int cmd_clear(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_printf("\033[2J\033[H");
    return 0;
}

int cmd_reboot(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_printf("Rebooting...\r\n");
    HAL_Delay(50U);
    NVIC_SystemReset();
    return 0;
}

int cmd_version(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_printf("Firmware : %s\r\n", sys_get_version());
    shell_printf("Build    : %s %s\r\n", __DATE__, __TIME__);
    shell_printf("Shell    : %s\r\n", NR_SHELL_VERSION);
    return 0;
}
