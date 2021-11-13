#ifndef WIN_H_
#define WIN_H_

#include <stdint.h>
#include <stdio.h>  // FILE
#include <signal.h> // SIGINT
#include <Windows.h>
#include <conio.h>  // _kbhit


uint16_t check_key();

void disable_input_buffering();
void restore_input_buffering();
void handle_interrupt(int signal);

#endif