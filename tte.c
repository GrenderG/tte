/*** Include section ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** Data section ***/

struct termios orig_termios;

/*** Terminal section ***/

void die(const char* s) {
	// perror looks for global errno variable and then prints
	// a descriptive error mesage for it.
	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
		die("Failed to disable raw mode");
}

void enableRawMode() {
	// Save original terminal state into orig_termios.
	if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
		die("Failed to get current terminal state");
	// At exit, restore the original state.
	atexit(disableRawMode);

	// Modify the original state to enter in raw mode.
	struct termios raw = orig_termios;
	// This disables Ctrl-M, Ctrl-S and Ctrl-Q commands.
	// (BRKINT, INPCK and ISTRIP are not estrictly mandatory,
	// but it is recommended to turn them off in case any
	// system needs it).
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	// Turning off all output processing (\r\n).
	raw.c_oflag &= ~(OPOST);
	// Setting character size to 8 bits per byte (it should be
	// like that on most systems, but whatever).
	raw.c_cflag |= (CS8);
	// Using NOT operator on ECHO | ICANON | IEXTEN | ISIG and  
	// then bitwise-AND them with flags field in order to 
	// force c_lflag 4th bit to become 0. This disables 
	// chars being printed (ECHO) and let us turn off 
	// canonical mode in order to read input byte-by-byte 
	// instead of line-by-line (ICANON), ISIG disables
	// Ctrl-C command and IEXTEN the Ctrl-V one.
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	// read() function now returns as soon as there is any
	// input to be read.
	raw.c_cc[VMIN] = 0;
	// Forcing read() function to return every 1/10 of a 
	// second if there is nothing to read.
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("Failed to set raw mode");
}

/*** Init section ***/

int main() {
	enableRawMode();

	while (1) {
		char c = '\0';
		// Ignoring EAGAIN to make it work on Cygwin.
		if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
			die("Error reading input");

		if (isprint(c))
			printf("%d ('%c')\r\n", c, c);
		else
			printf("%d\n\r", c);

		if (c == 'q')
			break;
	}

	return 0;
}