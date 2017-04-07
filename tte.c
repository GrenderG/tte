/*** Include section ***/

// We add them above our includes, because the header 
// files we are including use the macros to decide what 
// features to expose. These macros remove some compilation
// warnings. See
// https://www.gnu.org/software/libc/manual/html_node/Feature-Test-Macros.html
// for more info.
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
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
// Times to press Ctrl-Q before exiting
#define TTE_QUIT_TIMES 3

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
	int dirty; // To know if a file has been modified since opening.
	char* file_name;
	char status_msg[80];
	time_t status_msg_time;
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
	BACKSPACE = 0x7f, // 127
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

void editorRefreshScreen();

void editorSetStatusMessage(const char* msg, ...);

void consoleBufferOpen();

void abufFree();

void abufAppend();

char *editorPrompt(char* prompt, void (*callback)(char*, int));

/*** Terminal section ***/

void die(const char* s) {
	editorClearScreen();
	// perror looks for global errno variable and then prints
	// a descriptive error mesage for it.
	perror(s);
	printf("\r\n");
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

	consoleBufferOpen();

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

void editorUpdateWindowSize() {
	if (getWindowSize(&ec.screen_rows, &ec.screen_cols) == -1)
		die("Failed to get window size");
	ec.screen_rows -= 2; // Room for the status bar.
}

void editorHandleSigwinch() {
	editorUpdateWindowSize();
	if (ec.cursor_y > ec.screen_rows)
		ec.cursor_y = ec.screen_rows - 1;
	if (ec.cursor_x > ec.screen_cols)
		ec.cursor_x = ec.screen_cols - 1;
	editorRefreshScreen();
}

void consoleBufferOpen() {
	// Switch to another terminal buffer in order to be able to restore state at exit
    // by calling consoleBufferClose().
    if (write(STDOUT_FILENO, "\x1b[?47h", 6) == -1)
    	die("Error changing terminal buffer");
}

void consoleBufferClose() {
	// Restore console to the state tte opened.
	if (write(STDOUT_FILENO, "\x1b[?9l", 5) == -1 ||
		write(STDOUT_FILENO, "\x1b[?47l", 6) == -1)
		die("Error restoring buffer state");

	/*struct a_buf ab = {.buf = NULL, .len = 0};
	char* buf = NULL;
	if (asprintf(&buf, "\x1b[%d;%dH\r\n", ec.screen_rows + 1, 1) == -1)
		die("Error restoring buffer state");
	abufAppend(&ab, buf, strlen(buf));
	free(buf);

	if (write(STDOUT_FILENO, ab.buf, ab.len) == -1)
		die("Error restoring buffer state");
	abufFree(&ab);*/

	editorClearScreen();
}

/*** Row operations ***/

int editorRowCursorXToRenderX(editor_row* row, int cursor_x) {
	int render_x = 0;
	int j;
	// For each character, if its a tab we use rx % TTE_TAB_STOP 
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

int editorRowRenderXToCursorX(editor_row* row, int render_x) {
	int cur_render_x = 0;
	int cursor_x;
	for (cursor_x = 0; cursor_x < row -> size; cursor_x++) {
		if (row -> chars[cursor_x] == '\t')
			cur_render_x += (TTE_TAB_STOP - 1) - (cur_render_x % TTE_TAB_STOP);
		cur_render_x++;

		if (cur_render_x > render_x)
			return cursor_x;
	}
	return cursor_x;
}

void editorUpdateRow(editor_row* row) {
	// First, we have to loop through the chars of the row 
	// and count the tabs in order to know how much memory 
	// to allocate for render. The maximum number of characters 
	// needed for each tab is 8. row->size already counts 1 for 
	// each tab, so we multiply the number of tabs by 7 and add 
	// that to row->size to get the maximum amount of memory we'll 
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

void editorInsertRow(int at, char* s, size_t line_len) {
	if (at < 0 || at > ec.num_rows)
		return;

	ec.row = realloc(ec.row, sizeof(editor_row) * (ec.num_rows + 1));
	memmove(&ec.row[at + 1], &ec.row[at], sizeof(editor_row) * (ec.num_rows - at));

	ec.row[at].size = line_len;
	ec.row[at].chars = malloc(line_len + 1); // We want to add terminator char '\0' at the end
	memcpy(ec.row[at].chars, s, line_len);
	ec.row[at].chars[line_len] = '\0';

	ec.row[at].render_size = 0;
	ec.row[at].render = NULL;
	editorUpdateRow(&ec.row[at]);

	ec.num_rows++;
	ec.dirty++;
}

void editorFreeRow(editor_row* row) {
	free(row -> render);
	free(row -> chars);
}

void editorDelRow(int at) {
	if (at < 0 || at >= ec.num_rows)
		return;
	editorFreeRow(&ec.row[at]);
	memmove(&ec.row[at], &ec.row[at + 1], sizeof(editor_row) * (ec.num_rows - at - 1));
	ec.num_rows--;
	ec.dirty++;
}

void editorRowInsertChar(editor_row* row, int at, int c) {
	if (at < 0 || at > row -> size)
		at = row -> size;
	// We need to allocate 2 bytes because we also have to make room for
	// the null byte.
	row -> chars = realloc(row -> chars, row -> size + 2);
	// memmove it's like memcpy(), but is safe to use when the source and
	// destination arrays overlap
	memmove(&row -> chars[at + 1], &row -> chars[at], row -> size - at + 1);
	row -> size++;
	row -> chars[at] = c;
	editorUpdateRow(row);
	ec.dirty++; // This way we can see "how dirty" a file is.
}

void editorInsertNewline() {
	// If we're at the beginning of a line, all we have to do is insert 
	// a new blank row before the line we're on.
	if (ec.cursor_x == 0) {
		editorInsertRow(ec.cursor_y, "", 0);
	// Otherwise, we have to split the line we're on into two rows.
	} else {
		editor_row* row = &ec.row[ec.cursor_y];
		editorInsertRow(ec.cursor_y + 1, &row -> chars[ec.cursor_x], row -> size - ec.cursor_x);
		row = &ec.row[ec.cursor_y];
		row -> size = ec.cursor_x;
		row -> chars[row -> size] = '\0';
		editorUpdateRow(row);
	}
	ec.cursor_y++;
	ec.cursor_x = 0;
}

void editorRowAppendString(editor_row* row, char* s, size_t len) {
	row -> chars = realloc(row -> chars, row -> size + len + 1);
	memcpy(&row -> chars[row -> size], s, len);
	row -> size += len;
	row -> chars[row -> size] = '\0';
	editorUpdateRow(row);
	ec.dirty++;
}

void editorRowDelChar(editor_row* row, int at) {
	if (at < 0 || at >= row -> size)
		return;
	// Overwriting the deleted character with the characters that come
	// after it.
	memmove(&row -> chars[at], &row -> chars[at + 1], row -> size - at);
	row -> size--;
	editorUpdateRow(row);
	ec.dirty++;
}

/*** Editor operations ***/

void editorInsertChar(int c) {
	// If this is true, the cursor is on the tilde line after the end of
	// the file, so we need to append a new row to the file before inserting
	// a character there.
	if (ec.cursor_y == ec.num_rows)
		editorInsertRow(ec.num_rows, "", 0);
	editorRowInsertChar(&ec.row[ec.cursor_y], ec.cursor_x, c);
	ec.cursor_x++; // This way we can see "how dirty" a file is.
}

void editorDelChar() {
	// If the cursor is pats the end of the file, there's nothing to delete.
	if (ec.cursor_y == ec.num_rows)
		return;
	// Cursor is at the beginning of a file, there's nothing to delete.
	if (ec.cursor_x == 0 && ec.cursor_y == 0)
		return;

	editor_row* row = &ec.row[ec.cursor_y];
	if (ec.cursor_x > 0) {
		editorRowDelChar(row, ec.cursor_x - 1);
		ec.cursor_x--;
	// Deleting a line and moving up all the content.
	} else {
		ec.cursor_x = ec.row[ec.cursor_y - 1].size;
		editorRowAppendString(&ec.row[ec.cursor_y -1], row -> chars, row -> size);
		editorDelRow(ec.cursor_y);
		ec.cursor_y--;
	}
}

/*** File I/O ***/

char* editorRowsToString(int* buf_len) {
	int total_len = 0;
	int j;
	// Adding up the lengths of each row of text, adding 1
	// to each one for the newline character we'll add to 
	// the end of each line.
	for (j = 0; j < ec.num_rows; j++) {
		total_len += ec.row[j].size + 1;
	}
	*buf_len = total_len;

	char* buf = malloc(total_len);
	char* p = buf;
	// Copying the contents of each row to the end of the
	// buffer, appending a newline character after each
	// row.
	for (j = 0; j < ec.num_rows; j++) {
		memcpy(p, ec.row[j].chars, ec.row[j].size);
		p += ec.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void editorOpen(char* file_name) {
	free(ec.file_name);
	ec.file_name = strdup(file_name);

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
		editorInsertRow(ec.num_rows, line, line_len);
	}
	free(line);
	fclose(file);
	ec.dirty = 0;
}

void editorSave() {
	if (ec.file_name == NULL) {
		ec.file_name = editorPrompt("Save as: %s (ESC to cancel)", NULL);
		if (ec.file_name == NULL) {
			editorSetStatusMessage("Save aborted");
			return;
		}
	}

	int len;
	char* buf = editorRowsToString(&len);

	// We want to create if it doesn't already exist (O_CREAT flag), giving
	// 0644 permissions (the standard ones). O_RDWR stands for reading and
	// writing.
	int fd = open(ec.file_name, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		// ftruncate sets the file's size to the specified length.
		if (ftruncate(fd, len) != -1) {
			// Writing the file.
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				ec.dirty = 0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Cant's save file. Error occurred: %s", strerror(errno));
}

/*** Search section ***/

void editorSearchCallback(char* query, int key) {
	// Checking if the user pressed Enter or Escape, in which case 
	// they are leaving search mode so we return immediately.
	if (key == '\r' || key == '\x1b')
		return;

	int i;
	for (i = 0; i < ec.num_rows; i++) {
		editor_row* row = &ec.row[i];
		// We use strstr to check if query is a substring of the
		// current row. It returns NULL if there is no match,
		// oterwhise it returns a pointer to the matching substring.
		char* match = strstr(row -> render, query);
		if (match) {
			ec.cursor_y = i;
			ec.cursor_x = editorRowRenderXToCursorX(row, match - row -> render);
			// We set this like so to scroll to the bottom of the file so
			// that the next screen refresh will cause the matching line to
			// be at the very top of the screen.
			ec.row_offset = ec.num_rows;
			break;
		}
	}
}

void editorSearch() {
	int saved_cursor_x = ec.cursor_x;
	int saved_cursor_y = ec.cursor_y;
	int saved_col_offset = ec.col_offset;
	int saved_row_offset = ec.row_offset;

	char* query = editorPrompt("Search: %s (ESC to cancel)", editorSearchCallback);

	if (query) {
		free(query);
	// If query is NULL, that means they pressed Escape, so in that case we 
	// restore the cursor previous position.
	} else {
		ec.cursor_x = saved_cursor_x;
		ec.cursor_y = saved_cursor_y;
		ec.col_offset = saved_col_offset;
		ec.row_offset = saved_row_offset;
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

void editorScroll() {
	ec.render_x = 0;
	if (ec.cursor_y < ec.num_rows)
		ec.render_x = editorRowCursorXToRenderX(&ec.row[ec.cursor_y], ec.cursor_x);
	// The first if statement checks if the cursor is above the visible window, 
	// and if so, scrolls up to where the cursor is. The second if statement checks 
	// if the cursor is past the bottom of the visible window, and contains slightly 
	// more complicated arithmetic because ec.row_offset refers to what's at the top 
	// of the screen, and we have to get ec.screen_rows involved to talk about what's 
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

void editorDrawStatusBar(struct a_buf* ab) {
	// This switches to inverted colors.
	// NOTE:
	// The m command (Select Graphic Rendition) causes the text printed 
	// after it to be printed with various possible attributes including 
	// bold (1), underscore (4), blink (5), and inverted colors (7). An
	// argument of 0 clears all attributes (the default one). See
	// http://vt100.net/docs/vt100-ug/chapter3.html#SGR for more info.
	abufAppend(ab, "\x1b[7m", 4);

	char status[80], r_status[80];
	// Showing up to 20 characters of the filename, followed by the number of lines.
	int len = snprintf(status, sizeof(status), "Editing: %.20s %s", ec.file_name ? ec.file_name : "New file", ec.dirty ? "(modified)" : "");
	int r_len = snprintf(r_status, sizeof(r_status), "%d/%d lines  %d/%d cols", ec.cursor_y + 1 > ec.num_rows ? ec.num_rows : ec.cursor_y + 1, ec.num_rows,
		ec.cursor_x + 1 > ec.row[ec.cursor_y].size ? ec.row[ec.cursor_y].size : ec.cursor_x + 1, ec.row[ec.cursor_y].size);
	if (len > ec.screen_cols)
		len = ec.screen_cols;
	abufAppend(ab, status, len);
	while (len < ec.screen_cols) {
		if (ec.screen_cols - len == r_len) {
			abufAppend(ab, r_status, r_len);
			break;
		} else {
			abufAppend(ab, " ", 1);
			len++;
		}
	}
	// This switches back to normal colors.
	abufAppend(ab, "\x1b[m", 3);

	abufAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct a_buf *ab) {
	// Clearing the message bar.
	abufAppend(ab, "\x1b[K", 3);
	int msg_len = strlen(ec.status_msg);
	if (msg_len > ec.screen_cols)
		msg_len = ec.screen_cols;
	// We only show the message if its less than 5 secons old, but
	// remember the screen is only being refreshed after each keypress.
	if (msg_len && time(NULL) - ec.status_msg_time < 5)
		abufAppend(ab, ec.status_msg, msg_len);
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

// The ... argument makes editorSetStatusMessage() a variadic function, 
// meaning it can take any number of arguments. C's way of dealing with 
// these arguments is by having you call va_start() and va_end() on a 
// // value of type va_list. The last argument before the ... (in this 
// case, msg) must be passed to va_start(), so that the address of 
// the next arguments is known. Then, between the va_start() and 
// va_end() calls, you would call va_arg() and pass it the type of 
// the next argument (which you usually get from the given format 
// string) and it would return the value of that argument. In 
// this case, we pass msg and args to vsnprintf() and it takes care 
// of reading the format string and calling va_arg() to get each 
// argument.
void editorSetStatusMessage(const char* msg, ...) {
	va_list args;
	va_start(args, msg);
	vsnprintf(ec.status_msg, sizeof(ec.status_msg), msg, args);
	va_end(args);
	ec.status_msg_time = time(NULL);
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
		// Addind a new line
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
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);
	
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

char* editorPrompt(char* prompt, void (*callback)(char*, int)) {
	size_t buf_size = 128;
	char* buf = malloc(buf_size);

	size_t buf_len = 0;
	buf[0] = '\0';

	while (1) {
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buf_len != 0)
				buf[--buf_len] = '\0';
		} else if (c == '\x1b') {
			editorSetStatusMessage("");
			if (callback)
				callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buf_len != 0) {
				editorSetStatusMessage("");
				if (callback)
					callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c)) {
			if (buf_len == buf_size - 1) {
				buf_size *= 2;
				buf = realloc(buf, buf_size);
			}
			buf[buf_len++] = c;
			buf[buf_len] = '\0';
		}

		if (callback)
			callback(buf, c);
	}
}

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
	static int quit_times = TTE_QUIT_TIMES;

	int c = editorReadKey();

	switch (c) {
		case '\r': // Enter key
			editorInsertNewline();
			break;
		case CTRL_KEY('q'):
			if (ec.dirty && quit_times > 0) {
				editorSetStatusMessage("Warning! File has unsaved changes. Press Ctrl-Q %d more times to quit", quit_times);
				quit_times--;
				return;
			}
			editorClearScreen();
			consoleBufferClose();
			exit(0);
			break;
		case CTRL_KEY('s'):
			editorSave();
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
			if (ec.cursor_y < ec.num_rows)
				ec.cursor_x = ec.row[ec.cursor_y].size;
			break;
		case CTRL_KEY('f'):
			editorSearch();
			break;
		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY)
				editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;
		case CTRL_KEY('l'):
		case '\x1b': // Escape key
			break;
		default:
			editorInsertChar(c);
			break;
	}

	quit_times = TTE_QUIT_TIMES;
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
	ec.dirty = 0;
	ec.file_name = NULL;
	ec.status_msg[0] = '\0';
	ec.status_msg_time = 0;
	editorUpdateWindowSize();
	// The SIGWINCH signal is sent to a process when its controlling 
	// terminal changes its size (a window change).
	signal(SIGWINCH, editorHandleSigwinch); 
}

int main(int argc, char* argv[]) {
	enableRawMode();
	initEditor();
	if (argc >= 2)
		editorOpen(argv[1]);

	editorSetStatusMessage("Ctrl-Q to quit | Ctrl-S to save | Ctrl-F to search - ISO-8859-1 is recommended");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
