/*** Include section ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** Define section ***/

// This mimics the Ctrl + whatever behavior, setting the
// 3 upper bits of the character pressed to 0.
#define CTRL_KEY(k) ((k) & 0x1f)
// Empty buffer
#define ABUF_INIT {NULL, 0}
// Version code
#define TTE_VERSION "0.0.1"

/*** Data section ***/

struct editor_config {
	int cursor_x;
	int cursor_y;
	int rows;
	int cols;
	struct termios orig_termios;
} ec;

// Having a dynamic buffer will allow us to write only one
// time once the screen is refreshing, instead of doing
// a lot of write's.
struct a_buf {
	char* buf;
	int len;
};

enum editor_key {
	ARROW_LEFT = 0x3e8, // 1000, large value out of range of a char.
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	PAGE_UP,
	PAGE_DOWN,
	HOME_KEY,
	END_KEY,
	DEL_KEY
};

/*** Declarations section ***/

void editorClearScreen();

/*** Terminal section ***/

void die(const char* s) {
	editorClearScreen();
	// perror looks for global errno variable and then prints
	// a descriptive error mesage for it.
	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ec.orig_termios) == -1)
		die("Failed to disable raw mode");
}

void enableRawMode() {
	// Save original terminal state into orig_termios.
	if (tcgetattr(STDIN_FILENO, &ec.orig_termios) == -1)
		die("Failed to get current terminal state");
	// At exit, restore the original state.
	atexit(disableRawMode);

	// Modify the original state to enter in raw mode.
	struct termios raw = ec.orig_termios;
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

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		// Ignoring EAGAIN to make it work on Cygwin.
		if (nread == -1 && errno != EAGAIN)
			die("Error reading input");
	}

	// Check escape sequences, if first byte
	// is an escape character then...
	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1 ||
			read(STDIN_FILENO, &seq[1], 1) != 1)
			return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
						// Home and End keys may be sent in many ways depending on the OS
						// \x1b[1~, \x1b[7~, \x1b[4~, \x1b[8~
						case '1':
						case '7':
							return HOME_KEY;
						case '4':
						case '8':
							return END_KEY;
						// Del key is sent as \x1b[3~
						case '3':
							return DEL_KEY;
						// Page Up and Page Down send '\x1b', '[', '5' or '6' and '~'.
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
					}
				}
			} else {
				switch (seq[1]) {
					// Arrow keys send multiple bytes starting with '\x1b', '[''
					// and followed by an 'A', 'B', 'C' or 'D' depending on which
					// arrow is pressed.
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					// Home key can also be sent as \x1b[H
					case 'H': return HOME_KEY;
					// End key can also be sent as \x1b[F
					case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				// Yes, Home key can ALSO be sent as \x1bOH
				case 'H': return HOME_KEY;
				// And... End key as \x1bOF
				case 'F': return END_KEY;
			}
		}
		return '\x1b';
	} else {
		return c;
	}
}

int getWindowSize(int* rows, int* cols) {
	struct winsize ws;

	// Getting window size thanks to ioctl into the given 
	// winsize struct.
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		return -1;
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** Append buffer section **/

void abufAppend(struct a_buf* ab, const char* s, int len) {
	// Using realloc to get a block of free memory that is
	// the size of the current string + the size of the string
	// to be appended.
	char* new = realloc(ab -> buf, ab -> len + len);

	if (new == NULL)
		return;

	// Copying the string s at the end of the current data in
	// the buffer.
	memcpy(&new[ab -> len], s, len);
	ab -> buf = new;
	ab -> len += len;
}

void abufFree(struct a_buf* ab) {
	// Deallocating buffer.
	free(ab -> buf);
}

/*** Output section ***/

void editorDrawWelcomeMessage(struct a_buf* ab) {
	char welcome[80];
	// Using snprintf to truncate message in case the terminal
	// is too tiny to handle the entire string.
	int welcome_len = snprintf(welcome, sizeof(welcome),
		"tte -- version %s", TTE_VERSION);
	if (welcome_len > ec.cols)
		welcome_len = ec.cols;
	// Centering the message.
	int padding = (ec.cols - welcome_len) / 2;
	// Remember that everything != 0 is true.
	if (padding) {
		abufAppend(ab, "~", 1);
		padding--;
	}
	while (padding--)
		abufAppend(ab, " ", 1);
	abufAppend(ab, welcome, welcome_len);
}

void editorDrawRows(struct a_buf* ab) {
	int y;
	for (y = 0; y < ec.rows; y++) {
		if (y == ec.rows / 3)
			editorDrawWelcomeMessage(ab);
		else
			abufAppend(ab, "~", 1);

		// Redrawing each line instead of the whole screen.
		abufAppend(ab, "\x1b[K", 3);
		// Addind a new line only if we are not printing the
		// last tilde.
		if (y < ec.rows - 1)
			abufAppend(ab, "\r\n", 2);
	}
}

void editorRefreshScreen() {
	struct a_buf ab = ABUF_INIT;

	// Hiding the cursor while the screen is refreshing.
	// See http://vt100.net/docs/vt100-ug/chapter3.html#S3.3.4
	// for more info.
	abufAppend(&ab, "\x1b[?25l", 6);
	abufAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	
	// Moving the cursor where it should be.
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ec.cursor_y + 1, ec.cursor_x + 1);
	abufAppend(&ab, buf, strlen(buf));

	// Showing again the cursor.
	abufAppend(&ab, "\x1b[?25h", 6);
	
	// Writing all content at once
	write(STDOUT_FILENO, ab.buf, ab.len);
	abufFree(&ab);
}

void editorClearScreen() {
	// Writing 4 bytes out to the terminal:
	// - (1 byte) \x1b : escape character
	// - (3 bytes) [2J : Clears the entire screen, see
	// http://vt100.net/docs/vt100-ug/chapter3.html#ED
	// for more info. 
	write(STDOUT_FILENO, "\x1b[2J", 4);
	// Writing 3 bytes to reposition the cursor back at 
	// the top-left corner, see
	// http://vt100.net/docs/vt100-ug/chapter3.html#CUP
	// for more info.
	write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** Input section ***/

void editorMoveCursor(int key) {
	switch (key) {
		case ARROW_LEFT:
			if (ec.cursor_x != 0)
				ec.cursor_x--;
			break;
		case ARROW_RIGHT:
			if (ec.cursor_x != ec.cols - 1)
				ec.cursor_x++;
			break;
		case ARROW_UP:
			if (ec.cursor_y != 0)
				ec.cursor_y--;
			break;
		case ARROW_DOWN:
			if (ec.cursor_y != ec.rows - 1)
				ec.cursor_y++;
			break;
	}
}

void editorProcessKeypress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			editorClearScreen();
			exit(0);
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{ // You can't declare variables directly inside a switch statement.
				int times = ec.rows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;
		case HOME_KEY:
			ec.cursor_x = 0;
			break;
		case END_KEY:
			ec.cursor_x = ec.cols - 1;
			break;
	}
}

/*** Init section ***/

void initEditor() {
	ec.cursor_x = 0;
	ec.cursor_y = 0;

	if (getWindowSize(&ec.rows, &ec.cols) == -1)
		die("Failed to get window size");
}

int main() {
	enableRawMode();
	initEditor();

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}