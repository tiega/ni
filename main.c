/*** includes ***/

#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#define _BSD_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#ifdef __APPLE__
#include <termios.h>
#else
#include <termio.h>
#endif

#include <time.h>
#include <unistd.h>

/***defines ***/

#define NI_VERSION "0.0.1"
#define NI_TAB_STOP 4

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKeys {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP ,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    SPACE,
};

enum editorModes {
    INSERT_MODE,
    NORMAL_MODE,
    COMMAND_MODE,
};

/*** dynamic string type ***/
typedef struct {
    char *b; // Heap allocated buffer
    int len; // string length
} abuf;

#define ABUF_INIT {NULL, 0}

/*** dynamic string methods ***/
void abAppend(abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abDelete(abuf *ab, size_t n) {
    // Reduce len, does not reallocate memory
    if (ab->len - n > 0) {
        ab->len -= n;
    }
}

void abFree(abuf *ab) {
    ab->len = 0;
    free(ab->b);
}

/*** data ***/

typedef struct {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

struct editorConfig {
    enum editorModes mode; // Editor mode
    abuf cmdbuf; // Command buffer for command mode and others
    int cmdrep; // Movement repetition

    int cx, cy; // Cursor coord in files
    int rx; // Cursor x rendered with tabs
    int rowoff; // Row offset for scroll
    int coloff; // Column offset
    int screenrows; // Screen dimensions
    int screencols;

    int numrows; // No. rows in buffer
    erow *row; // dynamically allocated line array of the buffer

    char *filename; // file in current editor buffer
    char statusmsg[80]; // Status
    time_t statusmsg_time;

    struct termios orig_termios; // Original terminal attributes
};

struct editorConfig E;

/*** terminal ***/

void editorClearScreen();

/*
 * Errorhandling.
 * Check each of our library calls for failure and call die when they fail
 */
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

/*
 * Restore terminal attributes before quitting the program
 */
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

/*
 * Turn off echo mode in the terminal
 */
void enableRawMode() {
    // Read the current attributes into a struct
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");

    // Register disableRawMode to be called at exit
    atexit(disableRawMode);

    // Copy the original attributes
    struct termios raw = E.orig_termios;

    // Disable input flags:
    //     IXON: Ctrl-S and Ctrl-Q (flow control)
    //     ICRNL: Stop translating carriage returns into newlines
    //            Ctrl-M and Enter both read 13, '\r' instead of 10, '\n'
    //     BRKINT: Stop break condition from sending SIGINT
    // Other misc flags:
    //     INPCK, ISTRIP: Flags required to enable raw mode in old
    //          terminals that are probably already disabled
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | ICRNL | IXON);

    // Disable output flags:
    //     OPOST: Stop translating newlines into carriage returns followed
    //            by a newline ('\n' to '\r\n')
    raw.c_oflag &= ~(OPOST);

    // Set character size to 8 bits per byte. Probably already set
    raw.c_cflag |= (CS8);

    // Disable local flags:
    //     ECHO mode (key presses aren't echoed)
    //     Canonical mode (each key press is passed to the program instead
    //         of new lines)
    //     ISIG: Ctrl-C and Ctrl-Z
    //     IEXTEN: Ctrl-V
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // Set a timeout for read() so we can do something else while waiting
    // Set the timeout to 1/10 of a second
    raw.c_cc[VTIME] = 1;
    // Set maximum amount of time read() waits before returning
    raw.c_cc[VMIN] = 0;

    // Write the new terminal attributes
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/*
 * Reads a key press and return the char
 */
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // Process escape sequences
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }

            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }

        return '\x1b';
    }

    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    // Null terminate the string
    buf[i] = '\0';

    // Make sure it begins with <ESC>[
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // If sys/ioctl fail to give us the screen size,
        // we do it the hard way :)
        // Send the cursor to the bottom right corner of the screen and query
        // its position.
        //
        // There isn't a command to send the cursor to the bottom right of the screen :(
        // However, the C (cursor forward) and B (cursor down) escape sequences
        // are documented to stop the cursor from going past the edge of the
        // screen
        //
        // We don't use the H escape sequence because it is not documented
        // what happens when you move the cursor off screen.
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

/*
 * Calculate correct cursor x position on screen
 * with the correct tab indentations
 */
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j=0; j < cx; ++j) {
        if (row->chars[j] == '\t') {
            rx += (NI_TAB_STOP - 1) - (rx % NI_TAB_STOP);
        }
        ++rx;
    }
    return rx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;

    for (j = 0; j < row->size; ++j) {
        if (row->chars[j] == '\t') ++tabs;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(NI_TAB_STOP) + 1);

    int idx = 0;
    for (j = 0; j < row->size; ++j) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % NI_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
}

/*** file i/o ***/
void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // getline takes a NULL lineptr, current capacity (0), and filepointer
    // and allocates the memory for the next line it reads.
    while ((linelen = getline(&line, &linecap, fp)) != -1 ) {
        // Strip the newline/carriage return characters
        while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) {
            linelen--;
        }
        // Copy to our editor row buffer
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
}

/*
 * Write an escape sequence to the screen.
 * Escape sequence begins with the "\x1b"
 * byte which means ESCAPE or 27 in decimal,
 * followed by '['.
 * '2J' clears the entire screen
 */
void editorExit() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
}

/*** Normal mode ***/

/*
 * Process new numeric values to
 * generate the proper number of
 * reps a command will be ran
 */
void editorNormalModeNumRep(int n) {
    E.cmdrep = E.cmdrep*10 + n;
}

/*** Command mode ***/

/*
 * Handle command mode commands
 */
void editorCommandModeHandle() {
    int j;
    int _write = 0;
    int _quit = 0;
    for (j=0; j<E.cmdbuf.len; ++j) {
        if (E.cmdbuf.b[j] == 'q') {
            _quit = 1;
        }
    }
    if (_write) {
        // Write file here
    }

    if (_quit) editorExit();
}

/*** output ***/

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}


/*
 * Handle drawing each row of the text buffer being edited
 */
void editorDrawRows(abuf *ab) {
    int y;
    for (y=0; y < E.screenrows; y++) {

        // Vertical offset
        int filerow = y + E.rowoff;

        if (filerow >= E.numrows) {
            // print welcome screen if buffer is empty
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                        "Ni editor -- version %s", NI_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);

            } else {
                // Print ~ in from of empty lines
                abAppend(ab, "~", 1);
            }

        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, E.row[filerow].render + E.coloff, len);
        }

        // Clear line
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

char *editorGetMode() {
    switch (E.mode) {
        case INSERT_MODE:
            {
                return "INSERT\0";
            }
        case NORMAL_MODE:
            {
                return "NORMAL\0";
            }
        case COMMAND_MODE:
            {
                return "COMMAND\0";
            }
    }
    return "UNKNOWN\0";
}

void editorDrawStatusBar(abuf *ab) {
    abAppend(ab, "\x1b[7m", 4); // Set status bar background

    // Create status (left) and rstatus (right) messages
    char status[80], rstatus[80];
    char* mode = editorGetMode();
    int len = snprintf(status, sizeof(status), " %.20s | %.20s | %d lines", mode, E.filename ? E.filename : "[No name]", E.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d ", E.cy + 1, E.cx + 1);

    if (len > E.screencols) len = E.screencols;

    // Align status to the left
    abAppend(ab, status, len);

    // Alight rstatus to the right
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            ++len;
        }
    }
    abAppend(ab, "\x1b[m", 3); // Clear status bar formatting
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(abuf *ab) {
    abAppend(ab, "\x1b[K", 3); // Clear line

    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    /*if (msglen && time(NULL) - E.statusmsg_time < 5) {*/
    abAppend(ab, E.statusmsg, msglen);
    /*}*/

    if (E.cmdrep != 0) {
        char rstatusmsg[10];
        int rlen = snprintf(rstatusmsg, sizeof(rstatusmsg), "%d ", E.cmdrep);
        // Alight rstatus to the right
        while (msglen < E.screencols) {
            if (E.screencols - msglen == rlen) {
                abAppend(ab, rstatusmsg, rlen);
                break;
            } else {
                abAppend(ab, " ", 1);
                ++msglen;
            }
        }
    }
}

void editorRefreshScreen() {
    editorScroll();
    abuf ab = ABUF_INIT;

    // Hide cursor
    abAppend(&ab, "\x1b[?25l", 6);
    // Reset cursor
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);  // Draw editor buffer
    editorDrawStatusBar(&ab); // Draw status line
    editorDrawMessageBar(&ab); // Draw status message

    // Set cursor position
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1 - E.rowoff, E.rx + 1 - E.coloff);
    abAppend(&ab, buf, strlen(buf));

    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);

    // Write buffer
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

/*
 * Handle all cursor movement keys
 */
void editorMoveCursor(int key) {
    // Current row
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case 'k':
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case 'j':
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
        case 'h':
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case 'l':
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;

        case 'w':
        case 'W':
        case 'e':
        case 'E':
            if (row) {
                if (key == 'W' || key == 'E') {
                    // Move pass all chars
                    while (E.cx < row->size && !isspace(E.row[E.cy].chars[E.cx++])) {}
                } else { // w, e
                    // TODO: moving words while stopping at punctuations doesn't really work
                    while (E.cx < row->size && !isspace(E.row[E.cy].chars[E.cx++]) && !ispunct(E.row[E.cy].chars[E.cx])) {}
                }
                if (key == 'W' || key == 'w') {
                    // Move pass all spaces
                    while (E.cx < row->size && E.row[E.cy].chars[E.cx] == ' ') E.cx++;
                }

                if (E.cx >= row->size) {
                    E.cy++;
                    E.cx = 0;
                }

            }
    }

    // Snap cursor to end of line
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

/*
 * Waits for a keypress and handles it
 */
void editorProcessKeypress() {
    int c = editorReadKey();

    if (E.mode == NORMAL_MODE) {

        if (c <= '9' && (c >= '1' || (E.cmdrep !=0 && c >= '0'))) {
            // If the char is between 1-9, start counting for rep cmd
            // and from now on 0 is acceptable as well (normally beginning of line)
            editorNormalModeNumRep(c-'0');
        } else {

            switch (c) {
                // Insert mode
                case 'i':
                    E.mode = INSERT_MODE;
                    break;

                    // Command mode
                case ':':
                    E.mode = COMMAND_MODE;
                    editorSetStatusMsg(":");
                    break;

                    // Easy quit command
                case CTRL_KEY('q'): // Ctrl-Q to quit
                    editorExit();
                    break;

                    // Beginning of line
                case '0':
                case HOME_KEY:
                    E.cx = 0;
                    break;

                    // End of line
                case '$':
                case END_KEY:
                    if (E.cy < E.numrows) {
                        E.cx = E.row[E.cy].size;
                    }
                    break;

                case CTRL_KEY('u'):
                case CTRL_KEY('d'):
                case PAGE_UP:
                case PAGE_DOWN:
                    {
                        // Move cursor to top or bottom of screen
                        if (c == PAGE_UP || c == CTRL_KEY('u')) {
                            E.cy = E.rowoff;
                        } else if (c == PAGE_DOWN || c == CTRL_KEY('d')) {
                            E.cy = E.rowoff + E.screenrows - 1;
                            if (E.cy > E.numrows) E.cy = E.numrows;
                        }

                        int times = E.screenrows;
                        while (times--) {
                            editorMoveCursor(c==PAGE_DOWN || c == CTRL_KEY('d') ? ARROW_DOWN : ARROW_UP);
                        }
                    }
                    break;

                case 'k':
                case 'j':
                case 'l':
                case 'h':
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                case 'w':
                case 'W':
                case 'b':
                case 'B':
                case 'e':
                case 'E':
                    editorMoveCursor(c);
                    break;
            }

            E.cmdrep = 0;
        }

    } else if (E.mode == INSERT_MODE) {
        switch (c) {
            case '\x1b': // Esc to return to normal mode
                E.mode = NORMAL_MODE;
                break;

            case ARROW_UP:
            case ARROW_DOWN:
            case ARROW_LEFT:
            case ARROW_RIGHT:
                editorMoveCursor(c);
                break;
        }
    } else if (E.mode == COMMAND_MODE) {
        switch (c) {
            case 13: // Enter key executes command
                editorCommandModeHandle();

                // Clear command buffer and return to normal mode
                abFree(&E.cmdbuf); // free the command buffer
                editorSetStatusMsg(""); // clear status message
                E.mode = NORMAL_MODE;
                break;

            case '\x1b': // Esc to return to normal mode
                abFree(&E.cmdbuf); // free the command buffer
                editorSetStatusMsg(""); // clear status message
                E.mode = NORMAL_MODE;
                break;

            case '8': // Delete key
                abDelete(&E.cmdbuf, 1);
                break;

            default: // Append characters to E.cmdbuf
                if (isprint(c)) {
                    abAppend(&E.cmdbuf, (char *) &c, 1);
                    editorSetStatusMsg(":%s", E.cmdbuf.b);
                }
        }


    } else {
        char errbuf[80];
        sprintf(errbuf, "E.mode not recognised: %d\n", E.mode);
        die(errbuf);
    }
}

/*** init ***/

void initEditor() {
    E.mode = NORMAL_MODE;
    E.cmdbuf.b = NULL;
    E.cmdbuf.len = 0;
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.filename = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    // Make room for a 1 line status bar and 1 line message
    E.screenrows -= 2;
}

int main(int argc, char **argv) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMsg("Welcome");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    };

    return 0;
}
