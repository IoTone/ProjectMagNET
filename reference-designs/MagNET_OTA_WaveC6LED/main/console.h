#pragma once
/* Line-oriented console. Owns the read loop so provisioning commands are handled
 * in C and everything else falls through to Forth. */
void console_run(int (*getch)(void), void (*putch)(int),
                 void (*print)(const char *));
