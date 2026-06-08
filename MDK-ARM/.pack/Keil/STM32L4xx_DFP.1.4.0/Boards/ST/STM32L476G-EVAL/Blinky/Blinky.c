/*----------------------------------------------------------------------------
 * Name:    Blinky.c
 * Purpose: LED Flasher
 *----------------------------------------------------------------------------
 * This file is part of the uVision/ARM development tools.
 * This software may only be used under the terms of a valid, current,
 * end user licence from KEIL for a compatible version of KEIL software
 * development tools. Nothing else gives you the right to use this software.
 *
 * This software is supplied "AS IS" without warranties of any kind.
 *
 * Copyright (c) 2015 Keil - An ARM Company. All rights reserved.
 *----------------------------------------------------------------------------*/

#include <stdio.h>

#include "cmsis_os.h"                   // ARM::CMSIS:RTOS:Keil RTX
#include "Board_LED.h"                  // ::Board Support:LED
#include "Board_Buttons.h"              // ::Board Support:Buttons

#include "stm32l4xx.h"                  // Device header

volatile int32_t delay_val = 500;

osThreadId tid_thrLED;                  // Thread id of thread: LED

/*
 * thrLED: blink LED and check button state
 */
void thrLED(void const *argument) {
  uint32_t led_max    = LED_GetCount();
  uint32_t led_num    = 0;

  for (;;) {
    LED_On(led_num);                                           // Turn specified LED on
    osSignalWait(0x0001, osWaitForever);
    LED_Off(led_num);                                          // Turn specified LED off
    osSignalWait(0x0001, osWaitForever);

    led_num++;                                                 // Change LED number
    if (led_num >= led_max) {
      led_num = 0;                                             // Restart with first LED
    }
  }

}


osThreadDef(thrLED, osPriorityNormal, 1, 0);

/*----------------------------------------------------------------------------
 * main: blink LED and check button state
 *----------------------------------------------------------------------------*/
int main (void) {
  uint32_t button_msk = (1U << Buttons_GetCount()) - 1;

  osKernelInitialize ();                                       // initialize CMSIS-RTOS

  SystemCoreClockUpdate();

  LED_Initialize();                                            // initialize LEDs
  Buttons_Initialize();                                        // initialize Buttons

  tid_thrLED = osThreadCreate (osThread(thrLED), NULL);        // create LED thread

  osKernelStart ();                                            // start thread execution

  for (;;) {                                                   // main must not be terminated!
    osDelay(delay_val);                                        // Wait
    while (Buttons_GetState() & (button_msk));                 // Wait while holding USER button
    osSignalSet(tid_thrLED, 0x0001);
  }
}
