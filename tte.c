/*** Include section ***/

// We add them above our includes, because the header 
// files we’re including use the macros to decide what 
// features to expose. These macros remove some compilation
// warnings. See
// https://www.gnu.org/software/libc/manual/html_node/Feature-Test-Macros.html
// for more info.
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
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
// Length of a tab stop
#define TTE_TAB_STOP 8

/*** Data section ***/

typedef struct editor_row {
	int size;
	int render_size;
	char* chars;
	char* render;
} editor_row;

struct editor_config {
	int cursor_x;
	int cursor_y;
	int render_x;
	int row_offset;
	int col_offset;
	int screen_rows;
	int screen_cols;
	int num_rows;
	editor_row* row;
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
	ARROW_LEFT = 0x3e8, // 1000, large value out of the range of a char.
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

int getWindowSize(int* screen_rows, int* screen_cols) {
	struct winsize ws;

	// Getting window size thanks to ioctl into the given 
	// winsize struct.
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		return -1;
	} else {
		*screen_cols = ws.ws_col;
		*screen_rows = ws.ws_row;
		return 0;
	}
}

/*** Row operations ***/

int editorRowCursorXToRenderX(editor_row* row, int cursor_x) {
	int render_x = 0;
	int j;
	// For each character, if it’s a tab we use rx % TTE_TAB_STOP 
	// to find out how many columns we are to the right of the last 
	// tab stop, and then subtract that from TTE_TAB_STOP - 1 to 
	// find out how many columns we are to the left of the next tab 
	// stop. We add that amount to rx to get just to the left of the 
	// next tab stop, and then the unconditional rx++ statement gets 
	// us right on the next tab stop. Notice how this works even if 
	// we are currently on a tab stop.
	for (j = 0; j < cursor_x; j++) {
		if (row -> chars[j] == '\t')
			render_x += (TTE_TAB_STOP - 1) - (render_x % TTE_TAB_STOP);
		render_x++;
	}
	return render_x;
}

void editorUpdateRow(editor_row* row) {
	// First, we have to loop through the chars of the row 
	// and count the tabs in order to know how much memory 
	// to allocate for render. The maximum number of characters 
	// needed for each tab is 8. row->size already counts 1 for 
	// each tab, so we multiply the number of tabs by 7 and add 
	// that to row->size to get the maximum amount of memory we’ll 
	// need for the rendered row.
	int tabs = 0;
	int j;
	for (j = 0; j < row -> size; j++) {
		if (row -> chars[j] == '\t')
			tabs++;
	}
	free(row -> render);
	row -> render = malloc(row -> size + tabs * (TTE_TAB_STOP - 1) + 1);

	// After allocating the memory, we check whether the current character 
	// is a tab. If it is, we append one space (because each tab must 
	// advance the cursor forward at least one column), and then append 
	// spaces until we get to a tab stop, which is a column that is 
	// divisible by 8
	int idx = 0;
	for (j = 0; j < row -> size; j++) {
		if (row -> chars[j] == '\t') {
			row -> render[idx++] = ' ';
			while (idx % TTE_TAB_STOP != 0)
				row -> render[idx++] = ' ';
		} else 
			row -> render[idx++] = row -> chars[j];
	}
	row -> render[idx] = '\0';
	row -> render_size = idx;
}

void editorAppendRow(char* s, size_t line_len) {
	ec.row = realloc(ec.row, sizeof(editor_row) * (ec.num_rows + 1));

	int at = ec.num_rows;
	ec.row[at].size = line_len;
	ec.row[at].chars = malloc(line_len + 1); // We want to add terminator char '\0' at the end
	memcpy(ec.row[at].chars, s, line_len);
	ec.row[at].chars[line_len] = '\0';

	ec.row[at].render_size = 0;
	ec.row[at].render = NULL;
	editorUpdateRow(&ec.row[at]);

	ec.num_rows++;
}

/*** File I/O ***/

void editorOpen(char* file_name) {
	FILE* file = fopen(file_name, "r");
	if (!file)
		die("Failed to open the file");

	char* line = NULL;
	// Unsigned int of at least 16 bit.
	size_t line_cap = 0;
	// Bigger than int
	ssize_t line_len;
	while ((line_len = getline(&line, &line_cap, file)) != -1) {
		// We already know each row represents one line of text, there's no need
		// to keep carriage return and newline characters.
		if (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
			line_len--;
		editorAppendRow(line, line_len);
	}
	free(line);
	fclose(file);
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

void editorScroll() {
	ec.render_x = 0;
	if (ec.cursor_y < ec.num_rows)
		ec.render_x = editorRowCursorXToRenderX(&ec.row[ec.cursor_y], ec.cursor_x);
	// The first if statement checks if the cursor is above the visible window, 
	// and if so, scrolls up to where the cursor is. The second if statement checks 
	// if the cursor is past the bottom of the visible window, and contains slightly 
	// more complicated arithmetic because ec.row_offset refers to what’s at the top 
	// of the screen, and we have to get ec.screen_rows involved to talk about what’s 
	// at the bottom of the screen.
	if (ec.cursor_y < ec.row_offset)
		ec.row_offset = ec.cursor_y;
	if (ec.cursor_y >= ec.row_offset + ec.screen_rows)
		ec.row_offset = ec.cursor_y - ec.screen_rows + 1;

	if (ec.render_x < ec.col_offset)
		ec.col_offset = ec.render_x;
	if (ec.render_x >= ec.col_offset + ec.screen_cols)
		ec.col_offset = ec.render_x - ec.screen_cols + 1;
}

void editorDrawWelcomeMessage(struct a_buf* ab) {
	char welcome[80];
	// Using snprintf to truncate message in case the terminal
	// is too tiny to handle the entire string.
	int welcome_len = snprintf(welcome, sizeof(welcome),
		"tte -- version %s", TTE_VERSION);
	if (welcome_len > ec.screen_cols)
		welcome_len = ec.screen_cols;
	// Centering the message.
	int padding = (ec.screen_cols - welcome_len) / 2;
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
	for (y = 0; y < ec.screen_rows; y++) {
		int file_row = y + ec.row_offset;
		if(file_row >= ec.num_rows) {
			if (ec.num_rows == 0 && y == ec.screen_rows / 3)
				editorDrawWelcomeMessage(ab);
			else
				abufAppend(ab, "~", 1);
		} else {
			int len = ec.row[file_row].render_size - ec.col_offset;
			// len can be a negative number, meaning the user scrolled 
			// horizontally past the end of the line. In that case, we set 
			// len to 0 so that nothing is displayed on that line.
			if (len < 0)
				len = 0;
			if (len > ec.screen_cols)
				len = ec.screen_cols;
			abufAppend(ab, &ec.row[file_row].render[ec.col_offset], len);
		}

		// Redrawing each line instead of the whole screen.
		abufAppend(ab, "\x1b[K", 3);
		// Addind a new line only if we are not printing the
		// last tilde.
		if (y < ec.screen_rows - 1)
			abufAppend(ab, "\r\n", 2);
	}
}

void editorRefreshScreen() {
	editorScroll();

	struct a_buf ab = ABUF_INIT;

	// Hiding the cursor while the screen is refreshing.
	// See http://vt100.net/docs/vt100-ug/chapter3.html#S3.3.4
	// for more info.
	abufAppend(&ab, "\x1b[?25l", 6);
	abufAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	
	// Moving the cursor where it should be.
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ec.cursor_y - ec.row_offset) + 1, (ec.render_x - ec.col_offset) + 1);
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
	editor_row* row = (ec.cursor_y >= ec.num_rows) ? NULL : &ec.row[ec.cursor_y];

	switch (key) {
		case ARROW_LEFT:
			if (ec.cursor_x != 0)
				ec.cursor_x--;
			// If <- is pressed, move to the end of the previous line
			else if (ec.cursor_y > 0) {
				ec.cursor_y--;
				ec.cursor_x = ec.row[ec.cursor_y].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && ec.cursor_x < row -> size)
				ec.cursor_x++;
			// If -> is pressed, move to the start of the next line
			else if (row && ec.cursor_x == row -> size) {
				ec.cursor_y++;
				ec.cursor_x = 0;
			}
			break;
		case ARROW_UP:
			if (ec.cursor_y != 0)
				ec.cursor_y--;
			break;
		case ARROW_DOWN:
			if (ec.cursor_y < ec.num_rows)
				ec.cursor_y++;
			break;
	}

	// Move cursor_x if it ends up past the end of the line it's on
	row = (ec.cursor_y >= ec.num_rows) ? NULL : &ec.row[ec.cursor_y];
	int row_len = row ? row -> size : 0;
	if (ec.cursor_x > row_len)
		ec.cursor_x = row_len;
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
				if (c == PAGE_UP)
					ec.cursor_y = ec.row_offset;
				else if (c == PAGE_DOWN)
					ec.cursor_y = ec.row_offset + ec.screen_rows - 1;

				int times = ec.screen_rows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;
		case HOME_KEY:
			ec.cursor_x = 0;
			break;
		case END_KEY:
			ec.cursor_x = ec.screen_cols - 1;
			break;
	}
}

/*** Init section ***/

void initEditor() {
	ec.cursor_x = 0;
	ec.cursor_y = 0;
	ec.render_x = 0;
	ec.row_offset = 0;
	ec.col_offset = 0;
	ec.num_rows = 0;
	ec.row = NULL;

	if (getWindowSize(&ec.screen_rows, &ec.screen_cols) == -1)
		die("Failed to get window size");
}

int main(int argc, char* argv[]) {
	enableRawMode();
	initEditor();
	if (argc >= 2)
		editorOpen(argv[1]);

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}