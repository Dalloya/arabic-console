/* Arabic console input shaper.  Tables are generated from the Python engine. */
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>     /* WO85: filesystem completion (readdir) */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#ifdef AR_TYPE_SHELL
#include <sys/stat.h>   /* WO79 D1: struct stat for the home-inode compare */
#endif
#include <linux/input.h>
#include <linux/kd.h>
#include "ar_type_tables.h"
#include "ar_type_latin.h"
enum { ISOL, INIT, MEDI, FINA, MAX_LINES = 512, MAX_COLS = 512 };
struct Line { unsigned int cp[MAX_COLS]; size_t len; };
static struct Line lines[MAX_LINES];
static size_t line_count = 1, cursor_line, cursor_col, displayed_rows = 1;
static size_t terminal_cols = 80, terminal_rows = 25;
static size_t justify_width;
static size_t visual_origin[MAX_COLS]; /* hit-test source of each painted cell */
static void load_justify_width(void) {
 const char *e=getenv("AR_JUSTIFY_WIDTH");char *end;unsigned long n;
 if(!e||!*e)return;
 errno=0;n=strtoul(e,&end,10);
 if(errno||*end||!n||n>MAX_COLS){fprintf(stderr,"ar-type: AR_JUSTIFY_WIDTH must be 1..%d\n",MAX_COLS);exit(2);}
 justify_width=(size_t)n;
}
static size_t anchor_line, anchor_col;
/* Task 36/51: 0 = Arabic typing (the default), 1 = Latin typing.  Toggled by
 * the ESC + space chord - Super+Space, which the keymap delivers ESC-prefixed
 * only while the kernel's Meta-bit is on - and by the single byte 0x00
 * (Ctrl+Space), the chord the console actually delivers: kbd 2.6.4's
 * kbd_mode has no meta option, so Super+Space arrives as a bare space. */
static int latin_mode;
static unsigned int clipboard[MAX_LINES * MAX_COLS + MAX_LINES];
static size_t clipboard_len;
static struct termios saved_termios; static int tty_fd = -1, have_termios;
/* Optional private byte stream.  It feeds the exact same byte parser as the
 * VT keyboard; unset means no listener and preserves the legacy behavior. */
static int input_listen_fd = -1, input_client_fd = -1;
static char input_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int input_socket_start(void) {
 const char *path=getenv("ARABIC_CONSOLE_INPUT_SOCKET");
 struct sockaddr_un sa; struct stat st,tty_st; mode_t oldmask;
 if(!path||!*path)return 0;
 if(strlen(path)>=sizeof sa.sun_path){errno=ENAMETOOLONG;return -1;}
 if(lstat(path,&st)==0){if(!S_ISSOCK(st.st_mode)){errno=EEXIST;return -1;}if(unlink(path)<0)return -1;}
 else if(errno!=ENOENT)return -1;
 input_listen_fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
 if(input_listen_fd<0)return -1;
 memset(&sa,0,sizeof sa);sa.sun_family=AF_UNIX;strcpy(sa.sun_path,path);
 oldmask=umask(0007);
 if(bind(input_listen_fd,(struct sockaddr *)&sa,sizeof sa)<0){umask(oldmask);close(input_listen_fd);input_listen_fd=-1;return -1;}
 umask(oldmask);
 if(fstat(tty_fd,&tty_st)<0||chmod(path,0660)<0||chown(path,(uid_t)-1,tty_st.st_gid)<0||listen(input_listen_fd,SOMAXCONN)<0){int e=errno;close(input_listen_fd);input_listen_fd=-1;unlink(path);errno=e;return -1;}
 strcpy(input_socket_path,path);
 return 0;
}
static void input_socket_stop(void) {
 if(input_client_fd>=0)close(input_client_fd);
 if(input_listen_fd>=0)close(input_listen_fd);
 input_client_fd=input_listen_fd=-1;
 if(input_socket_path[0])unlink(input_socket_path);
}
/* Drain every queued connection.  The most recent client replaces even a
 * half-open one, and an idle client never disables acceptance of new input. */
static void input_socket_accept_newest(void) {
 int next;
 if(input_listen_fd<0)return;
 for(;;){
  next=accept4(input_listen_fd,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
  if(next<0){if(errno==EINTR)continue;break;}
  if(input_client_fd>=0)close(input_client_fd);
  input_client_fd=next;
 }
}
static int input_socket_byte(unsigned char *b) {
 ssize_t n;
 if(input_client_fd<0)return 0;
 n=read(input_client_fd,b,1);
 if(n==1)return 1;
 if(n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)){
  close(input_client_fd);input_client_fd=-1;
 }
 return 0;
}
enum PointerKind { POINTER_NONE, POINTER_ABS, POINTER_REL, POINTER_MICE };
struct Cell { long x, y; };
/* This is deliberately side-effect free: it is shared by the event reader and
 * --mouse-test, where raw absolute and relative packets are replayed. */
struct PointerMap { int absolute; long raw_x, raw_y, min_x, max_x, min_y, max_y;
                    long fb_w, fb_h, cols, rows; struct Cell previous; };
static struct Cell map_pointer(const struct PointerMap *m) {
 struct Cell r=m->previous; long x,y;
 if(!m->cols||!m->rows)return r;
 if(m->absolute) {
  x=m->max_x>m->min_x?(m->raw_x-m->min_x)*m->cols/(m->max_x-m->min_x+1):0;
  y=m->max_y>m->min_y?(m->raw_y-m->min_y)*m->rows/(m->max_y-m->min_y+1):0;
 } else {
  long cw=m->fb_w/m->cols, ch=m->fb_h/m->rows;
  if(cw<1)cw=1;if(ch<1)ch=1; x=r.x+m->raw_x/cw; y=r.y+m->raw_y/ch;
 }
 if(x<0)x=0;if(y<0)y=0;if(x>=m->cols)x=m->cols-1;if(y>=m->rows)y=m->rows-1;
 r.x=x;r.y=y;return r;
}
enum DragEvent { DRAG_LEFT_DOWN, DRAG_MOVE, DRAG_LEFT_UP, DRAG_MIDDLE };
struct DragState { struct Cell anchor, current; int down, moved; };
/* Also pure.  A release retains a dragged range; a stationary click requests
 * selection clearing and cursor placement. */
static struct DragState drag_step(struct DragState s, enum DragEvent e, struct Cell at) {
 if(e==DRAG_LEFT_DOWN){s.anchor=s.current=at;s.down=1;s.moved=0;}
 else if(e==DRAG_MOVE&&s.down){if(at.x!=s.current.x||at.y!=s.current.y)s.moved=1;s.current=at;}
 else if(e==DRAG_LEFT_UP&&s.down){if(at.x!=s.current.x||at.y!=s.current.y)s.moved=1;s.current=at;s.down=0;}
 return s;
}
enum { MAX_POINTER_DEVICES = 9, MAX_EVENT_POINTER_DEVICES = 8 };
struct PointerDevice {
 int fd; enum PointerKind kind; char node[64];
 dev_t node_rdev; ino_t node_ino; int needs_reopen;
 struct input_id identity_id; char identity_name[128],identity_uniq[128];
 long min_x,max_x,min_y,max_y,abs_x,abs_y;
 unsigned char packet[3]; size_t packet_have;
 int left_down;
};
static struct PointerDevice pointer_devices[MAX_POINTER_DEVICES];
static size_t pointer_device_count;
static long pointer_scan_due_ms;
static struct Cell pointer_cell; static int pointer_visible, pointer_down, pointer_debug, pointer_structural, pointer_draw_enabled = 1; static unsigned int pointer_arrow_glyph;
/* The cell the arrow is painted at on the screen, or (-1,-1) while nothing is
 * painted yet.  Painting is coalesced: bytes are emitted only when this cell
 * changes, so a burst of events that stays inside one cell paints nothing. */
static struct Cell painted_cell = {-1,-1};
/* The painted marker can be disabled per instance without disabling pointer
 * events, selection, click positioning, or middle-click paste.  Unset keeps
 * the historical behavior; only the explicit value "0" turns painting off. */
static void configure_pointer_drawing(void) {
 const char *value=getenv("AR_TYPE_DRAW_POINTER");
 pointer_draw_enabled=!(value&&strcmp(value,"0")==0);
}
static int pointer_marker_visible(void) { return pointer_draw_enabled&&pointer_visible; }
/* Task 60: the console row the typing frame starts at, learned at startup
 * with a cursor-position report (ESC[6n -> ESC[r;cR, answered by the real
 * console).  The profile prints the banner before launching the editor and
 * leaves the cursor several rows down: the typing frame's row 0 belongs
 * there, but the pointer's model rows are absolute console rows, so every
 * pointer paint must move from the cursor's TRUE console row (console_row),
 * not from a row the editor assumed.  On a terminal that does not answer
 * (a pty), the origin stays 0 and the behavior is exactly as before task
 * 60.  The reply is consumed by the input loop, which also times the query
 * out. */
static long screen_origin, console_row;
static int cpr_pending; static long cpr_sent_ms; static int startup_arrow_done;
/* Task 60: the rows ABOVE the typing frame are the profile's banner, which
 * the editor does not own.  Snapshot them at startup (read from /dev/vcsu
 * once the CPR reply has located the frame) and redraw them from the
 * snapshot, so the arrow can visit the banner rows without erasing the
 * hints.  Unavailable (a pty, say): those rows redraw as empty rows,
 * exactly as before. */
static unsigned int banner_cells[32][MAX_COLS];
static size_t banner_len[32], banner_count;
/* The legacy aggregate is the only poison (task 46).  /dev/input/mice is the
 * aggregate of the very evdev nodes opened above, so once any evdev node has
 * delivered a single event, the aggregate's motion is dropped - reading it
 * would move the same physical mouse a second time (that was the task-44
 * drift, and it must stay dropped).  Its packets are still read and its
 * buttons still register.  Both event types are accepted from the evdev
 * nodes themselves: an absolute event SETS the position (and so re-anchors
 * any accumulated relative error whenever it arrives), a relative event
 * MOVES it from the current cell, scaled by the framebuffer/cell size and
 * clamped.  There is no session-long authority and no re-arm rule:
 * virt-viewer sends absolute motion while the pointer is ungrabbed and
 * relative motion while it is grabbed, so an "absolute wins forever" rule
 * would freeze the arrow exactly when it is grabbed.  Buttons are never
 * gated: a press from any opened device still registers and starts a drag at
 * the current cell, and a button from an ignored movement source never moves
 * the pointer by itself. */
static int pointer_evdev_delivered;
/* Task 52, test-only: sweep8 emits one @AR-TYPE52P record per coalesced paint
 * (test_paint_marker), numbered by the count of accepted movement events
 * (test_event_count) so the harness can align paint with model per event. */
static int test_paint_marker; static long test_event_count;
/* All terminal output is staged here and flushed with a single write, so an
 * erase+draw pair travels as one ordered burst and a repaint is one syscall
 * instead of dozens. */
static char outbuf[65536]; static size_t outlen; static long out_bytes_total;
static long framebuffer_w,framebuffer_h; static struct DragState drag;
static int insert_cp(unsigned int cp);
static int split_line(void);
static void emit_paint_marker(void);
static void emit_test_marker(void);
static size_t line_rows(size_t row);
static void restore_terminal(void) { if (have_termios) tcsetattr(tty_fd, TCSANOW, &saved_termios); }
static void on_signal(int sig) { input_socket_stop(); restore_terminal(); signal(sig, SIG_DFL); raise(sig); }
static const struct ArabicLetter *letter(unsigned int cp) { size_t i; for (i=0;i<ARABIC_LETTER_COUNT;++i) if (arabic_letters[i].base==cp) return &arabic_letters[i]; return NULL; }
static int transparent(unsigned int cp) { size_t i; for (i=0;i<TRANSPARENT_CODEPOINT_COUNT;++i) if (transparent_codepoints[i]==cp) return 1; return 0; }
/* Task 36: Latin-mode translation.  A codepoint the Arabic layer delivered
 * maps to the US character of the same physical key and level - a plain
 * letter to the US lowercase, the Shift level (a mark) to the US capital,
 * Arabic-Indic digits to ASCII.  The table is generated from the keymap's
 * own sources and cross-checked against the built map, so the two cannot
 * drift.  The buffer stores the translated codepoint; the mode only changes
 * what a keystroke becomes. */
static int translate_latin(unsigned int *cp) { size_t i; for (i=0;i<ARABIC_LATIN_PAIR_COUNT;++i) if (arabic_latin_pairs[i].arabic==*cp) { *cp=arabic_latin_pairs[i].latin; return 1; } return 0; }
/* Work order R1-D1: the freed physical b levels use private sentinels so
 * Latin mode can recover b/B while Arabic mode still consumes nothing. */
static int translate_freed_b(unsigned int *cp) {
 if (*cp == 0xE100) { if (latin_mode) *cp = 'b'; else return 0; return 1; }
 if (*cp == 0xE101) { if (latin_mode) *cp = 'B'; else return 0; return 1; }
 return -1;
}
/* Task 45: the lam-alef ligature.  cp is an ALEF variant of the pair; the
 * isolated and final glyphs are returned (the alef never joins forward, so
 * there is no medial and no lam form beside it). */
static int lam_alef_variant(unsigned int cp,unsigned int *isol,unsigned int *fina) { size_t i;for(i=0;i<LAM_ALEF_COUNT;++i)if(lam_alef_table[i].alef==cp){if(isol)*isol=lam_alef_table[i].isol;if(fina)*fina=lam_alef_table[i].fina;return 1;}return 0; }
struct DrawCodepoint { unsigned int logical, terminal, connector; };
/* The kernel console drops combining marks; its font supplies printable aliases. */
static const struct DrawCodepoint dedicated_mark_cells[] = {
 {0x064B,0xE000,0xE010},{0x064C,0xE001,0xE011},{0x064D,0xE002,0xE012},
 {0x064E,0xE003,0xE013},{0x064F,0xE004,0xE014},{0x0650,0xE005,0xE015},
 {0x0651,0xE006,0xE016},{0x0652,0xE007,0xE017},{0x0670,0xE008,0xE018},
};
static unsigned int draw_codepoint(unsigned int cp,int connector) { size_t i; for(i=0;i<sizeof(dedicated_mark_cells)/sizeof(dedicated_mark_cells[0]);++i) if(dedicated_mark_cells[i].logical==cp)return connector?dedicated_mark_cells[i].connector:dedicated_mark_cells[i].terminal; return cp; }
/* Task 54: the six shadda+vowel stacks.  A shadda (U+0651) IMMEDIATELY
 * followed by one of the six vowels below collapses with it into ONE cell:
 * U+E030-U+E035, or the connector variants U+E038-U+E03D when the letter the
 * marks belong to (the one before the shadda) joins forward - the same rule
 * the standalone marks use.  The sukun (U+0652) and the superscript alef
 * (U+0670) never combine, and a lone shadda, a lone vowel, or any other
 * ordering (a vowel before a shadda, say) keeps one cell per mark. */
static int shadda_combo_vowel(unsigned int cp) { return cp==0x064E||cp==0x064F||cp==0x0650||cp==0x064B||cp==0x064C||cp==0x064D; }
static unsigned int shadda_combo_codepoint(unsigned int vowel,int connector) { static const unsigned int plain[6]={0xE030,0xE031,0xE032,0xE033,0xE034,0xE035},conn[6]={0xE038,0xE039,0xE03A,0xE03B,0xE03C,0xE03D};
 int idx=vowel==0x064E?0:vowel==0x064F?1:vowel==0x0650?2:vowel==0x064B?3:vowel==0x064C?4:5; return (connector?conn:plain)[idx]; }
static char joining_type(unsigned int cp) { size_t i; for(i=0;i<ARABIC_JOINING_COUNT;++i)if(arabic_joining[i].base==cp)return arabic_joining[i].joining;return 'U'; }
static unsigned int previous_joining_cp(const struct Line *line, size_t start, size_t index) { while(index>start){--index;if(!transparent(line->cp[index]))return line->cp[index];}return 0; }
static unsigned int next_joining_cp(const struct Line *line, size_t end, size_t index) { for(++index;index<end;++index)if(!transparent(line->cp[index]))return line->cp[index];return 0; }
/* The logical column of the first non-transparent character after index; only
 * called when next_joining_cp has already found one. */
static size_t next_joining_index(const struct Line *line, size_t end, size_t index) { for(++index;index<end;++index)if(!transparent(line->cp[index]))return index;return 0; }
static int flush_out(void) { ssize_t done=0; while(done<(ssize_t)outlen){ssize_t r=write(tty_fd,outbuf+done,outlen-(size_t)done);if(r<=0){outlen=0;return -1;}done+=r;} out_bytes_total+=(long)outlen;outlen=0;return 0; }
static int out_append(const char *s,size_t n) { if(outlen+n>sizeof(outbuf)){if(flush_out())return -1;if(n>sizeof(outbuf))return -1;}memcpy(outbuf+outlen,s,n);outlen+=n;return 0; }
static int putcp(unsigned int cp) { unsigned char b[4]; size_t n;
 if(cp<=0x7f)b[0]=cp,n=1; else if(cp<=0x7ff)b[0]=0xc0|(cp>>6),b[1]=0x80|(cp&63),n=2; else if(cp<=0xffff)b[0]=0xe0|(cp>>12),b[1]=0x80|((cp>>6)&63),b[2]=0x80|(cp&63),n=3; else b[0]=0xf0|(cp>>18),b[1]=0x80|((cp>>12)&63),b[2]=0x80|((cp>>6)&63),b[3]=0x80|(cp&63),n=4; return out_append((const char*)b,n); }
static int bytes(const char *s) { size_t n=strlen(s); return out_append(s,n); }
static int bit_set(const unsigned long *bits,int bit) { return (bits[bit/(8*sizeof(*bits))]>>(bit%(8*sizeof(*bits))))&1; }
static void read_framebuffer_size(void) { FILE *f=fopen("/sys/class/graphics/fb0/virtual_size","r");unsigned long w,h;
 if(f&&fscanf(f,"%lu,%lu",&w,&h)==2){framebuffer_w=(long)w;framebuffer_h=(long)h;}if(f)fclose(f);
}
/* The console font is the authority on whether the private-use arrow slot
 * exists.  On a VT this reads the loaded charmap; where the query is not
 * available (a pty, say) the arrow is assumed present so the console path is
 * unaffected. */
static long now_ms(void); /* task 64: forward decl - the paint trace stamps lines */
/* Task 64 diagnostic: one timestamped line per pointer-paint bookkeeping step,
 * appended to the same /tmp/ar-type-pointer.log the calibration log uses, so a
 * console-only trail can be reconstructed from the log alone.  Gated by
 * AR_TYPE_POINTER_DEBUG exactly like the calibration lines; never written to
 * the screen, never affects the escape stream. */
static void ptr_trace(const char *fmt, ...) { FILE *f;va_list ap;static long anchor;
 if(!pointer_debug)return;
 f=fopen("/tmp/ar-type-pointer.log","a");if(!f)return;
 if(!anchor)anchor=now_ms();
 fprintf(f,"[%+8ld] ",now_ms()-anchor);
 va_start(ap,fmt);vfprintf(f,fmt,ap);va_end(ap);
 fputc('\n',f);fclose(f);
}
static int font_has_cp(unsigned int want) { struct consolefontdesc cfd; unsigned short map[512]; size_t i;
 cfd.charcount=512;cfd.chardata=(char*)map;
 if(ioctl(tty_fd,GIO_FONTX,&cfd))return 1;
 for(i=0;i<cfd.charcount&&i<512;++i)if(map[i]==(unsigned short)want)return 1;
 return 0;
}
static void pointer_log(void) { FILE *f=fopen("/tmp/ar-type-pointer.log","w");size_t i;
 if(!f)return;fprintf(f,"pointer devices opened:");
 for(i=0;i<pointer_device_count;++i){struct PointerDevice *d=&pointer_devices[i];
  fprintf(f," %s(%s",d->node,d->kind==POINTER_ABS?"abs":d->kind==POINTER_REL?"rel":"mice");
  if(d->kind==POINTER_ABS)fprintf(f," range=%ld..%ld,%ld..%ld",d->min_x,d->max_x,d->min_y,d->max_y);
  fputc(')',f);
 } fputc('\n',f);fclose(f);
}
/* Calibration log: one appended line per pointer event while
 * AR_TYPE_POINTER_DEBUG is set, so the hand-to-cell mapping can be corrected
 * from measurements.  Nothing is ever written to the screen. */
static void pointer_debug_log(struct PointerDevice *d,const struct input_event *ev,struct Cell c) { FILE *f;const char *kind;
 if(!pointer_debug)return;
 f=fopen("/tmp/ar-type-pointer.log","a");if(!f)return;
 /* Provenance matters on the VM: an abs node reporting relative motion
  * (virt-viewer grabbed) logs "rel" so its lines can be told from its
  * absolute ones, and the aggregate always logs "mice". */
 kind=d->kind==POINTER_MICE?"mice":ev->type==EV_ABS?"abs":ev->type==EV_REL?"rel":d->kind==POINTER_ABS?"abs":"rel";
 if(ev->type==EV_ABS)
  fprintf(f,"%s %s raw=(%ld,%ld) range=%ld..%ld,%ld..%ld fb=%ldx%ld geo=%ldx%ld cell=(%ld,%ld)\n",d->node,kind,d->abs_x,d->abs_y,d->min_x,d->max_x,d->min_y,d->max_y,framebuffer_w,framebuffer_h,(long)terminal_cols,(long)terminal_rows,c.x,c.y);
 else if(ev->type==EV_REL)
  fprintf(f,"%s %s raw=(%d,%d) range=none fb=%ldx%ld geo=%ldx%ld cell=(%ld,%ld)\n",d->node,kind,ev->code==REL_X?ev->value:0,ev->code==REL_Y?ev->value:0,framebuffer_w,framebuffer_h,(long)terminal_cols,(long)terminal_rows,c.x,c.y);
 else
  fprintf(f,"%s %s key=%d value=%d fb=%ldx%ld geo=%ldx%ld cell=(%ld,%ld)\n",d->node,kind,ev->code,ev->value,framebuffer_w,framebuffer_h,(long)terminal_cols,(long)terminal_rows,c.x,c.y);
 fclose(f);
}
static int add_pointer_device(int fd,enum PointerKind kind,const char *node) { struct PointerDevice *d;struct stat st;
 if(pointer_device_count>=MAX_POINTER_DEVICES){close(fd);return 0;}d=&pointer_devices[pointer_device_count];memset(d,0,sizeof(*d));d->fd=fd;d->kind=kind;snprintf(d->node,sizeof(d->node),"%s",node);
 if(fstat(fd,&st)==0){d->node_rdev=st.st_rdev;d->node_ino=st.st_ino;}
 (void)ioctl(fd,EVIOCGID,&d->identity_id);(void)ioctl(fd,EVIOCGNAME(sizeof(d->identity_name)),d->identity_name);(void)ioctl(fd,EVIOCGUNIQ(sizeof(d->identity_uniq)),d->identity_uniq);
 if(kind==POINTER_ABS){struct input_absinfo ax,ay;if(ioctl(fd,EVIOCGABS(ABS_X),&ax)||ioctl(fd,EVIOCGABS(ABS_Y),&ay)){close(fd);return 0;}d->min_x=ax.minimum;d->max_x=ax.maximum;d->min_y=ay.minimum;d->max_y=ay.maximum;d->abs_x=ax.value;d->abs_y=ay.value;}
 ++pointer_device_count;return 1;
}
/* Keep every usable route: hypervisors can advertise several absolute devices,
 * while routing real motion to a later one. */
static void discover_pointer(void) { int n,fd;unsigned long ev[(EV_MAX+8*sizeof(long))/(8*sizeof(long))];
 if(getenv("AR_TYPE_DISABLE_POINTER")){pointer_log();return;}
 for(n=0;n<64;++n){char name[64];unsigned long abs[(ABS_MAX+8*sizeof(long))/(8*sizeof(long))],rb[(REL_MAX+8*sizeof(long))/(8*sizeof(long))];
  snprintf(name,sizeof(name),"/dev/input/event%d",n);fd=open(name,O_RDONLY|O_NONBLOCK);if(fd<0)continue;
  memset(ev,0,sizeof(ev));memset(abs,0,sizeof(abs));memset(rb,0,sizeof(rb));
  if(ioctl(fd,EVIOCGBIT(0,sizeof(ev)),ev)>=0) {
   if(bit_set(ev,EV_ABS)&&ioctl(fd,EVIOCGBIT(EV_ABS,sizeof(abs)),abs)>=0&&bit_set(abs,ABS_X)&&bit_set(abs,ABS_Y)){if(pointer_device_count<MAX_EVENT_POINTER_DEVICES)(void)add_pointer_device(fd,POINTER_ABS,name);else close(fd);continue;}
   if(bit_set(ev,EV_REL)&&ioctl(fd,EVIOCGBIT(EV_REL,sizeof(rb)),rb)>=0&&bit_set(rb,REL_X)&&bit_set(rb,REL_Y)){if(pointer_device_count<MAX_EVENT_POINTER_DEVICES)(void)add_pointer_device(fd,POINTER_REL,name);else close(fd);continue;}
  }close(fd);
 }
 fd=open("/dev/input/mice",O_RDONLY|O_NONBLOCK);if(fd>=0)(void)add_pointer_device(fd,POINTER_MICE,"/dev/input/mice");
 pointer_log();
}
static int pointer_node_kind(int fd,enum PointerKind *kind) {
 unsigned long ev[(EV_MAX+8*sizeof(long))/(8*sizeof(long))];
 unsigned long abs[(ABS_MAX+8*sizeof(long))/(8*sizeof(long))];
 unsigned long rb[(REL_MAX+8*sizeof(long))/(8*sizeof(long))];
 memset(ev,0,sizeof(ev));memset(abs,0,sizeof(abs));memset(rb,0,sizeof(rb));
 if(ioctl(fd,EVIOCGBIT(0,sizeof(ev)),ev)<0)return 0;
 if(bit_set(ev,EV_ABS)&&ioctl(fd,EVIOCGBIT(EV_ABS,sizeof(abs)),abs)>=0&&bit_set(abs,ABS_X)&&bit_set(abs,ABS_Y)){*kind=POINTER_ABS;return 1;}
 if(bit_set(ev,EV_REL)&&ioctl(fd,EVIOCGBIT(EV_REL,sizeof(rb)),rb)>=0&&bit_set(rb,REL_X)&&bit_set(rb,REL_Y)){*kind=POINTER_REL;return 1;}
 return 0;
}
static int pointer_identity_open(int fd,dev_t old_rdev,ino_t old_ino) {
 struct stat st;return fstat(fd,&st)==0&&(st.st_rdev!=old_rdev||st.st_ino!=old_ino);
}
static int pointer_identity_present(dev_t rdev,ino_t ino) {
 size_t i;for(i=0;i<pointer_device_count;++i)if(pointer_devices[i].fd>=0&&pointer_devices[i].node_rdev==rdev&&pointer_devices[i].node_ino==ino)return 1;return 0;
}
static int pointer_same_device(struct PointerDevice *old,int fd) {
 struct input_id id;char name[128]={0},uniq[128]={0};
 if(ioctl(fd,EVIOCGID,&id)<0||ioctl(fd,EVIOCGNAME(sizeof(name)),name)<0)return 0;
 (void)ioctl(fd,EVIOCGUNIQ(sizeof(uniq)),uniq);
 return id.bustype==old->identity_id.bustype&&id.vendor==old->identity_id.vendor&&
        id.product==old->identity_id.product&&id.version==old->identity_id.version&&
        strcmp(name,old->identity_name)==0&&strcmp(uniq,old->identity_uniq)==0;
}
/* Re-enumeration is only called after a device has been dropped and no more
 * often than once per second by the owning poll loop.  A path is only a scan
 * location: a reopened device must be a valid pointer node with a new
 * (st_rdev, inode) identity, so udev's reuse of eventNN cannot resurrect the
 * stale file description. */
static void reopen_pointer_devices(void) {
 size_t slot;int n,fd,opened=0;
 for(slot=0;slot<pointer_device_count;++slot){struct PointerDevice *old=&pointer_devices[slot];
  if(!old->needs_reopen)continue;
  if(old->kind==POINTER_MICE){struct stat st;fd=open("/dev/input/mice",O_RDONLY|O_NONBLOCK);
   if(fd>=0&&fstat(fd,&st)==0&&(st.st_rdev!=old->node_rdev||st.st_ino!=old->node_ino)){
    old->fd=fd;old->node_rdev=st.st_rdev;old->node_ino=st.st_ino;old->needs_reopen=0;snprintf(old->node,sizeof(old->node),"/dev/input/mice");opened=1;
   }else if(fd>=0)close(fd);
  }else for(n=0;n<64;++n){char name[64];enum PointerKind kind;struct stat st;
   snprintf(name,sizeof(name),"/dev/input/event%d",n);fd=open(name,O_RDONLY|O_NONBLOCK);if(fd<0)continue;
   if(fstat(fd,&st)==0&&pointer_node_kind(fd,&kind)&&kind==old->kind&&pointer_same_device(old,fd)&&
      (st.st_rdev!=old->node_rdev||st.st_ino!=old->node_ino)&&
      !pointer_identity_present(st.st_rdev,st.st_ino)){
    struct PointerDevice replacement=*old;replacement.fd=-1;replacement.needs_reopen=0;replacement.node_rdev=st.st_rdev;replacement.node_ino=st.st_ino;snprintf(replacement.node,sizeof(replacement.node),"%s",name);
    if(kind==POINTER_ABS){struct input_absinfo ax,ay;if(ioctl(fd,EVIOCGABS(ABS_X),&ax)||ioctl(fd,EVIOCGABS(ABS_Y),&ay)){close(fd);continue;}replacement.min_x=ax.minimum;replacement.max_x=ax.maximum;replacement.min_y=ay.minimum;replacement.max_y=ay.maximum;replacement.abs_x=ax.value;replacement.abs_y=ay.value;}
    replacement.fd=fd;*old=replacement;opened=1;break;
   }close(fd);
  }
 }
 if(opened)pointer_log();
}
static int pointer_device_is_real(const struct PointerDevice *d) {
 return !strncmp(d->node,"/dev/input/event",16)||!strcmp(d->node,"/dev/input/mice");
}
static void drop_pointer_device(struct PointerDevice *d) {
 if(d->fd>=0){int real_node=pointer_device_is_real(d);close(d->fd);d->fd=-1;d->needs_reopen=real_node;}
}
static int pointer_reopen_pending(void) {
 size_t i;for(i=0;i<pointer_device_count;++i)if(pointer_devices[i].needs_reopen)return 1;return 0;
}
static int pointer_poll_timeout(int key_timeout) {
 long now,remain;int scan_timeout;
 if(!pointer_reopen_pending())return key_timeout;
 now=now_ms();if(!pointer_scan_due_ms)pointer_scan_due_ms=now;
 remain=pointer_scan_due_ms-now;if(remain<0)remain=0;
 scan_timeout=remain>2147483647L?2147483647:(int)remain;
 return key_timeout<0||scan_timeout<key_timeout?scan_timeout:key_timeout;
}
static void pointer_scan_if_due(void) {
 long now;if(!pointer_reopen_pending())return;now=now_ms();
 if(!pointer_scan_due_ms||now>=pointer_scan_due_ms){reopen_pointer_devices();pointer_scan_due_ms=now+1000;}
}
static struct Cell pointer_map(const struct PointerDevice *d,long x,long y) { struct PointerMap m;
 m.absolute=d->kind==POINTER_ABS;m.raw_x=x;m.raw_y=y;m.min_x=d->min_x;m.max_x=d->max_x;m.min_y=d->min_y;m.max_y=d->max_y;
 m.fb_w=framebuffer_w;m.fb_h=framebuffer_h;m.cols=(long)terminal_cols;m.rows=(long)terminal_rows;m.previous=pointer_cell;return map_pointer(&m);
}
/* A relative event moves the pointer from its CURRENT cell by the delta,
 * scaled by the framebuffer/cell size, clamped - whatever the device kind.
 * virt-viewer emits relative motion through the very node that reports
 * absolute positions while ungrabbed, so mapping by event type, not by the
 * kind discovered at open time, is what lets the arrow follow the hand. */
static struct Cell pointer_map_delta(const struct PointerDevice *d,long dx,long dy) { struct PointerMap m;
 m.absolute=0;m.raw_x=dx;m.raw_y=dy;m.min_x=d->min_x;m.max_x=d->max_x;m.min_y=d->min_y;m.max_y=d->max_y;
 m.fb_w=framebuffer_w;m.fb_h=framebuffer_h;m.cols=(long)terminal_cols;m.rows=(long)terminal_rows;m.previous=pointer_cell;return map_pointer(&m);
}
/* Task 60: every cursor row movement is booked against the TRUE console
 * row.  The console clamps cursor moves at the screen edges, so the row the
 * editor believed could drift from the row the console actually holds
 * (measured: a 9-row startup offset, then a permanent 4-row one after the
 * first clamped paint); clamping here exactly the way the console does
 * keeps console_row real. */
/* Task 64: a console scroll (an LF at the bottom row) shifts every row on
 * screen up by one - the painted arrow included.  The erase target
 * (painted_cell) must follow the scroll, or the next pointer repaint erases
 * the row the arrow was AT, not the row it is ON: the one-row trail measured
 * after a frame scroll.  Called exactly once per scroll, at the LF that
 * causes it, so painted_cell keeps tracking the arrow's real row. */
static void console_scrolled(void){ if(screen_origin>0)screen_origin--; if(painted_cell.y>=0)painted_cell.y--; }
static int move_rows(long n) { long target=console_row+n;
 if(target<0)target=0;if(target>(long)terminal_rows-1)target=(long)terminal_rows-1;
 ptr_trace("move_rows n=%ld console_row %ld->%ld",n,console_row,target);
 console_row=target;
 while(n<0){if(bytes("\033[A"))return-1;++n;} while(n>0){if(bytes("\033[B"))return-1;--n;} return 0; }
static int has_selection(void) { return anchor_line != cursor_line || anchor_col != cursor_col; }
static int before(size_t l1,size_t c1,size_t l2,size_t c2) { return l1<l2 || (l1==l2 && c1<c2); }
static void selection_bounds(size_t *sl,size_t *sc,size_t *el,size_t *ec) { if(before(anchor_line,anchor_col,cursor_line,cursor_col)){*sl=anchor_line;*sc=anchor_col;*el=cursor_line;*ec=cursor_col;}else{*sl=cursor_line;*sc=cursor_col;*el=anchor_line;*ec=anchor_col;} }
static void clear_selection(void) { anchor_line=cursor_line; anchor_col=cursor_col; }
static int buffer_valid(void) { size_t row;if(!line_count||line_count>MAX_LINES)return 0;for(row=0;row<line_count;++row)if(lines[row].len>MAX_COLS)return 0;return 1; }
static int cursor_valid(void) { return buffer_valid()&&cursor_line<line_count&&cursor_col<=lines[cursor_line].len; }
/* Bounds are checked before deriving an offset or a copy size from them. */
static int selection_valid(size_t *sl,size_t *sc,size_t *el,size_t *ec) { if(!cursor_valid()||anchor_line>=line_count||anchor_col>lines[anchor_line].len)return 0;selection_bounds(sl,sc,el,ec);return *sl<=*el&&*el<line_count&&*sc<=lines[*sl].len&&*ec<=lines[*el].len; }
static void reject_selection(void) { if(cursor_valid())clear_selection(); }
static int selected(size_t row,size_t col) { size_t sl,sc,el,ec;if(!has_selection()||!selection_valid(&sl,&sc,&el,&ec))return 0;if(row<sl||row>el)return 0;if(sl==el)return col>=sc&&col<ec;if(row==sl)return col>=sc;if(row==el)return col<ec;return 1; }
static int delete_selection(void) { size_t sl,sc,el,ec,tail,remove,move;if(!has_selection())return 0;if(!selection_valid(&sl,&sc,&el,&ec)){reject_selection();return 0;}if(sl==el){struct Line *l=&lines[sl];move=l->len-ec;memmove(l->cp+sc,l->cp+ec,move*sizeof(*l->cp));l->len-=ec-sc;}else{struct Line *first=&lines[sl],*last=&lines[el];tail=last->len-ec;if(sc>MAX_COLS-tail){reject_selection();return 0;}memmove(first->cp+sc,last->cp+ec,tail*sizeof(*first->cp));first->len=sc+tail;remove=el-sl;move=line_count-el-1;memmove(&lines[sl+1],&lines[el+1],move*sizeof(lines[0]));line_count-=remove;}cursor_line=sl;cursor_col=sc;clear_selection();return 1; }
static void copy_selection(void) { size_t sl,sc,el,ec,row,i,needed=0,cap=sizeof(clipboard)/sizeof(clipboard[0]);if(!has_selection())return;if(!selection_valid(&sl,&sc,&el,&ec)){reject_selection();return;}for(row=sl;row<=el;++row){size_t from=row==sl?sc:0,to=row==el?ec:lines[row].len,count=to-from;if(count>cap-needed||(row!=el&&count==cap-needed)){reject_selection();return;}needed+=count;if(row!=el)++needed;}clipboard_len=0;for(row=sl;row<=el;++row){size_t from=row==sl?sc:0,to=row==el?ec:lines[row].len;for(i=from;i<to;++i)clipboard[clipboard_len++]=lines[row].cp[i];if(row!=el)clipboard[clipboard_len++]=0x0a;} }
static int paste_fits(void) { size_t lens[MAX_LINES],n,row,i,col;size_t sl,sc,el,ec;if(!cursor_valid()||clipboard_len>sizeof(clipboard)/sizeof(clipboard[0]))return 0;n=line_count;for(row=0;row<n;++row)lens[row]=lines[row].len;row=cursor_line;col=cursor_col;if(has_selection()){if(!selection_valid(&sl,&sc,&el,&ec)){reject_selection();return 0;}if(sl==el)lens[sl]-=ec-sc;else{size_t tail=lens[el]-ec,remove=el-sl;lens[sl]=sc+tail;memmove(&lens[sl+1],&lens[el+1],(n-el-1)*sizeof(lens[0]));n-=remove;}row=sl;col=sc;}for(i=0;i<clipboard_len;++i){if(clipboard[i]==0x0a){if(n==MAX_LINES)return 0;memmove(&lens[row+2],&lens[row+1],(n-row-1)*sizeof(lens[0]));lens[row+1]=lens[row]-col;lens[row]=col;++n;++row;col=0;}else{if(lens[row]==MAX_COLS)return 0;++lens[row];++col;}}return 1; }
static int paste_clipboard(void) { size_t i;int changed=0;if(!paste_fits())return 0;if(has_selection()){if(!delete_selection())return 0;changed=1;}for(i=0;i<clipboard_len;++i){if(clipboard[i]==0x0a){if(!split_line())return 0;}else if(!insert_cp(clipboard[i]))return 0;changed=1;}clear_selection();return changed; }
static int insert_cp(unsigned int cp) { struct Line *l;if(!cursor_valid())return 0;l=&lines[cursor_line];if(l->len>=MAX_COLS)return 0;memmove(l->cp+cursor_col+1,l->cp+cursor_col,(l->len-cursor_col)*sizeof(*l->cp));l->cp[cursor_col++]=cp;++l->len;return 1; }
static int split_line(void) { struct Line *l;size_t move;if(!cursor_valid()||line_count>=MAX_LINES)return 0;move=line_count-cursor_line-1;memmove(&lines[cursor_line+2],&lines[cursor_line+1],move*sizeof(lines[0]));l=&lines[cursor_line];lines[cursor_line+1].len=l->len-cursor_col;memcpy(lines[cursor_line+1].cp,l->cp+cursor_col,lines[cursor_line+1].len*sizeof(*l->cp));l->len=cursor_col;++line_count;++cursor_line;cursor_col=0;clear_selection();return 1; }
static int backspace_cp(void) { struct Line *l;if(!cursor_valid())return 0;l=&lines[cursor_line];if(cursor_col){memmove(l->cp+cursor_col-1,l->cp+cursor_col,(l->len-cursor_col)*sizeof(*l->cp));--cursor_col;--l->len;clear_selection();return 1;}if(!cursor_line)return 0; {struct Line *up=&lines[cursor_line-1];size_t old=up->len,kept=old?old-1:0,move;if(kept>MAX_COLS-l->len)return 0;up->len=kept;memcpy(up->cp+up->len,l->cp,l->len*sizeof(*l->cp));up->len+=l->len;cursor_col=old?old-1:0;--cursor_line;move=line_count-cursor_line-2;memmove(&lines[cursor_line+1],&lines[cursor_line+2],move*sizeof(lines[0]));--line_count;clear_selection();return 1;} }
static int delete_cp(void) { struct Line *l;size_t move;if(!cursor_valid())return 0;l=&lines[cursor_line];if(cursor_col<l->len){memmove(l->cp+cursor_col,l->cp+cursor_col+1,(l->len-cursor_col-1)*sizeof(*l->cp));--l->len;clear_selection();return 1;}if(cursor_line+1>=line_count||lines[cursor_line+1].len>MAX_COLS-l->len)return 0;memcpy(l->cp+l->len,lines[cursor_line+1].cp,lines[cursor_line+1].len*sizeof(*l->cp));l->len+=lines[cursor_line+1].len;move=line_count-cursor_line-2;memmove(&lines[cursor_line+1],&lines[cursor_line+2],move*sizeof(lines[0]));--line_count;clear_selection();return 1; }
/* Build the already-existing visual permutation before placing it on the grid. */
/* A transparent mark receives a connector only if the preceding logical
 * letter has a forward join (its shaped form is initial or medial). */
static int mark_after_forward_join(const struct Line *l,size_t start,size_t end,size_t index) { size_t prior=index;const struct ArabicLetter *cur,*next;unsigned int nxt;
 while(prior>start){--prior;if(!transparent(l->cp[prior]))break;}
 if(prior==index||!letter(l->cp[prior]))return 0;cur=letter(l->cp[prior]);nxt=next_joining_cp(l,end,prior);
 /* Task 45: a lam that ligates with its following alef never joins forward,
  * so a mark between the two takes the plain cell, not the connector. */
 if(l->cp[prior]==0x0644&&lam_alef_variant(nxt,NULL,NULL))return 0;
 next=letter(nxt);
 return next&&next->form[FINA].unicode&&cur->form[INIT].unicode;
}
static int visual_joins_right(unsigned int cp) { size_t j;
 if(cp==0xFEF6||cp==0xFEF8||cp==0xFEFA||cp==0xFEFC)return 1; /* final lam-alef ligatures */
 for(j=0;j<ARABIC_LETTER_COUNT;++j)if(cp==arabic_letters[j].form[2].unicode||cp==arabic_letters[j].form[3].unicode)return 1;return 0; }
static int visual_joins_left(unsigned int cp) { size_t j;for(j=0;j<ARABIC_LETTER_COUNT;++j)if(cp==arabic_letters[j].form[1].unicode||cp==arabic_letters[j].form[2].unicode)return 1;return 0; }
/* Stretch one whole Arabic word at its leftmost visual connection.  The
 * inserted U+0640 cells own their neighbour's logical source for hit tests. */
static size_t justify_visual_word(size_t first,size_t count,size_t remaining,
                                  unsigned int *out,unsigned char *inverse,
                                  unsigned char *connector,size_t *cursor_visual,
                                  int cursor_here) {
 size_t i,extra,j;
 if(!justify_width||count-first>=justify_width)return count;
 extra=justify_width-(count-first);
 if(extra>MAX_COLS-count-remaining)return count;
 for(i=first+1;i<count;++i){
  if(!visual_joins_right(out[i-1])||!visual_joins_left(out[i]))continue;
  memmove(out+i+extra,out+i,(count-i)*sizeof(out[0]));
  memmove(inverse+i+extra,inverse+i,(count-i)*sizeof(inverse[0]));
  memmove(connector+i+extra,connector+i,(count-i)*sizeof(connector[0]));
  memmove(visual_origin+i+extra,visual_origin+i,(count-i)*sizeof(visual_origin[0]));
  for(j=0;j<extra;++j){out[i+j]=0x0640;inverse[i+j]=0;connector[i+j]=0;visual_origin[i+j]=visual_origin[i-1];}
  if(cursor_here&&*cursor_visual>=i)*cursor_visual+=extra;
  return count+extra;
 }
 return count;
}
static size_t build_cells(size_t row, unsigned int *out, unsigned char *inverse, unsigned char *connector,
                          size_t *cursor_visual) { struct Line *l=&lines[row];size_t i,start,end,cells=0,cur_visual=0,run_start,run_end;int ascii;
 if (latin_mode) {
  int only_latin=1;
  for (i=0; i<l->len; ++i)
   if (l->cp[i] > 0x7f) { only_latin=0; break; }
  if (only_latin) {
   /* Latin runs are LTR islands, including their spaces.  Arabic mode and
    * mixed-script lines keep the established word-reversed renderer below. */
   for (i=0; i<l->len; ++i) {
    if (row==cursor_line && i==cursor_col) cur_visual=cells;
    out[cells]=l->cp[i]; connector[cells]=0; visual_origin[cells]=i; inverse[cells++]=selected(row,i);
   }
   if (row==cursor_line && cursor_col==l->len) cur_visual=cells;
   if (row==cursor_line) *cursor_visual=cur_visual;
   return cells;
  }
 }
 end=l->len;while(end){size_t word_first=cells;int pure_arabic=1;start=end;while(start&&l->cp[start-1]!=0x20)--start;
 for(i=start;i<end;++i)if(l->cp[i]<0x80){pure_arabic=0;break;}
 /* Task 77 (finding 3): a word is a sequence of maximal ASCII and Arabic
  * runs.  Emit the runs last-to-first like the words around them, but keep
  * an ASCII run's own left-to-right order: Latin islands (paths, quotes,
  * %s\n) must read forward.  Joining context stays the whole word
  * (start..end), so Arabic shaping is unchanged. */
 for(run_end=end;run_end>start;run_end=run_start){run_start=run_end;ascii=l->cp[run_end-1]<0x80;
  while(run_start>start&&(l->cp[run_start-1]<0x80)==ascii)--run_start;
  if(ascii){for(i=run_start;i<run_end;++i){if(row==cursor_line&&i==cursor_col)cur_visual=cells;out[cells]=l->cp[i];connector[cells]=0;visual_origin[cells]=i;inverse[cells++]=selected(row,i);}continue;}
  for(i=run_end;i-->run_start;){const struct ArabicLetter *cur=letter(l->cp[i]);unsigned int shown=l->cp[i],covered=i;int combo=0;
  /* Task 54: a vowel whose cell belongs to the shadda immediately before it
   * has no cell of its own; the shadda emits the pair below. */
  if(shadda_combo_vowel(l->cp[i])&&i>start&&l->cp[i-1]==0x0651)continue;
  if(cur){unsigned int prev=previous_joining_cp(l,start,i),next=next_joining_cp(l,end,i),isol=0,fina=0;
   /* Task 45: this alef belongs to a lam-alef ligature; the lam's own cell
    * already carries the pair, so no cell is emitted for the alef. */
   if(lam_alef_variant(l->cp[i],NULL,NULL)&&prev==0x0644)continue;
   if(l->cp[i]==0x0644&&lam_alef_variant(next,&isol,&fina)){int jp=prev&&(joining_type(prev)=='D'||joining_type(prev)=='C');shown=jp?fina:isol;covered=next_joining_index(l,end,i);}
   else{const struct ArabicLetter *following=letter(next);int jp=cur->form[FINA].unicode&&prev&&(joining_type(prev)=='D'||joining_type(prev)=='C'),jn=cur->form[INIT].unicode&&((following&&following->form[FINA].unicode)||joining_type(next)=='C'),p=jp?(jn?MEDI:FINA):(jn?INIT:ISOL);if(cur->form[p].unicode)shown=cur->form[p].unicode;}
  }
  /* Task 54: a shadda immediately followed by one of the six vowels draws
   * the single stacked cell covering both marks - the connector variant
   * exactly when the standalone marks would take one. */
  if(l->cp[i]==0x0651&&i+1<end&&shadda_combo_vowel(l->cp[i+1])){shown=shadda_combo_codepoint(l->cp[i+1],mark_after_forward_join(l,start,end,i));covered=i+1;combo=1;}
  if(row==cursor_line&&(i==cursor_col||covered==cursor_col))cur_visual=cells;
  out[cells]=shown;connector[cells]=!combo&&transparent(l->cp[i])&&mark_after_forward_join(l,start,end,i);visual_origin[cells]=i;inverse[cells++]=selected(row,i);}}
 if(pure_arabic)cells=justify_visual_word(word_first,cells,start,out,inverse,connector,&cur_visual,row==cursor_line);
 if(!start)break;out[cells]=0x20;connector[cells]=0;visual_origin[cells]=start-1;inverse[cells++]=0;end=start-1;}/* RTL insertion boundaries run right-to-left as logical columns run left-to-right.  A ligature cell covers its lam's column and its alef's, so the cursor lands on the cell whose span holds cursor_col (end of line: after the last cell). */if(row==cursor_line)*cursor_visual=cursor_col<l->len?cells-cur_visual-1:cells;return cells; }
/* The renderer's visual order is word-reversed.  Keep this hit-test alongside
 * it, so a console cell always maps back to the logical selection model. */
static size_t visual_source(size_t row,size_t visual) {
 unsigned int cells[MAX_COLS];unsigned char inverse[MAX_COLS],connector[MAX_COLS];
 size_t count,unused=0;
 if(row>=line_count)return 0;
 count=build_cells(row,cells,inverse,connector,&unused);
 return visual<count?visual_origin[visual]:lines[row].len;
}
static void pointer_logical(struct Cell c,size_t *row,size_t *col) { size_t r,physical=0,count,chunk,base,n;unsigned int cells[MAX_COLS];unsigned char inverse[MAX_COLS],connector[MAX_COLS];size_t unused=0;
 *row=line_count-1;*col=lines[*row].len;
 for(r=0;r<line_count;++r){count=build_cells(r,cells,inverse,connector,&unused);for(chunk=0;chunk<line_rows(r);++chunk,++physical)if((long)physical==c.y){base=chunk*terminal_cols;n=count-base;if(n>terminal_cols)n=terminal_cols;if(c.x<(long)(terminal_cols-n)){*row=r;*col=lines[r].len;return;}if((size_t)c.x>=terminal_cols){*row=r;*col=lines[r].len;return;}*row=r;*col=visual_source(r,base+(size_t)c.x-(terminal_cols-n));return;}}
}
static void apply_pointer_drag(enum DragEvent event,struct Cell c) { size_t row,col,arow,acol;int moved;
 drag=drag_step(drag,event,c);pointer_cell=c;pointer_visible=1;pointer_logical(c,&row,&col);
 if(event==DRAG_LEFT_DOWN){pointer_down=1;cursor_line=row;cursor_col=col;clear_selection();return;}
 if(event==DRAG_MOVE&&pointer_down){pointer_logical(drag.anchor,&arow,&acol);if(before(arow,acol,row,col)){cursor_line=row;cursor_col=col<lines[row].len?col+1:col;}else{cursor_line=row;cursor_col=col;}anchor_line=arow;anchor_col=acol;return;}
 if(event==DRAG_LEFT_UP&&pointer_down){pointer_down=0;moved=drag.moved;if(!moved){cursor_line=row;cursor_col=col;clear_selection();}return;}
 if(event==DRAG_MIDDLE){(void)paste_clipboard();}
}
static size_t line_rows(size_t row) { unsigned int cells[MAX_COLS];unsigned char inv[MAX_COLS],conn[MAX_COLS];size_t unused=0,n=build_cells(row,cells,inv,conn,&unused);return n?(n+terminal_cols-1)/terminal_cols:1; }
static size_t physical_rows(void) { size_t i,n=0;for(i=0;i<line_count;++i)n+=line_rows(i);return n; }
/* The screen row the insertion cursor occupies; clamps to the last drawn row
 * exactly as redraw does, so the targeted pointer repaint lands on the same
 * cell as a full redraw would. */
static long cursor_physical_row(size_t total) { size_t logical,physical=0,visual=0;unsigned int cells[MAX_COLS];unsigned char inverse[MAX_COLS],connector[MAX_COLS];
 if(!total)return 0;
 if(cursor_line>=line_count)return (long)total-1;
 for(logical=0;logical<cursor_line;++logical)physical+=line_rows(logical);
 (void)build_cells(cursor_line,cells,inverse,connector,&visual);
 physical+=visual/terminal_cols;
 if(physical>=total)physical=total-1;
 return (long)physical;
}
/* Put the terminal cursor back on the insertion cursor's cell: row, then the
 * RTL column.  Shared by the full redraw and the two-row pointer repaint. */
static int place_cursor(long from_row,long to_row) { unsigned int cells[MAX_COLS];unsigned char inverse[MAX_COLS],connector[MAX_COLS];size_t visual=0,local,target;char seq[32];
 (void)from_row; /* task 60: the move is computed from the true console row */
 ptr_trace("place_cursor from=%ld to=%ld origin=%ld console_row=%ld",from_row,to_row,screen_origin,console_row);
 if(move_rows(screen_origin+(long)to_row-console_row))return-1;
 if(cursor_line>=line_count){if(bytes("\r"))return-1;return 0;}
 (void)build_cells(cursor_line,cells,inverse,connector,&visual);
 local=visual%terminal_cols;if(visual&&local==0)local=terminal_cols;
 if(bytes("\r"))return-1;
 target=terminal_cols-local-1;
 if(target){snprintf(seq,sizeof(seq),"\033[%zuC",target);if(bytes(seq))return-1;}
 return 0;
}
/* Task 52: the painter takes the pointer's cell as a parameter - the single
 * authoritative cell, never a second copy - so a paint can only ever draw
 * the cell the model computed. */
/* Task 60: the arrow overlay is keyed to console_row - the row this draw
 * ACTUALLY lands at (the cursor has not moved since the caller moved it).
 * Keying it to a computed row breaks the moment a scroll adjusts the frame
 * (measured: a one-row trail after the frame scrolls). */
static int draw_row(size_t logical,size_t chunk,unsigned int *cells,unsigned char *inverse,unsigned char *connector,size_t count,struct Cell ptrcell) { size_t from=chunk*terminal_cols,to=from+terminal_cols<count?from+terminal_cols:count,i,n=to-from,pad=terminal_cols-n;
 if(bytes("\r\033[K"))return-1;for(i=0;i<pad;++i){int p=pointer_marker_visible()&&ptrcell.y==console_row&&ptrcell.x==(long)i;if(p){painted_cell=ptrcell;painted_cell.y=console_row;}if(p&&bytes("\033[7m"))return-1;if(putcp(p&&pointer_arrow_glyph?pointer_arrow_glyph:0x20))return-1;if(p&&bytes("\033[27m"))return-1;}for(i=from;i<to;++i){int p=pointer_marker_visible()&&ptrcell.y==console_row&&ptrcell.x==(long)(pad+i-from);unsigned int shown=p&&pointer_arrow_glyph?pointer_arrow_glyph:draw_codepoint(cells[i],connector[i]);if(p){painted_cell=ptrcell;painted_cell.y=console_row;}if((inverse[i]||p)&&bytes("\033[7m"))return-1;if(putcp(shown))return-1;if((inverse[i]||p)&&bytes("\033[27m"))return-1;}return bytes("\033[0m"); }
/* Work order 66, owner ruling (Sep 27 2026): the mode indicator is DROPPED
 * entirely ("drop it looks ugly").  This function is kept as a no-op so the
 * two call sites (full redraw, pointer repaint) and the cursor geometry
 * around them stay untouched; it no longer paints a corner glyph.  The
 * remaining cue for which script the keys produce is the letterforms
 * themselves — Latin is only reachable by holding AltGr / Ctrl+Space. */
static int paint_indicator(void) {
    return 0;
}
/* Map a screen row back to the logical line and chunk that own it; rows past
 * the text have no owner and are drawn as empty rows, so the pointer stays
 * visible (and erasable) anywhere on the screen. */
static int row_for_physical(long physical,size_t *logical,size_t *chunk) { size_t r,start=0;
 for(r=0;r<line_count;++r){size_t rows=line_rows(r);if(physical<(long)(start+rows)){*logical=r;*chunk=(size_t)(physical-(long)start);return 1;}start+=rows;}
 return 0;
}
/* Task 60: draw_physical_row takes a CONSOLE row; the typing frame lives at
 * console row screen_origin, so the frame's own row is crow - screen_origin
 * (negative: a row above the frame - the profile's banner). */
static int draw_physical_row(long crow,struct Cell ptrcell) { unsigned int cells[MAX_COLS];unsigned char inverse[MAX_COLS],connector[MAX_COLS];size_t logical,chunk,count,unused=0;long frow=crow-screen_origin;
 if(frow>=0&&row_for_physical((size_t)frow,&logical,&chunk)){count=build_cells(logical,cells,inverse,connector,&unused);return draw_row(logical,chunk,cells,inverse,connector,count,ptrcell);}
 /* Task 60: a row above the typing frame is the profile's banner; redraw it
  * from the startup snapshot instead of as an empty row.  The snapshot row
  * is padded to the full width so draw_row's RTL anchoring pad (meant for
  * typing rows) is zero and the hint lands on its original columns. */
 if(crow<screen_origin&&(size_t)crow<banner_count){unsigned int bcell[MAX_COLS];unsigned char inv[MAX_COLS],con[MAX_COLS];size_t i,len=banner_len[crow];
  if(len>terminal_cols)len=terminal_cols;
  for(i=0;i<len;++i)bcell[i]=banner_cells[crow][i];
  for(;i<terminal_cols;++i)bcell[i]=0x20;
  memset(inv,0,sizeof(inv));memset(con,0,sizeof(con));
  return draw_row(0,0,bcell,inv,con,terminal_cols,ptrcell);}
 return draw_row(0,0,NULL,NULL,NULL,0,ptrcell);
}
/* The pointer repaint is deliberately local: redraw only the row it left
 * (restoring whatever cell was there) and the row it entered, then put the
 * insertion cursor back.  A whole-screen repaint per mouse event is what
 * flickered and left stale markers on the console.  The whole erase+draw
 * sequence is staged in the output buffer and flushed with a single write,
 * so the two rows can never interleave with a text redraw. */
static int repaint_pointer(struct Cell oldc,struct Cell newc) { long a=oldc.y,b=newc.y,cp;
 if(a<0)a=b; /* nothing painted yet: only the row it entered */
 if(a>b){long t=a;a=b;b=t;}
 ptr_trace("repaint_pointer old=(%ld,%ld) new=(%ld,%ld) rows a=%ld b=%ld origin=%ld console_row=%ld",oldc.x,oldc.y,newc.x,newc.y,a,b,screen_origin,console_row);
 if(move_rows(a-console_row))return-1; /* the model cell's y is a console row */
 if(draw_physical_row(a,newc))return-1;
 if(a==screen_origin){if(paint_indicator())return-1;}
 if(b!=a){if(move_rows(b-a))return-1;if(draw_physical_row(b,newc))return-1;}
 cp=cursor_physical_row(physical_rows());
 if(place_cursor(b,cp))return-1;
 ptr_trace("repaint_pointer flush console_row=%ld",console_row);
 if(flush_out())return -1;
 /* Task 64: painted_cell was recorded by draw_row at the moment the arrow
  * was painted (keyed to console_row); nothing to assign here. */
 return 0;
}
/* Relative CSI row movement lets the buffer be redrawn without an alternate screen. */
static int redraw(size_t first) { unsigned int cells[MAX_COLS];unsigned char inverse[MAX_COLS],connector[MAX_COLS];size_t total=physical_rows(),old=displayed_rows,last=(old>total?old:total)-1,logical,chunk,physical=0,cursor_visual=0,count=0;long cursor_physical;
 (void)first;ptr_trace("redraw entry origin=%ld console_row=%ld total=%zu last=%ld",screen_origin,console_row,total,last);if(move_rows(screen_origin-console_row))return-1;cursor_physical=cursor_physical_row(total);
 for(logical=0;logical<line_count;++logical){count=build_cells(logical,cells,inverse,connector,&cursor_visual);for(chunk=0;chunk<line_rows(logical);++chunk,++physical){if(draw_row(logical,chunk,cells,inverse,connector,count,pointer_cell))return-1;if(physical==0){if(paint_indicator())return-1;}if(physical!=last){if(bytes("\r\n"))return-1;if(console_row==(long)terminal_rows-1)console_scrolled();else console_row++;}}}
 /* Task 60: the arrow may sit ABOVE the typing frame (on the banner); the
  * frame's own rows do not cover it, and a screen scroll would otherwise
  * strand it off-screen.  Repaint its row, then return to the tail start.
  * Task 64: the row it LEFT may also be a banner row (a hover move from the
  * banner into the frame); repaint that row too, or it trails. */
 if(pointer_marker_visible()){
  long br=pointer_cell.y>=0&&pointer_cell.y<screen_origin?pointer_cell.y:-1;
  long bo=painted_cell.y>=0&&painted_cell.y<screen_origin?painted_cell.y:-1;
  if(br>=0){if(move_rows(br-console_row))return-1;if(draw_physical_row(br,pointer_cell))return-1;}
  if(bo>=0&&bo!=br){if(move_rows(bo-console_row))return-1;if(draw_physical_row(bo,pointer_cell))return-1;}
  if(br>=0||bo>=0){if(move_rows(screen_origin+(long)physical-console_row))return-1;}
 }
 /* Task 60: and the arrow may sit BELOW the frame's last row; extend the
  * empty-row tail so the redraw covers it too.  When the main loop ended on
  * the frame's last row no \r\n followed, so align before the tail starts. */
 {long end=last;if(pointer_marker_visible()&&pointer_cell.y>=0&&pointer_cell.y-screen_origin>end)end=pointer_cell.y-screen_origin;
 /* Task 64: a full redraw must cover the row the arrow LEFT too (it may sit
  * BELOW the new cell - a drag upward), or that arrow survives as a trail. */
 if(pointer_marker_visible()&&painted_cell.y>=0&&painted_cell.y-screen_origin>end)end=painted_cell.y-screen_origin;
 if(physical<=end&&console_row==screen_origin+(long)physical-1){if(bytes("\r\n"))return-1;if(console_row==(long)terminal_rows-1)console_scrolled();else console_row++;}
 while(physical<=end){if(draw_row(0,0,NULL,NULL,NULL,0,pointer_cell))return-1;if(physical!=end){if(bytes("\r\n"))return-1;if(console_row==(long)terminal_rows-1)console_scrolled();else console_row++;}++physical;}}
 displayed_rows=total;if(place_cursor((long)last,cursor_physical))return-1;
 ptr_trace("redraw exit origin=%ld console_row=%ld",screen_origin,console_row);
 if(flush_out())return -1;
 /* Task 64: painted_cell is set by draw_row at the moment the arrow is
  * actually painted (keyed to console_row) and decremented by every scroll
  * (console_scrolled), so it always names the row the arrow is ON.  A final
  * assignment here would re-store the pre-scroll row and bring the trail
  * back (measured: the task-60 edit's assignment did exactly that). */
 return 0; }
static int decode_byte(unsigned char b,unsigned int *cp,unsigned int *acc,int *need){if(!*need){if(b<0x80){*cp=b;return 1;}if((b&0xe0)==0xc0)*acc=b&31,*need=1;else if((b&0xf0)==0xe0)*acc=b&15,*need=2;else if((b&0xf8)==0xf0)*acc=b&7,*need=3;else return-1;return 0;}if((b&0xc0)!=0x80){*need=0;return-1;}*acc=(*acc<<6)|(b&63);if(!--*need){*cp=*acc;return 1;}return 0;}
static int any_pointer_button_down(void) { size_t i;for(i=0;i<pointer_device_count;++i)if(pointer_devices[i].left_down)return 1;return 0; }
static int pointer_event(struct PointerDevice *d,const struct input_event *ev) { struct Cell c=pointer_cell;
 if(d->kind!=POINTER_MICE)pointer_evdev_delivered=1;
 if(ev->type==EV_ABS&&(ev->code==ABS_X||ev->code==ABS_Y)){if(ev->code==ABS_X)d->abs_x=ev->value;else d->abs_y=ev->value;c=pointer_map(d,d->abs_x,d->abs_y);apply_pointer_drag(DRAG_MOVE,c);pointer_debug_log(d,ev,pointer_cell);++test_event_count;return 1;}
 if(ev->type==EV_REL&&(ev->code==REL_X||ev->code==REL_Y)){if(d->kind==POINTER_MICE&&pointer_evdev_delivered){pointer_debug_log(d,ev,pointer_cell);return 0;}c=pointer_map_delta(d,ev->code==REL_X?ev->value:0,ev->code==REL_Y?ev->value:0);apply_pointer_drag(DRAG_MOVE,c);pointer_debug_log(d,ev,pointer_cell);++test_event_count;return 1;}
 if(ev->type==EV_KEY&&ev->code==BTN_LEFT){if(ev->value&&!d->left_down){pointer_structural=1;d->left_down=1;if(!pointer_down)apply_pointer_drag(DRAG_LEFT_DOWN,pointer_cell);}else if(!ev->value&&d->left_down){pointer_structural=1;d->left_down=0;if(pointer_down&&!any_pointer_button_down())apply_pointer_drag(DRAG_LEFT_UP,pointer_cell);}else{pointer_debug_log(d,ev,pointer_cell);return 0;}pointer_debug_log(d,ev,pointer_cell);return 1;}
 if(ev->type==EV_KEY&&ev->code==BTN_MIDDLE&&ev->value){pointer_structural=1;apply_pointer_drag(DRAG_MIDDLE,pointer_cell);pointer_debug_log(d,ev,pointer_cell);return 1;}return 0;
}
static int dead_pointer_errno(int e) { return e==ENODEV||e==EIO||e==EBADF||e==ENXIO; }
static int pointer_events(struct PointerDevice *d) { int changed=0;struct input_event ev;
 if(d->fd<0)return 0;
 if(d->kind==POINTER_MICE){unsigned char buf[64];ssize_t nr;while((nr=read(d->fd,buf,sizeof(buf)))>0){size_t i;for(i=0;i<(size_t)nr;++i){d->packet[d->packet_have++]=buf[i];if(d->packet_have==3){struct input_event move={.type=EV_REL,.code=REL_X,.value=(signed char)d->packet[1]},ymove={.type=EV_REL,.code=REL_Y,.value=-(signed char)d->packet[2]},button={.type=EV_KEY,.code=BTN_LEFT,.value=(d->packet[0]&1)!=0};changed|=pointer_event(d,&move);changed|=pointer_event(d,&ymove);changed|=pointer_event(d,&button);d->packet_have=0;}}}if((nr==0&&pointer_device_is_real(d))||(nr<0&&dead_pointer_errno(errno)))drop_pointer_device(d);return changed;}
 for(;;){ssize_t nr=read(d->fd,&ev,sizeof(ev));if(nr==(ssize_t)sizeof(ev)){changed|=pointer_event(d,&ev);continue;}if((nr==0&&pointer_device_is_real(d))||(nr<0&&dead_pointer_errno(errno)))drop_pointer_device(d);break;}return changed; }
/* One pointer cycle: drain EVERY device completely, keep only the LAST cell,
 * then paint at most once.  Nothing is ever painted from the read path, and
 * nothing at all is painted while the coalesced cell equals the cell already
 * on screen, so a burst of events inside one cell emits byte-for-byte
 * nothing.  Structural events (press, release, middle click) and drag moves
 * repaint the whole screen from the model; hover moves repaint only the row
 * the arrow left and the row it entered, in one ordered burst. */
static int pointer_coalesce_paint(void) { int any=0,structural=0,was_down=pointer_down;size_t i;struct Cell before=painted_cell;
 for(i=0;i<pointer_device_count;++i){pointer_structural=0;if(pointer_events(&pointer_devices[i])){any=1;if(pointer_structural)structural=1;}}
 if(!any)return 0;
 ptr_trace("coalesce painted_cell=(%ld,%ld) pointer_cell=(%ld,%ld) structural=%d",before.x,before.y,pointer_cell.x,pointer_cell.y,structural);
 if(pointer_visible&&!structural&&before.x==pointer_cell.x&&before.y==pointer_cell.y)return 0;
 if(was_down||pointer_down||structural){if(redraw(0))return -1;}
 else if(repaint_pointer(before,pointer_cell))return -1;
 /* Task 52: painted_cell is maintained by the paint functions themselves
  * (repaint_pointer and redraw), so every path that paints also records what
  * it painted and the paint-on-change rule can never drift from the screen. */
 if(test_paint_marker)emit_paint_marker();
 return 0;
}
/* ---- Task 60: the arrow exists from the first frame ---- */
static long now_ms(void) { struct timeval tv; gettimeofday(&tv,NULL); return (long)tv.tv_sec*1000+tv.tv_usec/1000; }
/* The cursor-position query only makes sense on the real console: a pty's
 * cursor starts at row 0 (the origin is trivially 0 there), and the suite
 * and the fuzzer run the editor on ptys where the stream must stay exactly
 * as before. */
static int is_vt_console(void) { int kb=0; return ioctl(tty_fd,KDGKBTYPE,&kb)>=0; }
/* Task 60 diagnostic: read_banner findings appended to the pointer debug
 * log (which the guest profile already enables), so a silent snapshot
 * failure can be told from a successful one without touching the screen. */
static void banner_debug(const char *fmt, ...) { FILE *f;va_list ap;
 if(!pointer_debug)return;
 f=fopen("/tmp/ar-type-pointer.log","a");if(!f)return;
 fprintf(f,"banner: ");va_start(ap,fmt);vfprintf(f,fmt,ap);va_end(ap);fputc('\n',f);fclose(f);
}
/* The /dev/vcsu layout is kernel-dependent.  Kernel 7.x (measured on the
 * guest: an 8000-byte read for an 80x25 console, every cell U+0020 for the
 * blank rows) serves HEADERLESS UTF-32LE cells - 80x25x4 = 8000 exactly.
 * Older kernels serve an 8-byte rows/cols header (u16 or u32 fields) plus
 * UTF-16LE cells.  The read size against the editor's own geometry (from
 * TIOCGWINSZ, the same console) picks the layout; anything unrecognised is
 * refused rather than mis-parsed. */
static size_t vcsu_parse(const unsigned char *h,size_t *rows,size_t *cols) { unsigned long r32,c32,r16,c16;
 r16=(unsigned long)h[0]|((unsigned long)h[1]<<8);c16=(unsigned long)h[2]|((unsigned long)h[3]<<8);
 r32=(unsigned long)h[0]|((unsigned long)h[1]<<8)|((unsigned long)h[2]<<16)|((unsigned long)h[3]<<24);
 c32=(unsigned long)h[4]|((unsigned long)h[5]<<8)|((unsigned long)h[6]<<16)|((unsigned long)h[7]<<24);
 if(r32>=1&&r32<=128&&c32>=8&&c32<=MAX_COLS){*rows=(size_t)r32;*cols=(size_t)c32;return 8;}
 if(r16>=1&&r16<=128&&c16>=8&&c16<=MAX_COLS){*rows=(size_t)r16;*cols=(size_t)c16;return 4;}
 return 0;
}
/* Snapshot the console rows above the typing frame (the profile's banner)
 * from /dev/vcsu (the foreground VT's cell contents), once the CPR reply
 * has located the frame.  A read failure, a missing device, or an
 * unrecognised layout leaves banner_count at 0: those rows then redraw as
 * empty rows, exactly as before task 60. */
static void read_banner(void) { int fd;unsigned char buf[16+4*MAX_COLS*64];ssize_t got;size_t r,c,n,cols,hs,cellbytes;char hex[32*3+1];size_t h;
 banner_count=0;if(screen_origin<=0)return;
 fd=open("/dev/vcsu",O_RDONLY);if(fd<0){banner_debug("open /dev/vcsu failed errno=%d",errno);return;}
 got=read(fd,buf,sizeof(buf));close(fd);
 banner_debug("read got=%ld",(long)got);
 if(got<=0)return;
 /* headerless UTF-32LE: the size must equal rows x cols x 4 exactly */
 if(got==(ssize_t)((size_t)terminal_rows*(size_t)terminal_cols*4)){hs=0;cellbytes=4;n=(size_t)terminal_rows;cols=(size_t)terminal_cols;
  banner_debug("layout: headerless UTF-32 (%zux%zu)",n,cols);
 }else{unsigned long r32,c32;
  hs=vcsu_parse(buf,&n,&cols);
  hex[0]=0;for(h=0;h<32&&h<(size_t)got;++h){char t[4];snprintf(t,sizeof(t),"%02x ",buf[h]);strcat(hex,t);}banner_debug("bytes 0-31: %s",hex);
  r32=(unsigned long)buf[0]|((unsigned long)buf[1]<<8)|((unsigned long)buf[2]<<16)|((unsigned long)buf[3]<<24);
  c32=(unsigned long)buf[4]|((unsigned long)buf[5]<<8)|((unsigned long)buf[6]<<16)|((unsigned long)buf[7]<<24);
  banner_debug("header parse rows=%zu cols=%zu hs=%zu (u32 r=%lu c=%lu) origin=%ld",n,cols,hs,r32,c32,screen_origin);
  if(!hs)return;
  if(got<hs+(ssize_t)((size_t)n*(size_t)cols*2))return;
  cellbytes=2;
 }
 if(n>(size_t)terminal_rows)n=(size_t)terminal_rows;
 if(n>(size_t)screen_origin)n=(size_t)screen_origin;
 if(cols>MAX_COLS)cols=MAX_COLS;
 for(r=0;r<n;++r){size_t len=0;
  for(c=0;c<cols;++c){unsigned long off=hs+(r*cols+c)*cellbytes;unsigned int cp=cellbytes==4?(unsigned int)(buf[off]|((unsigned long)buf[off+1]<<8)|((unsigned long)buf[off+2]<<16)|((unsigned long)buf[off+3]<<24)):(unsigned int)(buf[off]|(buf[off+1]<<8));if(cp&&cp!=0x20)len=c+1;}
  banner_len[r]=len;
  for(c=0;c<len;++c){unsigned long off=hs+(r*cols+c)*cellbytes;banner_cells[r][c]=cellbytes==4?(unsigned int)(buf[off]|((unsigned long)buf[off+1]<<8)|((unsigned long)buf[off+2]<<16)|((unsigned long)buf[off+3]<<24)):(unsigned int)(buf[off]|(buf[off+1]<<8));}
  banner_count=r+1;
 }
 banner_debug("snapshot rows=%zu len0=%zu len1=%zu first U+%04X U+%04X",banner_count,banner_len[0],banner_len[1],banner_len[1]?banner_cells[1][0]:0,banner_len[1]>1?banner_cells[1][1]:0);
}
/* Task 60: the first motion after a viewer connects is measured to be LOST
 * on the host side - 3/3 first commanded motions and 1/3 second motions
 * delivered no events to ANY pointer device, so the editor had nothing to
 * log or paint and the screen showed no arrow at all until the first
 * delivered event.  Paint the arrow on the first frame at the tablet's
 * REGISTERED position (the last cell the guest kernel saw), so the screen
 * always carries exactly one arrow at the model's last-known cell; the
 * first delivered event re-anchors it to the hand (task 46). */
static void paint_startup_arrow(void) { size_t i;struct Cell c;
 if(startup_arrow_done)return;startup_arrow_done=1;
 for(i=0;i<pointer_device_count;++i)if(pointer_devices[i].kind==POINTER_ABS)break;
 if(i==pointer_device_count)return; /* no absolute device: nothing is known */
 c=pointer_map(&pointer_devices[i],pointer_devices[i].abs_x,pointer_devices[i].abs_y);
 ptr_trace("paint_startup_arrow dev=%s abs=(%ld,%ld) cell=(%ld,%ld) origin=%ld console_row=%ld",pointer_devices[i].node,pointer_devices[i].abs_x,pointer_devices[i].abs_y,c.x,c.y,screen_origin,console_row);
 pointer_cell=c;pointer_visible=1;
 (void)repaint_pointer((struct Cell){-1,-1},c);
}
static int mouse_test(void) { struct PointerMap m;struct DragState s={0};struct Cell c;size_t r,col,el,ec;int ok=1,i;static const struct {int absolute;long x,y,minx,maxx,miny,maxy,fw,fh,cols,rows,px,py,ex,ey;} maps[]={
 {1,0,0,0,1279,0,799,1280,800,80,25,0,0,0,0},{1,1279,799,0,1279,0,799,1280,800,80,25,0,0,79,24},{1,0,799,0,1279,0,799,1280,800,160,50,0,0,0,49},{1,1279,0,0,1279,0,799,1280,800,160,50,0,0,159,0},{1,110,220,100,1100,200,1200,1280,800,80,25,0,0,0,0},
 {1,640,400,0,1279,0,799,1280,800,80,25,0,0,40,12},{1,640,400,0,1279,0,799,1280,800,160,50,0,0,80,25},{1,600,700,100,1100,200,1200,1280,800,80,25,0,0,39,12},
 {0,32,-16,0,0,0,0,1280,800,80,25,10,10,12,10}};
 for(i=0;i<(int)(sizeof(maps)/sizeof(maps[0]));++i){m.absolute=maps[i].absolute;m.raw_x=maps[i].x;m.raw_y=maps[i].y;m.min_x=maps[i].minx;m.max_x=maps[i].maxx;m.min_y=maps[i].miny;m.max_y=maps[i].maxy;m.fb_w=maps[i].fw;m.fb_h=maps[i].fh;m.cols=maps[i].cols;m.rows=maps[i].rows;m.previous=(struct Cell){maps[i].px,maps[i].py};c=map_pointer(&m);printf("MAP %d abs=%d raw=(%ld,%ld) range=(%ld..%ld,%ld..%ld) fb=%ldx%ld geo=%ldx%ld -> cell=(%ld,%ld) expect=(%ld,%ld)\n",i+1,m.absolute,m.raw_x,m.raw_y,m.min_x,m.max_x,m.min_y,m.max_y,m.fb_w,m.fb_h,m.cols,m.rows,c.x,c.y,maps[i].ex,maps[i].ey);ok&=c.x==maps[i].ex&&c.y==maps[i].ey;}
 printf("MAP table %s\n",ok?"PASS":"FAIL");s=drag_step(s,DRAG_LEFT_DOWN,(struct Cell){7,0});s=drag_step(s,DRAG_MOVE,(struct Cell){5,1});s=drag_step(s,DRAG_LEFT_UP,(struct Cell){5,1});printf("DRAG pure anchor=(%ld,%ld) current=(%ld,%ld) kept=%d\n",s.anchor.x,s.anchor.y,s.current.x,s.current.y,!s.down&&s.moved);ok&=!s.down&&s.moved;
 /* Task 77 (finding 3): ASCII lines now render left-to-right, so the same
  * "drag across the whole line" act starts at the left cell (5, the first
  * logical character) and ends at the right cell (7); the asserted values
  * are unchanged and were measured identical on the pre-77 renderer. */
 memset(lines,0,sizeof(lines));line_count=2;lines[0].cp[0]='a';lines[0].cp[1]='b';lines[0].cp[2]='c';lines[0].len=3;lines[1].cp[0]='d';lines[1].cp[1]='e';lines[1].cp[2]='f';lines[1].len=3;cursor_line=cursor_col=anchor_line=anchor_col=0;terminal_cols=8;terminal_rows=2;pointer_visible=0;pointer_down=0;drag=(struct DragState){0};apply_pointer_drag(DRAG_LEFT_DOWN,(struct Cell){5,0});apply_pointer_drag(DRAG_MOVE,(struct Cell){7,1});apply_pointer_drag(DRAG_LEFT_UP,(struct Cell){7,1});selection_bounds(&r,&col,&el,&ec);printf("DRAG selection rows=%zu..%zu cols=(%zu..3,0..%zu) cursor=(%zu,%zu) inverse row0=[0,1,2] row1=[0,1,2]\n",r,el,col,ec,cursor_line,cursor_col);ok&=r==0&&el==1&&col==0&&ec==3&&selected(0,0)&&selected(0,1)&&selected(0,2)&&selected(1,0)&&selected(1,1)&&selected(1,2);
 if(delete_selection()&&insert_cp('Z'))printf("TYPE line0=%c lines=%zu\n",(char)lines[0].cp[0],line_count);else ok=0;ok&=line_count==1&&lines[0].len==1&&lines[0].cp[0]=='Z';printf("DRAG/type %s\n",ok?"PASS":"FAIL");
 memset(lines,0,sizeof(lines));line_count=1;lines[0].cp[0]='X';lines[0].cp[1]='Y';lines[0].cp[2]='Z';lines[0].len=3;cursor_line=0;cursor_col=1;anchor_line=0;anchor_col=0;copy_selection();delete_selection();apply_pointer_drag(DRAG_MIDDLE,(struct Cell){0,0});printf("MIDDLE paste len=%zu line0=%c%c%c clipboard=%c\n",lines[0].len,(char)lines[0].cp[0],(char)lines[0].cp[1],(char)lines[0].cp[2],(char)clipboard[0]);ok&=lines[0].len==3&&lines[0].cp[0]=='X'&&lines[0].cp[1]=='Y'&&lines[0].cp[2]=='Z';printf("MIDDLE %s\n",ok?"PASS":"FAIL");
 anchor_line=0;anchor_col=0;cursor_line=1;cursor_col=2;apply_pointer_drag(DRAG_LEFT_DOWN,(struct Cell){6,0});apply_pointer_drag(DRAG_LEFT_UP,(struct Cell){6,0});pointer_logical((struct Cell){6,0},&r,&col);printf("CLICK selection=%d cursor=(%zu,%zu)\n",has_selection(),cursor_line,cursor_col);ok&=!has_selection();
 /* Task 54: pointer hit-testing across a shadda+vowel combo, through the
  * real pointer_logical/visual_source path.  A clicked cell must map back
  * to the first logical column of the pair the cell draws.  The buffers are
  * typed in shadda-then-vowel order, what the console produces. */
 {unsigned int cells[MAX_COLS];unsigned char inv[MAX_COLS],conn[MAX_COLS];size_t vis=0,count,k;int ok54=1;
  pointer_visible=0;pointer_down=0;drag=(struct DragState){0};terminal_cols=8;terminal_rows=1;
  /* مَّر mid-word: reh | combo(connector U+E038) | meem, 3 cells on 8 cols. */
  memset(lines,0,sizeof(lines));line_count=1;lines[0].cp[0]=0x0645;lines[0].cp[1]=0x0651;lines[0].cp[2]=0x064E;lines[0].cp[3]=0x0631;lines[0].len=4;cursor_line=0;cursor_col=4;
  count=build_cells(0,cells,inv,conn,&vis);
  printf("TASK54 combo مَّر cells:");for(k=0;k<count;++k)printf(" U+%04X",cells[k]);printf("\n");
  ok54&=count==3&&cells[0]==0xFEAE&&cells[1]==0xE038&&cells[2]==0xFEE3;
  for(k=0;k<count;++k){pointer_logical((struct Cell){7-k,0},&r,&col);printf("TASK54 click col %ld -> logical col %zu (cell U+%04X)\n",7-(long)k,col,cells[k]);}
  pointer_logical((struct Cell){5,0},&r,&col);ok54&=col==3;   /* reh cell */
  pointer_logical((struct Cell){6,0},&r,&col);ok54&=col==1;   /* combo cell -> shadda column */
  pointer_logical((struct Cell){7,0},&r,&col);ok54&=col==0;   /* meem cell */
  /* مَّ alone: combo(plain U+E030) | meem, 2 cells. */
  memset(lines,0,sizeof(lines));line_count=1;lines[0].cp[0]=0x0645;lines[0].cp[1]=0x0651;lines[0].cp[2]=0x064E;lines[0].len=3;cursor_line=0;cursor_col=3;
  count=build_cells(0,cells,inv,conn,&vis);
  printf("TASK54 combo مَّ cells:");for(k=0;k<count;++k)printf(" U+%04X",cells[k]);printf("\n");
  ok54&=count==2&&cells[0]==0xE030&&cells[1]==0xFEE1;
  pointer_logical((struct Cell){6,0},&r,&col);ok54&=col==1;   /* combo cell -> shadda column */
  pointer_logical((struct Cell){7,0},&r,&col);ok54&=col==0;   /* meem cell */
  ok&=ok54;printf("TASK54 combo hit-test %s\n",ok54?"PASS":"FAIL");
 }
 /* Task 77 (finding 3): mixed Latin/Arabic lines.  An ASCII island keeps its
  * own left-to-right order inside the word, the Arabic word stays word-
  * reversed, the pointer round-trip maps every cell back to its logical
  * column, and the layout survives insertion and deletion.  The Arabic word
  * is rendered alone first and used as the reference for its cells inside
  * the mixed line: shaping is word-bounded, so it must not move. */
 {unsigned int cells[MAX_COLS],word[3];unsigned char inv[MAX_COLS],conn[MAX_COLS];size_t vis=0,count,k,wordcount;int ok77=1;
  static const unsigned int root[6]={0x002F,0x0072,0x006F,0x006F,0x0074,0x002F};
  memset(lines,0,sizeof(lines));line_count=1;lines[0].cp[0]=0x0639;lines[0].cp[1]=0x0631;lines[0].cp[2]=0x0636;lines[0].len=3;cursor_line=0;cursor_col=3;
  count=build_cells(0,cells,inv,conn,&vis);wordcount=count;for(k=0;k<count&&k<3;++k)word[k]=cells[k];
  /* Logical: عرض /root/ - cells must be [/ r o o t /] space [word reversed]. */
  memset(lines,0,sizeof(lines));line_count=1;
  lines[0].cp[0]=0x0639;lines[0].cp[1]=0x0631;lines[0].cp[2]=0x0636;lines[0].cp[3]=0x0020;
  lines[0].cp[4]=0x002F;lines[0].cp[5]=0x0072;lines[0].cp[6]=0x006F;lines[0].cp[7]=0x006F;lines[0].cp[8]=0x0074;lines[0].cp[9]=0x002F;lines[0].len=10;cursor_line=0;cursor_col=10;
  count=build_cells(0,cells,inv,conn,&vis);
  printf("TASK77 mixed cells:");for(k=0;k<count;++k)printf(" U+%04X",cells[k]);printf("\n");
  ok77&=count==10&&wordcount==3;
  for(k=0;k<6;++k)ok77&=cells[k]==root[k];
  ok77&=cells[6]==0x0020;
  for(k=0;k<3;++k)ok77&=cells[7+k]==word[k];
  ok77&=visual_source(0,0)==4&&visual_source(0,5)==9&&visual_source(0,6)==3&&visual_source(0,7)==2&&visual_source(0,9)==0;
  cursor_col=7;(void)build_cells(0,cells,inv,conn,&vis);
  printf("TASK77 caret at logical 7 -> visual=%zu (expect 6)\n",vis);
  ok77&=vis==6;
  cursor_col=4;ok77&=insert_cp('x');
  count=build_cells(0,cells,inv,conn,&vis);
  printf("TASK77 after insert cells:");for(k=0;k<count;++k)printf(" U+%04X",cells[k]);printf("\n");
  ok77&=count==11&&cells[0]==0x0078&&cells[1]==0x002F&&cells[6]==0x002F;
  cursor_col=5;ok77&=backspace_cp();
  count=build_cells(0,cells,inv,conn,&vis);
  ok77&=count==10;for(k=0;k<6;++k)ok77&=cells[k]==root[k];
  ok77&=cells[6]==0x0020;for(k=0;k<3;++k)ok77&=cells[7+k]==word[k];
  cursor_col=10;(void)build_cells(0,cells,inv,conn,&vis);ok77&=vis==10;
  ok&=ok77;printf("TASK77 mixed-line Latin islands %s\n",ok77?"PASS":"FAIL");
 }
 /* Two absolute devices deliberately have unlike origins/ranges.  Feed their
  * events in turn through the same path used by poll(), rather than testing a
  * synthetic selection shortcut. */
 memset(lines,0,sizeof(lines));line_count=2;lines[0].cp[0]='a';lines[0].cp[1]='b';lines[0].cp[2]='c';lines[0].len=3;lines[1].cp[0]='d';lines[1].cp[1]='e';lines[1].cp[2]='f';lines[1].len=3;terminal_cols=8;terminal_rows=2;pointer_cell=(struct Cell){0,0};pointer_visible=0;pointer_down=0;drag=(struct DragState){0};memset(pointer_devices,0,sizeof(pointer_devices));pointer_device_count=2;
 pointer_devices[0]=(struct PointerDevice){.kind=POINTER_ABS,.min_x=0,.max_x=32767,.min_y=0,.max_y=32767};
 pointer_devices[1]=(struct PointerDevice){.kind=POINTER_ABS,.min_x=100,.max_x=1100,.min_y=200,.max_y=1200};
 {struct input_event ex={.type=EV_ABS,.code=ABS_X,.value=0},ey={.type=EV_ABS,.code=ABS_Y,.value=0},down={.type=EV_KEY,.code=BTN_LEFT,.value=1},bx={.type=EV_ABS,.code=ABS_X,.value=1100},by={.type=EV_ABS,.code=ABS_Y,.value=1200},up={.type=EV_KEY,.code=BTN_LEFT,.value=0},samea={.type=EV_ABS,.code=ABS_X,.value=1000},sameb={.type=EV_ABS,.code=ABS_X,.value=1000};
  pointer_event(&pointer_devices[0],&ex);pointer_event(&pointer_devices[0],&ey);printf("DEVICE A event -> cell=(%ld,%ld)\n",pointer_cell.x,pointer_cell.y);ok&=pointer_cell.x==0&&pointer_cell.y==0;
  pointer_event(&pointer_devices[0],&down);pointer_event(&pointer_devices[1],&bx);pointer_event(&pointer_devices[1],&by);printf("DEVICE B event -> cell=(%ld,%ld) button=%d selection=%d\n",pointer_cell.x,pointer_cell.y,pointer_down,has_selection());ok&=pointer_cell.x==7&&pointer_cell.y==1&&pointer_down&&has_selection();
  pointer_event(&pointer_devices[0],&up);printf("DEVICE A release after B move -> button=%d\n",pointer_down);ok&=!pointer_down;
  pointer_devices[0].abs_y=1000;pointer_devices[1].abs_y=1000;pointer_event(&pointer_devices[0],&samea);c=pointer_cell;pointer_event(&pointer_devices[1],&sameb);printf("RANGE same raw=(1000,1000): A=(%ld,%ld) B=(%ld,%ld)\n",c.x,c.y,pointer_cell.x,pointer_cell.y);ok&=(c.x!=pointer_cell.x||c.y!=pointer_cell.y);
 }
 /* Task 46: the aggregate is the only poison.  An absolute event SETS the
  * position; a relative event MOVES it from the current cell scaled by the
  * framebuffer/cell size - even a relative event from the very node that
  * just reported absolute, the exact case that froze the owner's arrow; a
  * later absolute event re-anchors and corrects the accumulated deltas.
  * Only the aggregate's motion is dropped, and only once an evdev node has
  * delivered. */
 pointer_devices[0]=(struct PointerDevice){.kind=POINTER_ABS,.min_x=0,.max_x=32767,.min_y=0,.max_y=32767};
 pointer_devices[1]=(struct PointerDevice){.kind=POINTER_REL};
 pointer_devices[2]=(struct PointerDevice){.kind=POINTER_MICE};
 pointer_evdev_delivered=0;pointer_cell=(struct Cell){0,0};pointer_visible=0;pointer_down=0;drag=(struct DragState){0};
 terminal_cols=80;terminal_rows=25;framebuffer_w=1280;framebuffer_h=800; /* one cell = 16x32 px */
 {struct input_event ax={.type=EV_ABS,.code=ABS_X,.value=0},ay={.type=EV_ABS,.code=ABS_Y,.value=0},
  rx={.type=EV_REL,.code=REL_X,.value=32},ry={.type=EV_REL,.code=REL_Y,.value=32},
  rn={.type=EV_REL,.code=REL_X,.value=-32},mx={.type=EV_REL,.code=REL_X,.value=64},
  bx={.type=EV_ABS,.code=ABS_X,.value=32767},by={.type=EV_ABS,.code=ABS_Y,.value=32767};
  pointer_event(&pointer_devices[0],&ax);pointer_event(&pointer_devices[0],&ay); /* (0,0) */
  pointer_event(&pointer_devices[0],&rx); /* rel FROM THE ABS NODE: 32px = 2 cells */
  printf("TASK46 same-node abs-then-rel moves cell=(%ld,%ld)\n",pointer_cell.x,pointer_cell.y);
  ok&=pointer_cell.x==2&&pointer_cell.y==0;
  pointer_event(&pointer_devices[0],&ry); /* 32px = 1 row at 32px/row */
  printf("TASK46 same-node rel-y moves cell=(%ld,%ld)\n",pointer_cell.x,pointer_cell.y);
  ok&=pointer_cell.x==2&&pointer_cell.y==1;
  pointer_event(&pointer_devices[0],&bx);pointer_event(&pointer_devices[0],&by); /* absolute re-anchors */
  printf("TASK46 absolute re-anchors cell=(%ld,%ld)\n",pointer_cell.x,pointer_cell.y);
  ok&=pointer_cell.x==79&&pointer_cell.y==24;
  pointer_event(&pointer_devices[1],&rn); /* the rel node moves too: -2 cells */
  printf("TASK46 rel-node moves cell=(%ld,%ld)\n",pointer_cell.x,pointer_cell.y);
  ok&=pointer_cell.x==77&&pointer_cell.y==24;
  pointer_event(&pointer_devices[2],&mx); /* the aggregate is gated: +64px dropped */
  printf("TASK46 mice-gated-after-evdev cell=(%ld,%ld)\n",pointer_cell.x,pointer_cell.y);
  ok&=pointer_cell.x==77&&pointer_cell.y==24;
  pointer_evdev_delivered=0;pointer_cell=(struct Cell){0,0};pointer_visible=0;pointer_down=0;drag=(struct DragState){0};
  pointer_event(&pointer_devices[1],&rx); /* with no absolute ever, rel-only still drives */
  printf("TASK46 rel-only moves cell=(%ld,%ld)\n",pointer_cell.x,pointer_cell.y);
  ok&=pointer_cell.x==2&&pointer_cell.y==0;
 }
 printf("MULTI DEVICE %s\n",ok?"PASS":"FAIL");printf("MOUSE TEST %s\n",ok?"PASS":"FAIL");return ok?0:1; }
/* Test-only entry: scripted absolute moves through the real event path and
 * the real renderer, so the Python suite can assert the emitted stream (the
 * arrow drawn, the left cell restored, an off-typing-row marker erased).
 * "noarrow" forces the fallback path.  Prints nothing; the PTY carries the
 * escape stream. */
static int pointer_test(int arrow_mode,int moves) { struct winsize ws;struct PointerDevice d;struct input_event ev;struct Cell before;int i;
 static const struct Cell targets[3]={{40,12},{78,0},{2,0}};
 static const long raws[3][2]={{16384,16384},{31949,0},{820,0}};
 if(moves<0)moves=0;if(moves>3)moves=3;
 tty_fd=open("/dev/tty",O_RDWR);if(tty_fd<0)return 1;
 terminal_cols=80;terminal_rows=25;
 if(!ioctl(tty_fd,TIOCGWINSZ,&ws)){if(ws.ws_col)terminal_cols=ws.ws_col;if(ws.ws_row)terminal_rows=ws.ws_row;}
 if(!terminal_cols)terminal_cols=80;if(!terminal_rows)terminal_rows=25;
 read_framebuffer_size();
 pointer_debug=getenv("AR_TYPE_POINTER_DEBUG")!=NULL;
 configure_pointer_drawing();
 pointer_arrow_glyph=arrow_mode?0xE020:0;
 memset(lines,0,sizeof(lines));line_count=2;
 lines[0].cp[0]=0x0645;lines[0].cp[1]=0x0644;lines[0].cp[2]=0x0643;lines[0].len=3;
 lines[1].cp[0]=0x0628;lines[1].cp[1]=0x0633;lines[1].cp[2]=0x0645;lines[1].len=3;
 cursor_line=1;cursor_col=3;anchor_line=cursor_line;anchor_col=cursor_col;
 pointer_visible=0;pointer_down=0;drag=(struct DragState){0};
 memset(pointer_devices,0,sizeof(pointer_devices));pointer_device_count=1;
 d=(struct PointerDevice){0};d.kind=POINTER_ABS;d.min_x=0;d.max_x=32767;d.min_y=0;d.max_y=32767;snprintf(d.node,sizeof(d.node),"/dev/input/test-abs");
 pointer_devices[0]=d;
 pointer_log();
 if(redraw(0))return 1;
 for(i=0;i<moves;++i){before=pointer_cell;
  ev=(struct input_event){.type=EV_ABS,.code=ABS_X,.value=raws[i][0]};pointer_event(&pointer_devices[0],&ev);
  ev=(struct input_event){.type=EV_ABS,.code=ABS_Y,.value=raws[i][1]};pointer_event(&pointer_devices[0],&ev);
  if(!pointer_visible||before.x!=pointer_cell.x||before.y!=pointer_cell.y){if(repaint_pointer(before,pointer_cell))return 1;}
 }
 close(tty_fd);return 0;
}
/* ---- Task 43: scripted pointer bursts through the real poll loop ---- */
static int editor_loop(void);
static const char *test_mode_name; static int test_marker, marker_done;
static void script_abs(int fd,int code,int value){ struct input_event ev;ssize_t w;memset(&ev,0,sizeof(ev));ev.type=EV_ABS;ev.code=code;ev.value=value;w=write(fd,&ev,sizeof(ev));(void)w; }
static void script_rel(int fd,int code,int value){ struct input_event ev;ssize_t w;memset(&ev,0,sizeof(ev));ev.type=EV_REL;ev.code=code;ev.value=value;w=write(fd,&ev,sizeof(ev));(void)w; }
static void script_key(int fd,int code,int value){ struct input_event ev;ssize_t w;memset(&ev,0,sizeof(ev));ev.type=EV_KEY;ev.code=code;ev.value=value;w=write(fd,&ev,sizeof(ev));(void)w; }
/* The legacy aggregate speaks 3-byte PS/2 packets, not input_event. */
static void script_mice(int fd,int x,int y,int buttons){ unsigned char pkt[3];ssize_t w;pkt[0]=(unsigned char)buttons;pkt[1]=(unsigned char)(signed char)x;pkt[2]=(unsigned char)(signed char)y;w=write(fd,pkt,3);(void)w; }
/* The scripted pointer stream is written by a child process into a pipe
 * registered as the pointer device, so the parent's poll loop, drain,
 * coalescing and painting are all exercised for real.  Raw values are chosen
 * so every event maps to its intended cell on the 80x25 test geometry
 * (col=raw*80/32768, row=raw*25/32768 with range 0..32767). */
static void burst_script(const char *mode,int abs_fd,int rel_fd,int mice_fd){
 if(!strcmp(mode,"same0")||!strcmp(mode,"same200")){ /* one move, then a burst inside the same cell */
  script_abs(abs_fd,ABS_X,16384);script_abs(abs_fd,ABS_Y,16384); /* (40,12) */
  if(!strcmp(mode,"same200")){int i;for(i=0;i<200;++i){script_abs(abs_fd,ABS_X,16384+(i&1?150:0));script_abs(abs_fd,ABS_Y,16384+(i&1?0:150));}}
 }else if(!strcmp(mode,"sweep")){ /* ~40 cells, many redundant events per cell */
  int c;script_abs(abs_fd,ABS_X,820);script_abs(abs_fd,ABS_Y,0); /* start (2,0) */
  for(c=0;c<40;++c){long cx=2+(c%10)*8,cy=c/10,rx=cx*410,ry=cy*1311;int k;
   for(k=0;k<12;++k){long jx=rx+(k%4==0?150:0),jy=ry+(k%4==1?400:0);
    script_abs(abs_fd,ABS_X,jx);script_abs(abs_fd,ABS_Y,jy);}
   usleep(2000);}
 }else if(!strcmp(mode,"drag")){ /* press at (79,0), burst-move the path, release at (77,1) */
  int k;script_abs(abs_fd,ABS_X,32359);script_abs(abs_fd,ABS_Y,0);script_key(abs_fd,BTN_LEFT,1);
  for(k=0;k<4;++k){script_abs(abs_fd,ABS_X,32359);script_abs(abs_fd,ABS_Y,0);}
  for(k=0;k<3;++k){script_abs(abs_fd,ABS_X,31949);script_abs(abs_fd,ABS_Y,0);}
  for(k=0;k<3;++k){script_abs(abs_fd,ABS_X,31949);script_abs(abs_fd,ABS_Y,1311);}
  for(k=0;k<3;++k){script_abs(abs_fd,ABS_X,31540);script_abs(abs_fd,ABS_Y,1311);}
  script_key(abs_fd,BTN_LEFT,0);
 }else if(!strcmp(mode,"interleave")){ /* heavy continuous motion while keystrokes arrive */
  int b;for(b=0;b<60;++b){int k;for(k=0;k<30;++k){long cx=((long)b*13+k*7)%80,cy=((long)b*5+k*3)%25;
   script_abs(abs_fd,ABS_X,cx*410);script_abs(abs_fd,ABS_Y,cy*1311);}
   usleep(20000);}
 }else if(!strcmp(mode,"drift0")||!strcmp(mode,"drift")){ /* task 46: an absolute position, then (drift) a flood from the AGGREGATE worth 20+ cells that must be ignored */
  script_abs(abs_fd,ABS_X,16384);script_abs(abs_fd,ABS_Y,16384); /* (40,12) */
  if(!strcmp(mode,"drift")){int k;
   for(k=0;k<6;++k)script_mice(mice_fd,127,0,0); /* 5 cells per packet, 30 cells if applied */
   for(k=0;k<4;++k)script_mice(mice_fd,0,127,0); /* 5 rows per packet, 20 rows if applied */
  }
 }else if(!strcmp(mode,"relonly")){ /* task 46: relative-only tracking - no absolute event ever arrives */
  int k;for(k=0;k<4;++k){script_rel(rel_fd,REL_X,24);script_rel(rel_fd,REL_Y,43);} /* (1,1)..(4,4) */
  script_rel(rel_fd,REL_X,480);script_rel(rel_fd,REL_Y,215); /* (24,9) */
 }else if(!strcmp(mode,"absthenrel")){ /* task 46: absolute then relative from the SAME node, then absolute again */
  script_abs(abs_fd,ABS_X,16384);script_abs(abs_fd,ABS_Y,16384); /* (40,12) */
  script_rel(abs_fd,REL_X,48); /* +2 cells (24px/cell) */
  script_rel(abs_fd,REL_Y,43); /* +1 row (43px/row) */
  script_rel(abs_fd,REL_X,48);
  script_rel(abs_fd,REL_Y,43);
  script_rel(abs_fd,REL_X,-96); /* -4 cells */
  script_rel(abs_fd,REL_Y,-86); /* -2 rows: back to (40,12) */
  script_abs(abs_fd,ABS_X,32767);script_abs(abs_fd,ABS_Y,32767); /* re-anchor (79,24) */
 }else if(!strcmp(mode,"interboth")){ /* task 46: abs jitter inside one cell interleaved with a sub-cell rel wiggle and ignored mice packets */
  script_abs(abs_fd,ABS_X,16384);script_abs(abs_fd,ABS_Y,16384); /* (40,12) */
  {int k;for(k=0;k<100;++k){
   script_abs(abs_fd,ABS_X,16384+(k&1?150:0));script_abs(abs_fd,ABS_Y,16384+(k&1?0:150));
   script_rel(rel_fd,REL_X,k&1?1:-1); /* sub-cell wiggle: 0 cells at 24px/cell */
   script_mice(mice_fd,k&1?127:-127,0,0); /* 5 cells if applied: ignored */
  }}
 }else if(!strcmp(mode,"scrollmove")){ /* task 64: an arrow mid-screen, then the harness types enough Enters to bottom the frame out and scroll it, then one more move.  The scrolls shift the painted arrow up a row each; the fixed editor tracks that (painted_cell follows the scroll), the broken one erases the pre-scroll row and leaves a trail. */
  script_abs(abs_fd,ABS_X,16384);script_abs(abs_fd,ABS_Y,16384); /* (40,12) */
  usleep(2000000); /* the harness types ~30 Enters in this window */
  script_abs(abs_fd,ABS_X,60*410);usleep(30000);
  script_abs(abs_fd,ABS_Y,10*1311);usleep(30000);
 }else if(!strcmp(mode,"sweep8")){ /* task 52: eight paced moves across the grid; every X and every Y event changes the cell, so the parent's coalescing paints exactly once per event */
  long cxy[8][2]={{2,2},{10,5},{22,0},{40,12},{60,18},{70,20},{74,3},{78,24}};
  int m;for(m=0;m<8;++m){
   script_abs(abs_fd,ABS_X,cxy[m][0]*410);usleep(30000);
   script_abs(abs_fd,ABS_Y,cxy[m][1]*1311);usleep(30000);
  }
  usleep(200000);
 }else if(!strcmp(mode,"dragignored")){ /* task 46: press and release on the AGGREGATE, motion on the evdev nodes */
  script_abs(abs_fd,ABS_X,32359);script_abs(abs_fd,ABS_Y,0); /* (79,0) */
  usleep(30000); /* let the parent drain: the pointer now sits at (79,0) */
  script_mice(mice_fd,0,0,1); /* press from the aggregate - an ignored movement source */
  usleep(30000); /* the press lands at (79,0) */
  script_mice(mice_fd,127,0,1); /* aggregate wiggle mid-drag: 5 cells, must not move */
  script_abs(abs_fd,ABS_X,31949);script_abs(abs_fd,ABS_Y,0); /* (78,0) */
  usleep(30000);
  script_abs(abs_fd,ABS_X,31949);script_abs(abs_fd,ABS_Y,1311); /* (78,1) */
  usleep(30000);
  script_abs(abs_fd,ABS_X,31540);script_abs(abs_fd,ABS_Y,1311); /* (77,1) */
  usleep(30000); /* all three moves processed while the button is down */
  script_mice(mice_fd,0,0,0); /* release from the aggregate */
 } /* "none": no pointer events at all */
}
/* Physical cells the selection covers, so the harness can assert the drag
 * span without reimplementing the renderer. */
static void inverse_report(char *out,size_t cap){ size_t r,off=0,physical=0;
 for(r=0;r<line_count;++r){unsigned int cells[MAX_COLS];unsigned char inv[MAX_COLS],conn[MAX_COLS];size_t i,count,v0=0;int any=0;
  count=build_cells(r,cells,inv,conn,&v0);
  for(i=0;i<count;++i)if(inv[i]){long x=(long)((i/terminal_cols+1)*terminal_cols-count+i),y=(long)(physical+i/terminal_cols);
   if(off+24>cap)return;
   if(any){out[off++]=',';}else{any=1;if(off){out[off++]=';';}off+=(size_t)snprintf(out+off,cap-off,"%ld:",y);}
   off+=(size_t)snprintf(out+off,cap-off,"%ld",x);}
  physical+=line_rows(r);}
}
/* Written into the escape stream when the scripted pointer stream ends (the
 * child closed its pipe), so the harness can sync and read the coalesced
 * outcome.  Emitted after the final paint; the harness strips this line
 * before feeding the screen.  Never emitted in the real editor. */
static void emit_test_marker(void){ char inv[512],buf[768];int n;
 if(!test_marker||marker_done)return;marker_done=1;
 inv[0]=0;
 inverse_report(inv,sizeof(inv));
 n=snprintf(buf,sizeof(buf),"@AR-TYPE43 mode=%s bytes=%ld visible=%d cell=(%ld,%ld) sel=(%zu,%zu)-(%zu,%zu) inverse=%s\n",
  test_mode_name,out_bytes_total,pointer_visible,pointer_cell.x,pointer_cell.y,
  anchor_line,anchor_col,cursor_line,cursor_col,inv);
 {ssize_t w=write(tty_fd,buf,(size_t)n);(void)w;}
}
/* Task 52: one record per coalesced paint, written to the tty only in the
 * sweep8 test mode (test_paint_marker), right after the paint bytes have
 * flushed.  n is the count of accepted movement events so far, so the
 * harness can align the record with the same event's calibration-log line
 * and check paint == model at intermediate points of the sweep.  Never
 * emitted in the real editor. */
static void emit_paint_marker(void){ char buf[128];int n;
 if(!test_paint_marker)return;
 n=snprintf(buf,sizeof(buf),"@AR-TYPE52P n=%ld cell=(%ld,%ld) painted=(%ld,%ld)\n",test_event_count,pointer_cell.x,pointer_cell.y,painted_cell.x,painted_cell.y);
 {ssize_t w=write(tty_fd,buf,(size_t)n);(void)w;}
}
static int burst_test(const char *mode){ struct winsize ws;struct PointerDevice d,dr,dm;pid_t child;int p[2],q[2],m[2],rc;
 tty_fd=open("/dev/tty",O_RDWR);if(tty_fd<0)return 1;
 terminal_cols=80;terminal_rows=25;
 if(!ioctl(tty_fd,TIOCGWINSZ,&ws)){if(ws.ws_col)terminal_cols=ws.ws_col;if(ws.ws_row)terminal_rows=ws.ws_row;}
 if(!terminal_cols)terminal_cols=80;if(!terminal_rows)terminal_rows=25;
 read_framebuffer_size();
 pointer_debug=getenv("AR_TYPE_POINTER_DEBUG")!=NULL;
 configure_pointer_drawing();
 pointer_arrow_glyph=0xE020;
 memset(lines,0,sizeof(lines));line_count=2;
 lines[0].cp[0]=0x0645;lines[0].cp[1]=0x0644;lines[0].cp[2]=0x0643;lines[0].len=3;
 lines[1].cp[0]=0x0628;lines[1].cp[1]=0x0633;lines[1].cp[2]=0x0645;lines[1].len=3;
 cursor_line=1;cursor_col=3;anchor_line=cursor_line;anchor_col=cursor_col;
 pointer_visible=0;pointer_down=0;drag=(struct DragState){0};painted_cell=(struct Cell){-1,-1};
 pointer_evdev_delivered=0;
 memset(pointer_devices,0,sizeof(pointer_devices));pointer_device_count=3;
 d=(struct PointerDevice){0};d.kind=POINTER_ABS;d.min_x=0;d.max_x=32767;d.min_y=0;d.max_y=32767;snprintf(d.node,sizeof(d.node),"/dev/input/burst-pipe");
 if(pipe(p))return 1;fcntl(p[0],F_SETFL,fcntl(p[0],F_GETFL)|O_NONBLOCK);d.fd=p[0];pointer_devices[0]=d;
 dr=(struct PointerDevice){0};dr.kind=POINTER_REL;snprintf(dr.node,sizeof(dr.node),"/dev/input/burst-pipe-rel");
 if(pipe(q)){close(p[0]);close(p[1]);return 1;}fcntl(q[0],F_SETFL,fcntl(q[0],F_GETFL)|O_NONBLOCK);dr.fd=q[0];pointer_devices[1]=dr;
 dm=(struct PointerDevice){0};dm.kind=POINTER_MICE;snprintf(dm.node,sizeof(dm.node),"/dev/input/burst-pipe-mice");
 if(pipe(m)){close(p[0]);close(p[1]);close(q[0]);close(q[1]);return 1;}fcntl(m[0],F_SETFL,fcntl(m[0],F_GETFL)|O_NONBLOCK);dm.fd=m[0];pointer_devices[2]=dm;
 test_mode_name=mode;test_marker=1;marker_done=0;
 test_paint_marker=(mode&&!strcmp(mode,"sweep8"));test_event_count=0;
 child=fork();
 if(child==0){close(p[0]);close(q[0]);close(m[0]);burst_script(mode,p[1],q[1],m[1]);close(p[1]);close(q[1]);close(m[1]);_exit(0);}
 close(p[1]);close(q[1]);close(m[1]);
 if(redraw(0)){rc=1;goto out;}
 rc=editor_loop()<0?1:0;
 out: if(pointer_devices[0].fd>=0)close(pointer_devices[0].fd);
 if(pointer_devices[1].fd>=0)close(pointer_devices[1].fd);
 if(pointer_devices[2].fd>=0)close(pointer_devices[2].fd);
 kill(child,SIGTERM);(void)waitpid(child,NULL,0);
 close(tty_fd);return rc;
}
/* ---- Task 51: one-shot input diagnostic ---- */
/* Reads a console (default /dev/tty1) in raw mode for ~20 s and writes a hex
 * dump of every byte the console delivers to /tmp/tty-keys.log - one "%02x"
 * line per byte, in arrival order, after a header line.  Nothing is ever
 * written to the screen, and the saved termios are restored before exit.
 * The VM profile runs this INSTEAD of the editor for one boot, so the log
 * records exactly what the console delivers for Super+Space and Ctrl+Space -
 * whether the kernel's keymap and kbd_mode layers turn them into ESC+0x20 or
 * leave them as a bare space. */
static int tty_keys_probe(const char *node,int seconds){ struct termios saved,raw;int fd;time_t start;FILE *f;
 if(!node||!*node)node="/dev/tty1";
 if(seconds<=0)seconds=20;
 fd=open(node,O_RDWR);
 if(fd<0){fprintf(stderr,"ar-type --tty-keys: cannot open %s: %s\n",node,strerror(errno));return 1;}
 if(tcgetattr(fd,&saved)){fprintf(stderr,"ar-type --tty-keys: tcgetattr %s: %s\n",node,strerror(errno));close(fd);return 1;}
 raw=saved;cfmakeraw(&raw);raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
 if(tcsetattr(fd,TCSANOW,&raw)){fprintf(stderr,"ar-type --tty-keys: tcsetattr %s: %s\n",node,strerror(errno));close(fd);return 1;}
 f=fopen("/tmp/tty-keys.log","w");
 if(f){fprintf(f,"# ar-type --tty-keys node=%s seconds=%d\n",node,seconds);fclose(f);}
 start=time(NULL);
 while(time(NULL)-start<seconds){struct pollfd p={.fd=fd,.events=POLLIN};int pr=poll(&p,1,1000);
  if(pr>0){unsigned char b;ssize_t n=read(fd,&b,1);
   if(n==1){f=fopen("/tmp/tty-keys.log","a");if(f){fprintf(f,"%02x\n",b);fclose(f);}}
  }else if(pr<0&&errno!=EINTR)break;
 }
 tcsetattr(fd,TCSANOW,&saved);close(fd);return 0;
}
/* Task 55: env-gated startup trace.  With AR_TYPE_START_DEBUG=1, one
 * timestamped line per startup step is appended to /tmp/ar-type-start.log -
 * opening the tty, tcgetattr, tcsetattr (errno on failure), the window size,
 * the screen model, the first frame's byte count, entering the read loop,
 * and any poll/read error - so a console program that stops mid-startup
 * says where it stopped.  Nothing is ever written to the screen. */
static int start_debug;
static void start_trace(const char *fmt, ...) { struct timeval tv;struct tm tm;char stamp[32];FILE *f;va_list ap;
 if(!start_debug)return;
 f=fopen("/tmp/ar-type-start.log","a");if(!f)return;
 gettimeofday(&tv,NULL);localtime_r(&tv.tv_sec,&tm);
 strftime(stamp,sizeof(stamp),"%H:%M:%S",&tm);
 fprintf(f,"[%s.%03ld] ",stamp,(long)tv.tv_usec/1000);
 va_start(ap,fmt);vfprintf(f,fmt,ap);va_end(ap);
 fputc('\n',f);fclose(f);
}
#ifndef AR_TYPE_SHELL
static int justify_test(void) {
 static const unsigned int rasm[]={0x0631,0x0633,0x0645};
 static const unsigned int bisk[]={0x0628,0x064A,0x0633,0x0643};
 static const unsigned int taqwim[]={0x062A,0x0642,0x0648,0x064A,0x0645};
 static const unsigned int want_rasm[]={0xFEE2,0x0640,0x0640,0xFEB3,0xFEAD};
 static const unsigned int want_bisk[]={0xFEDA,0x0640,0xFEB4,0xFEF4,0xFE91};
 static const unsigned int want_taqwim[]={0xFEE2,0xFEF3,0xFEEE,0xFED8,0xFE97};
 const struct {const char *name;const unsigned int *src;size_t srcn;const unsigned int *want;} cases[]={
  {"rasm",rasm,3,want_rasm},{"bisk",bisk,4,want_bisk},{"taqwim",taqwim,5,want_taqwim}};
 unsigned int cells[MAX_COLS];unsigned char inv[MAX_COLS],conn[MAX_COLS];size_t i,j,n,vis=0;int ok=1;
 memset(lines,0,sizeof(lines));line_count=1;cursor_line=0;cursor_col=0;latin_mode=0;
 justify_width=0;memcpy(lines[0].cp,rasm,sizeof(rasm));lines[0].len=3;
 n=build_cells(0,cells,inv,conn,&vis);ok&=n==3&&cells[0]==0xFEE2&&cells[1]==0xFEB3&&cells[2]==0xFEAD;
 printf("JUSTIFY editor natural رسم: %zu cells %s\n",n,ok?"PASS":"FAIL");
 justify_width=5;
 for(i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
  memset(lines,0,sizeof(lines));memcpy(lines[0].cp,cases[i].src,cases[i].srcn*sizeof(unsigned int));lines[0].len=cases[i].srcn;cursor_col=lines[0].len;
  n=build_cells(0,cells,inv,conn,&vis);
  if(n!=5)ok=0;
  for(j=0;j<n&&j<5;++j)if(cells[j]!=cases[i].want[j])ok=0;
  printf("JUSTIFY editor %s: %zu cells",cases[i].name,n);
  for(j=0;j<n;++j)printf(" U+%04X",cells[j]);
  printf(" %s\n",n==5&&memcmp(cells,cases[i].want,5*sizeof(unsigned int))==0?"PASS":"FAIL");
 }
 memset(lines,0,sizeof(lines));memcpy(lines[0].cp,rasm,sizeof(rasm));lines[0].len=3;
 n=build_cells(0,cells,inv,conn,&vis);
 ok&=n==5&&visual_source(0,1)==2&&visual_source(0,2)==2;
 printf("JUSTIFY editor connector hit-test: %s\n",n==5&&visual_source(0,1)==2&&visual_source(0,2)==2?"PASS":"FAIL");
 terminal_cols=4;
 ok&=line_rows(0)==2&&cells[4]==0xFEAD;
 printf("JUSTIFY editor hard wrap: %s (5 cells in 4 columns, tail U+%04X on row 2)\n",line_rows(0)==2&&cells[4]==0xFEAD?"PASS":"FAIL",cells[4]);
 return ok?0:1;
}
int main(int argc,char **argv){struct termios raw;struct winsize ws;
 if(argc==2&&!strcmp(argv[1],"--justify-test"))return justify_test();
 if(argc==2&&!strcmp(argv[1],"--mouse-test"))return mouse_test();
 if(argc>=2&&!strcmp(argv[1],"--pointer-test")){int arrow=1,moves=3;if(argc>=3&&!strcmp(argv[2],"noarrow"))arrow=0;if(argc>=4)moves=atoi(argv[3]);return pointer_test(arrow,moves);}
 if(argc>=3&&!strcmp(argv[1],"--burst-test"))return burst_test(argv[2]);
 if(argc>=2&&!strcmp(argv[1],"--tty-keys")){const char *node=argc>=3?argv[2]:"/dev/tty1";int seconds=argc>=4?atoi(argv[3]):20;return tty_keys_probe(node,seconds);}
 if(argc>=2&&!strcmp(argv[1],"--pointer-test")){int arrow=1,moves=3;if(argc>=3&&!strcmp(argv[2],"noarrow"))arrow=0;if(argc>=4)moves=atoi(argv[3]);return pointer_test(arrow,moves);}
 load_justify_width();
 start_debug=getenv("AR_TYPE_START_DEBUG")!=NULL;
 start_trace("startup: begin pid=%ld",(long)getpid());
 tty_fd=open("/dev/tty",O_RDWR);
 if(tty_fd<0){start_trace("open /dev/tty: failed errno=%d (%s)",errno,strerror(errno));perror("ar-type: tty");return 1;}
 if(input_socket_start()<0){perror("ar-type: input socket");return 1;}atexit(input_socket_stop);
 start_trace("open /dev/tty: fd=%d",tty_fd);
 if(tcgetattr(tty_fd,&saved_termios)){start_trace("tcgetattr: failed errno=%d (%s)",errno,strerror(errno));perror("ar-type: tty");return 1;}
 start_trace("tcgetattr: ok");
 {int got_ws=0;
  if(!ioctl(tty_fd,TIOCGWINSZ,&ws)){got_ws=1;if(ws.ws_col)terminal_cols=ws.ws_col;if(ws.ws_row)terminal_rows=ws.ws_row;}
  if(!terminal_cols)terminal_cols=80;if(!terminal_rows)terminal_rows=25;
  start_trace("window size: ioctl=%s reported=%ux%u effective=%zux%zu",got_ws?"ok":"failed",got_ws?ws.ws_col:0,got_ws?ws.ws_row:0,terminal_cols,terminal_rows);}
 read_framebuffer_size();
 pointer_debug=getenv("AR_TYPE_POINTER_DEBUG")!=NULL;
 configure_pointer_drawing();
 pointer_arrow_glyph=font_has_cp(0xE020)?0xE020:0;
 start_trace("screen model: %zux%zu cells, fb %ldx%ld",terminal_cols,terminal_rows,framebuffer_w,framebuffer_h);
 have_termios=1;raw=saved_termios;cfmakeraw(&raw);
 if(tcsetattr(tty_fd,TCSANOW,&raw)){start_trace("tcsetattr(raw): failed errno=%d (%s)",errno,strerror(errno));return 1;}
 start_trace("tcsetattr(raw): ok");
 atexit(restore_terminal);signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGHUP,on_signal);discover_pointer();
 {long paint_before=out_bytes_total;
  if(redraw(0)){start_trace("first paint: FAILED, %ld bytes written",out_bytes_total-paint_before);goto done;}
  start_trace("first paint: %ld bytes written",out_bytes_total-paint_before);}
 /* Task 60: locate the typing frame on the real console, then paint the
  * arrow at the tablet's registered position.  The reply arrives as
  * keyboard input and is consumed by the loop (cpr_pending); a terminal
  * that never answers keeps the origin at 0 and paints the arrow there. */
 if(pointer_device_count>0&&is_vt_console()){
  cpr_pending=1;cpr_sent_ms=now_ms();
  if(bytes("\033[6n"))goto done;if(flush_out())goto done;
  start_trace("cpr: query sent");
 }
 start_trace("entering read loop");
 (void)editor_loop();
 done:{size_t i;for(i=0;i<pointer_device_count;++i)close(pointer_devices[i].fd);}input_socket_stop();return 0;
}
#endif /* !AR_TYPE_SHELL (typing main) */
/* The main loop: poll the keyboard and every pointer device.  Each cycle
 * drains all pending pointer events, coalesces them to the last cell and
 * paints at most once (pointer_coalesce_paint), then handles at most one
 * keyboard byte.  Painting is a single buffered write, so a keystroke never
 * waits on pointer painting for more than one paint, and a fast wiggle can
 * never queue repaints ahead of the keyboard. */
static int editor_loop(void){ enum{NORMAL,ESC,CSI,SS3}state=NORMAL;unsigned char b;unsigned int cp,acc=0;int need=0,got,p1=0,p2=0,field=0;
 while(1){struct pollfd fds[3+MAX_POINTER_DEVICES];int nf=3+MAX_POINTER_DEVICES,pr,timeout;size_t i;
  pointer_scan_if_due();
  fds[0].fd=tty_fd;fds[0].events=POLLIN;fds[1].fd=input_listen_fd;fds[1].events=POLLIN;fds[2].fd=input_client_fd;fds[2].events=POLLIN;
  for(i=0;i<MAX_POINTER_DEVICES;++i){fds[3+i].fd=i<pointer_device_count?pointer_devices[i].fd:-1;fds[3+i].events=POLLIN;}
  timeout=pointer_poll_timeout(cpr_pending?100:-1);
  pr=poll(fds,nf,timeout);if(pr<0){start_trace("poll: error errno=%d (%s)",errno,strerror(errno));continue;}
  if(pr==0){ /* task 60: a CPR query that is never answered must not leave the arrow unpainted */
   if(cpr_pending&&now_ms()-cpr_sent_ms>=2000){cpr_pending=0;screen_origin=0;ptr_trace("cpr timeout: origin=0 console_row=%ld",console_row);read_banner();paint_startup_arrow();}
   pointer_scan_if_due();
   continue;}
  if(pointer_coalesce_paint())return -1;
  /* A pointer fd that reached EOF is gone (a device vanished, or the scripted
   * pipe closed): close it and, in the scripted tests, publish the coalesced
   * outcome after the final paint has flushed. */
  for(i=0;i<pointer_device_count;++i)if(fds[3+i].revents&(POLLHUP|POLLERR|POLLNVAL)&&pointer_devices[i].fd>=0){drop_pointer_device(&pointer_devices[i]);emit_test_marker();}
  if((fds[2].revents&(POLLHUP|POLLERR|POLLNVAL))&&!(fds[2].revents&POLLIN)){close(input_client_fd);input_client_fd=-1;}
  if(fds[1].revents&POLLIN){input_socket_accept_newest();continue;}
  if(fds[2].revents&POLLIN){got=(int)input_socket_byte(&b);if(!got)continue;}
  else if(fds[0].revents&POLLIN)got=(int)read(tty_fd,&b,1);else continue;
  if(got!=1){if(got<0)start_trace("read: error errno=%d (%s)",errno,strerror(errno));else start_trace("read: EOF");continue;}
  {int again=1;while(again){int changed=0,was_selection=has_selection(),extend=0;again=0;
   if(need&&(b&0xc0)!=0x80){need=0;acc=0;again=1;continue;}
   if(state==ESC){if(b=='['){state=CSI;p1=p2=field=0;continue;}if(b=='O'){state=SS3;continue;}if(b==' '){latin_mode=!latin_mode;state=NORMAL;if(redraw(0))return -1;continue;}state=NORMAL;continue;}
   if(state==SS3){state=NORMAL;/* Arrow keys follow screen direction; edits remain logical. */if(b=='D'&&cursor_col<lines[cursor_line].len)++cursor_col;else if(b=='C'&&cursor_col)--cursor_col;else if(b=='H')cursor_col=0;else if(b=='F')cursor_col=lines[cursor_line].len;else continue;clear_selection();if(redraw(0))return -1;continue;}
   if(state==CSI){if(b>='@'&&b<='~'){int p=field?p2:p1;extend=(p1==1&&p2==2);state=NORMAL;if(b=='A'&&cursor_line){--cursor_line;cursor_col=extend?0:(cursor_col>lines[cursor_line].len?lines[cursor_line].len:cursor_col);changed=1;}else if(b=='B'&&cursor_line+1<line_count){++cursor_line;cursor_col=extend?0:(cursor_col>lines[cursor_line].len?lines[cursor_line].len:cursor_col);changed=1;}else if(b=='D'&&cursor_col<lines[cursor_line].len){++cursor_col;changed=1;}else if(b=='C'&&cursor_col){--cursor_col;changed=1;}else if(b=='H'||(b=='~'&&(p==1||p==7))){cursor_col=0;changed=1;}else if(b=='F'||(b=='~'&&(p==4||p==8))){cursor_col=lines[cursor_line].len;changed=1;}else if(b=='~'&&p==3){changed=has_selection()?delete_selection():delete_cp();}else if(b=='R'&&cpr_pending){cpr_pending=0;screen_origin=p1>0?(long)p1-1:0;ptr_trace("cpr reply p1=%d: origin=%ld console_row was %ld",p1,screen_origin,console_row);console_row=screen_origin;read_banner();if(!startup_arrow_done)paint_startup_arrow();}if(changed||(!extend&&was_selection)){if(!extend)clear_selection();if(redraw(0))return -1;}continue;}if(b==';'){field=1;continue;}if(b>='0'&&b<='9'){int *q=field?&p2:&p1;if(*q<1000000)*q=*q*10+b-'0';continue;}continue;}
   /* Task 51: Ctrl+Space reaches the console program as the single byte
    * 0x00.  kbd 2.6.4's kbd_mode exposes no meta option, so the ESC+0x20
    * chord (kept above - free, and it fires once the meta bit exists) never
    * arrives; 0x00 is the toggle the console can actually deliver.  It is a
    * control byte: it toggles the mode and inserts nothing. */
   if(b==0&&!need){latin_mode=!latin_mode;if(redraw(0))return -1;continue;}
   if(b==0x1b&&!need){state=ESC;continue;}
   /* Task 37: Ctrl+D is the shell's EOF - it exits ONLY on an empty line
    * with no selection (the escape hatch for a deliberate exit).  On a
    * line with any content, or with a selection active, the byte does
    * NOTHING at all: no buffer change, no screen change, not even a
    * redraw - byte-for-byte nothing. */
   if(b==4&&!need){if(!lines[cursor_line].len&&!has_selection())return 0;continue;}
   if(b==3&&!need){copy_selection();continue;}if(b==24&&!need){copy_selection();if(has_selection()&&delete_selection()&&redraw(0))return -1;continue;}if(b==22&&!need){if(paste_clipboard()&&redraw(0))return -1;continue;}if((b==8||b==127)&&!need){changed=has_selection()?delete_selection():backspace_cp();if(changed&&redraw(0))return -1;continue;}
   if(b==21&&!need){if(!cursor_valid())continue;if(has_selection())delete_selection();if(lines[cursor_line].len){lines[cursor_line].len=cursor_col=0;clear_selection();changed=1;}if((changed||was_selection)&&redraw(0))return -1;continue;}
   if((b=='\r'||b=='\n')&&!need){if(has_selection())delete_selection();if(split_line()&&redraw(0))return -1;continue;}got=decode_byte(b,&cp,&acc,&need);if(got==1){int freed=translate_freed_b(&cp);if(freed==0)continue;if(latin_mode)(void)translate_latin(&cp);if(has_selection())delete_selection();if(insert_cp(cp)){clear_selection();if(redraw(0))return -1;}}
  }}
 }
}
#ifdef AR_TYPE_SHELL
/* ======================================================================== *
 * Shell mode — the Arabic-native editor (work order 65).                    *
 *                                                                           *
 * Built from the SAME translation unit as the typing window, compiled with   *
 * -DAR_TYPE_SHELL=1.  The typing main (above) is excluded in this build, and *
 * the typing binary stays byte-identical because this whole block vanishes   *
 * in the preprocessor when AR_TYPE_SHELL is not defined.                     *
 *                                                                           *
 * The editor owns the logical line, shapes it with the existing engine       *
 * (build_cells), keeps its own in-memory history, and runs each committed    *
 * line through the installed شكل wrapper:                                    *
 *     execv(<AR_SHELL_WRAPPER>, {<wrapper>, <AR_SHELL_SHELL>, "-c", line})   *
 * with ARSH_SHELL=1 and stdin /dev/null.                                     *
 * ======================================================================== */

#define SH_HIST_MAX 64

static struct Line sh_history[SH_HIST_MAX];
static struct Line sh_draft;
static size_t sh_hist_count, sh_hist_pos;
static size_t sh_caret;              /* caret: a boundary between editing atoms */
static long sh_row_origin;           /* absolute console row of the live region */
static size_t sh_prev_rows;          /* rows the live region last occupied */
static struct termios sh_raw;        /* the editor's own raw termios */
static char sh_wrapper[256], sh_shell[256];

/* ---- WO79 D1: the prompt -------------------------------------------------
 * النظام/المستخدم/<dir>: at the RIGHT margin, its colon at the left end, the
 * typed command growing LEFTWARD after it (the RTL reading of
 * system/user/dir: command).  The third element is the session's own working
 * directory - read with getcwd(), never re-derived - so it follows المسار and
 * انتقل by construction.  Component names come from TERMS.md's decided tree
 * table; a component with no decided name keeps its on-disk name (no invented
 * translation).  A cwd that IS the home directory - compared by device+inode,
 * so the Arabic tree's /مدير bind of /root counts too - displays as "~", and a
 * prompt wider than half the line drops leading components for ".../" until it
 * fits, so the typed text always keeps room.  The prompt is rebuilt on every
 * paint, so it always shows where the session is now.  All of this lives
 * inside #ifdef AR_TYPE_SHELL: the typing build never sees it.
 *
 * The marker is three ASCII dots, not "…": U+2026 has NO glyph in
 * out/console-16x32.psfu or out/console-8x16.psfu (measured from the psfu
 * unicode tables) and the fonts are not part of this order. */
#define SH_ED_MAX 128                /* file lines the editor holds */
static const struct { const char *path, *name; } sh_dir_names[] = {
    { "/", "جذر" }, { "/usr", "موارد" }, { "/etc", "إعدادات" }, { "/bin", "برامج" },
    { "/sbin", "إدارة" }, { "/lib", "مكتبات" }, { "/var", "متغيرات" }, { "/tmp", "مؤقت" },
    { "/home", "منازل" }, { "/root", "مدير" }, { "/dev", "أجهزة" }, { "/proc", "عمليات" },
    { "/sys", "نواة" }, { "/run", "تشغيل" }, { "/boot", "إقلاع" }, { "/opt", "إضافات" },
    { "/srv", "خدمات" }, { "/mnt", "وصل" }, { "/media", "وسائط" },
    { "/usr/bin", "برامج" }, { "/usr/lib", "مكتبات" }, { "/usr/share", "مشترك" },
    { "/usr/local", "محلي" },
};
static unsigned int sh_prompt[MAX_COLS];
static size_t sh_prompt_len;              /* codepoints; 0 = no prompt shown */
static struct Line sh_line_save;          /* the typed line while the prompt shares lines[0] */
static int sh_ed_active;                  /* WO79 D2: the file editor owns the line */
static char sh_ed_path[4096];
static char sh_ed_name[MAX_COLS];         /* the operand as typed, for the saving row */
static struct Line sh_ed_lines[SH_ED_MAX];
static size_t sh_ed_count, sh_ed_row;
static int sh_ed_dirty, sh_ed_warned;
/* WO83: the editor's bounded VIEWPORT.  The region is anchored at an absolute
 * console row (sh_ed_top; -1 = not anchored yet), its LAST row is always the
 * caret line, and the lines above it are re-shaped from the buffer and
 * repainted on every paint - never appended, never assumed to still be there.
 * sh_ed_prev_rows is how many rows the region occupied at the last paint, so a
 * region that shrank can erase its own stale rows; sh_ed_static_now is how many
 * of this paint's rows the region owns above the live line. */
static long sh_ed_top = -1;
static size_t sh_ed_prev_rows, sh_ed_static_now;
/* The goal atom index a vertical move keeps while the caret travels; SIZE_MAX =
 * none, so the next Up/Down takes the caret's current atom as the goal. */
static size_t sh_ed_goal = (size_t)-1;

/* ---- WO85 D2: the pager (متصفح الصفحات) ----------------------------------
 * A full-screen built-in over LOGICAL text.  The file - or a captured
 * command's own logical output, never the shaping wrapper's display bytes -
 * is decoded to UTF-8 codepoints, split into display lines of at most MAX_COLS
 * codepoints (man 1 fold's rule: a segment too long breaks at its last space,
 * else at the cap), and every page is SHAPED AND MEASURED BY THE ONE ENGINE
 * (build_cells) through the editor's own row painter, exactly as a static
 * editor row is.  Rows per line and cells per row come from build_cells,
 * never from a byte count (man 3 wcwidth's job belongs to that engine here:
 * the pager adds no second shaper and no second measuring rule).  Rows
 * 0..terminal_rows-2 are the page; the LAST row is the status row (page
 * number, key hint, search input or a note).  The pager repaints its own
 * rectangle in place and keeps no scrollback: what leaves the top of a page
 * is gone until you page back to it. */
#define SH_PG_LINES_MAX 4096                  /* display lines the pager holds */
#define SH_PG_BYTES_MAX (4u * 1024u * 1024u)  /* bytes a source may pour in */
static int sh_pg_active;
static struct Line *sh_pg_lines;              /* display lines, <= MAX_COLS each */
static size_t *sh_pg_rowcnt, *sh_pg_before;   /* rows per line; rows before it */
static size_t sh_pg_count, sh_pg_total;
static size_t sh_pg_line, sh_pg_chunk;        /* the page's first (line, row) */
static unsigned int sh_pg_query[MAX_COLS];    /* the literal search text */
static size_t sh_pg_query_len;
static int sh_pg_searching;                   /* the status row is taking a / */
static unsigned int sh_pg_note[MAX_COLS];     /* status note, or empty */
static size_t sh_pg_note_len;

/* Arabic errno wording for the message rows; never English.  The same
 * vocabulary انتقل uses, as one helper for the new rows. */
static const char *sh_err_ar(int e) {
    if (e == EACCES) return "الإذن مرفوض";
    if (e == EISDIR) return "هو دليل";
    if (e == ENOTDIR) return "ليس دليلاً";
    if (e == ENOENT) return "لا يوجد ملف أو دليل بهذا الاسم";
    if (e == ENOSPC) return "لا توجد مساحة";
    return "تعذر الفتح";
}
static int sh_same_inode(const char *a, const char *b) {
    struct stat sa, sb;
    return !stat(a, &sa) && !stat(b, &sb) &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}
/* UTF-8 -> codepoints, the same forgiving rule as sh_emit_row. */
static size_t sh_utf8_cp(const char *text, unsigned int *out, size_t max) {
    size_t n = 0, pos = 0; unsigned int cp, acc = 0; unsigned char need = 0;
    while (text[pos] && n < max) {
        unsigned char b = (unsigned char)text[pos++];
        if (need && (b & 0xc0) != 0x80) { need = 0; acc = 0; }
        if (!need) {
            if (b < 0x80) cp = b;
            else if ((b & 0xe0) == 0xc0) { acc = b & 31; need = 1; continue; }
            else if ((b & 0xf0) == 0xe0) { acc = b & 15; need = 2; continue; }
            else if ((b & 0xf8) == 0xf0) { acc = b & 7; need = 3; continue; }
            else continue;
        } else { acc = (acc << 6) | (b & 63); if (--need) continue; cp = acc; }
        out[n++] = cp;
    }
    return n;
}
/* The shaped width of a codepoint array, measured by the ONE engine
 * (build_cells) on scratch line 1, which shell mode never otherwise uses. */
static size_t sh_cp_cells(const unsigned int *cp, size_t len) {
    unsigned int cells[MAX_COLS];
    unsigned char inv[MAX_COLS], conn[MAX_COLS];
    size_t unused = 0;
    if (len > MAX_COLS) len = MAX_COLS;
    memcpy(lines[1].cp, cp, len * sizeof(cp[0]));
    lines[1].len = len;
    return build_cells(1, cells, inv, conn, &unused);
}
static const char *sh_dir_name(const char *full) {
    size_t i;
    for (i = 0; i < sizeof(sh_dir_names) / sizeof(sh_dir_names[0]); ++i)
        if (!strcmp(sh_dir_names[i].path, full)) return sh_dir_names[i].name;
    return NULL;
}
/* The display form of the session's directory: "~" for the home directory
 * (or anything under it, by device+inode so the /مدير bind counts), decided
 * Arabic names for the rest, on-disk names where nothing is decided. */
static void sh_dir_display(char *out, size_t outsz) {
    char cwd[4096], home[4096];
    const char *hp = NULL, *env, *p;
    char *o = out;
    size_t i, n;

    if (!getcwd(cwd, sizeof cwd)) { snprintf(out, outsz, "%s", "جذر"); return; }
    env = getenv("HOME");
    if (env && *env) snprintf(home, sizeof home, "%s", env); else home[0] = 0;
    if (home[0]) {
        n = strlen(cwd);
        for (i = 1; i <= n; ++i) {                  /* cwd[0..i) is a dir boundary? */
            char anc[4096]; size_t al = i;
            if (i < n && cwd[i] != '/') continue;
            if (al >= sizeof anc) break;
            memcpy(anc, cwd, al); anc[al] = 0;
            if (sh_same_inode(anc, home)) hp = cwd + al;   /* longest match wins */
        }
    }
    if (hp) o += snprintf(o, (size_t)(out + outsz - o), "~");
    for (p = hp ? hp : cwd; *p && o < out + outsz - 1; ) {
        const char *s; size_t clen, k; char full[4096]; const char *name;
        while (*p == '/') ++p;
        if (!*p) break;
        s = p; while (*p && *p != '/') ++p; clen = (size_t)(p - s);
        if (o > out && o < out + outsz - 1) *o++ = '/';
        k = (size_t)(p - cwd);
        name = NULL;
        if (k < sizeof full) { memcpy(full, cwd, k); full[k] = 0; name = sh_dir_name(full); }
        if (name) { clen = strlen(name); s = name; }
        if (clen > (size_t)(out + outsz - 1 - o)) clen = (size_t)(out + outsz - 1 - o);
        memcpy(o, s, clen); o += clen;
    }
    *o = 0;
    if (!*out) { const char *r = sh_dir_name("/"); snprintf(out, outsz, "%s", r ? r : "/"); }
}
/* WO79 D1 (measured on the installed build): the ONE engine lays a maximal
 * ASCII run out FORWARD - the task-77 Latin-island rule, right for paths
 * quoted inside Arabic text.  For prompt text that rule glues the dir element
 * and the terminating colon into a single island, so "~" + ":" painted as
 * "/~:" (colon at the island's RIGHT end, cell 64 of the live row) and the
 * caret of the empty line sat three cells to its left: the typed command did
 * NOT grow after the colon.  The prompt array is display text only - it is
 * never the typed line, which sh_paint restores around every paint - so its
 * ASCII runs are laid out for the RTL reading before the engine sees them:
 * groups of name characters ([A-Za-z0-9._-]) stay forward (on-disk Latin names
 * keep reading forward), every other group (the separator slashes, the ~, the
 * terminating colon) is emitted in reverse group order with the group's own
 * order reversed.  A one-character run - an Arabic dir's colon, a separator -
 * is unchanged, so the engine's own output for Arabic dirs is untouched. */
static int sh_prompt_name_char(unsigned int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}
static void sh_prompt_run_order(unsigned int *cp, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t a, b, g, gs[64], gl[64], ng = 0, w = 0, k;
        unsigned int tmp[MAX_COLS];
        if (cp[i] >= 0x80) { ++i; continue; }
        a = i;
        while (i < n && cp[i] < 0x80) ++i;
        b = i;
        for (g = a; g < b && ng < sizeof gs / sizeof gs[0]; ) {
            int name = sh_prompt_name_char(cp[g]);
            size_t s = g;
            while (g < b && sh_prompt_name_char(cp[g]) == name) ++g;
            gs[ng] = s; gl[ng] = g - s; ++ng;
        }
        for (g = ng; g-- > 0; ) {
            if (sh_prompt_name_char(cp[gs[g]]))
                for (k = 0; k < gl[g]; ++k) tmp[w++] = cp[gs[g] + k];
            else
                for (k = gl[g]; k-- > 0; ) tmp[w++] = cp[gs[g] + k];
        }
        memcpy(cp + a, tmp, w * sizeof cp[0]);
    }
}
/* "النظام/المستخدم/<display>:" as codepoints; returns the count.  The array is
 * display-ordered (sh_prompt_run_order above) and only ever painted. */
static size_t sh_prompt_build(const char *disp, unsigned int *cp) {
    char text[4096 + 64];    /* must hold the labels + the longest sh_dir_display */
    size_t n;
    snprintf(text, sizeof text, "النظام/المستخدم/%s:", disp);
    n = sh_utf8_cp(text, cp, MAX_COLS);
    sh_prompt_run_order(cp, n);
    return n;
}
static void sh_prompt_refresh(void) {
    char disp[4096], cand[4096];
    unsigned int cp[MAX_COLS];
    size_t n, cap, ncomp = 0, cstart[64], clen[64], i;
    const char *p;

    if (sh_ed_active) { sh_prompt_len = 0; return; }   /* the editor line has no prompt */
    sh_dir_display(disp, sizeof disp);
    n = sh_prompt_build(disp, cp);
    cap = terminal_cols / 2; if (cap < 24) cap = 24;   /* the labels alone: 19 cells */
    if (sh_cp_cells(cp, n) > cap) {
        for (p = disp; *p && ncomp < 64; ) {           /* component offsets of the display */
            while (*p == '/') ++p;
            if (!*p) break;
            cstart[ncomp] = (size_t)(p - disp);
            while (*p && *p != '/') ++p;
            clen[ncomp] = (size_t)(p - disp) - cstart[ncomp];
            ++ncomp;
        }
        for (i = 1; i < ncomp; ++i) {                  /* longest fitting suffix wins */
            size_t j; int o = snprintf(cand, sizeof cand, "...");
            for (j = i; j < ncomp; ++j)
                o += snprintf(cand + o, sizeof cand - (size_t)o, "/%.*s",
                              (int)clen[j], disp + cstart[j]);
            n = sh_prompt_build(cand, cp);
            if (sh_cp_cells(cp, n) <= cap) break;
        }
        if (i >= ncomp) {                              /* even one component is too wide */
            if (ncomp > 0) {
                size_t keep;
                for (keep = clen[ncomp - 1]; keep > 0; --keep) {
                    snprintf(cand, sizeof cand, ".../%.*s",
                             (int)keep, disp + cstart[ncomp - 1]);
                    n = sh_prompt_build(cand, cp);
                    if (sh_cp_cells(cp, n) <= cap) break;
                }
                if (!keep) snprintf(cand, sizeof cand, "...");
            } else snprintf(cand, sizeof cand, "...");
        }
        snprintf(disp, sizeof disp, "%s", cand);
        n = sh_prompt_build(disp, cp);
    }
    memcpy(sh_prompt, cp, n * sizeof cp[0]);
    sh_prompt_len = n;
}

/* The shell's own scroll bookkeeping: a console scroll shifts every row up
 * one, so both the live region's origin and a painted arrow must follow. */
static void sh_scrolled(void) {
    if (sh_row_origin > 0) sh_row_origin--;
    if (sh_ed_top > 0) sh_ed_top--;          /* WO83: the editor's region shifts too */
    if (painted_cell.y >= 0) painted_cell.y--;
}

/* A diagnostic line appended to /tmp/ar-shell.log; nothing is written to the
 * screen.  Used for the CPR fallback the design says must be logged, never
 * guessed. */
static void sh_log(const char *fmt, ...) {
    FILE *f = fopen("/tmp/ar-shell.log", "a");
    va_list ap;
    if (!f) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* ---- editing atoms -------------------------------------------------------
 * An atom is a base codepoint plus the transparent marks that immediately
 * follow it.  lam+alef stays two atoms painting as one cell.  The caret is
 * always an atom boundary, so a mark is never orphaned and the caret never
 * lands inside a base+marks group. */
static size_t sh_atom_next(size_t i) {
    size_t k;
    if (i >= lines[0].len) return lines[0].len;
    k = i + 1;
    while (k < lines[0].len && transparent(lines[0].cp[k])) ++k;
    return k;
}
static size_t sh_atom_prev(size_t i) {
    size_t k;
    if (i == 0) return 0;
    k = i;
    while (k > 0 && transparent(lines[0].cp[k - 1])) --k;
    return k > 0 ? k - 1 : 0;
}
static void sh_delete_range(size_t a, size_t b) {
    memmove(&lines[0].cp[a], &lines[0].cp[b],
            (lines[0].len - b) * sizeof(lines[0].cp[0]));
    lines[0].len -= (b - a);
}

static void sh_ed_join_back(void);   /* WO83: defined with the editor below */
static void sh_ed_join_fwd(void);
/* Backspace deletes the atom logically BEFORE the caret (the cell to its
 * right on screen).  At index 0 of a FILE line (WO83) the atom before the
 * caret lives on the line above: the two lines join and that atom is deleted,
 * as the line-editor contract has it.  At the very first file line - and on
 * the shell's single line - it stays a no-op: no visible character may ever be
 * dropped. */
static void sh_backspace(void) {
    size_t a;
    if (sh_caret > lines[0].len) sh_caret = lines[0].len;
    if (sh_ed_active) sh_ed_goal = (size_t)-1;
    if (sh_caret == 0) {
        if (sh_ed_active && sh_ed_row > 0) sh_ed_join_back();
        return;
    }
    a = sh_atom_prev(sh_caret);
    sh_delete_range(a, sh_caret);
    sh_caret = a;
}
/* Delete removes the atom AT the caret; at the END of a file line (WO83) it
 * joins the line below into this one (nothing is deleted by that join). */
static void sh_delete_fwd(void) {
    size_t b;
    if (sh_caret > lines[0].len) sh_caret = lines[0].len;
    if (sh_ed_active) sh_ed_goal = (size_t)-1;
    if (sh_caret >= lines[0].len) {
        if (sh_ed_active && sh_ed_row + 1 < sh_ed_count) sh_ed_join_fwd();
        return;
    }
    b = sh_atom_next(sh_caret);
    sh_delete_range(sh_caret, b);
}
/* Insert a codepoint at the caret.  A transparent mark must attach to a
 * preceding base; a mark typed at the right margin (no base before it) is
 * refused so it is never orphaned. */
static int sh_insert(unsigned int cp) {
    if (sh_caret > lines[0].len) sh_caret = lines[0].len;   /* never a stale caret */
    if (sh_ed_active) sh_ed_goal = (size_t)-1;              /* typing resets the goal */
    if (transparent(cp) && sh_caret == 0) return 0;
    /* WO79 D1: the prompt shares lines[0] with the typed line when painted,
     * so the typed line may grow only to the room the prompt leaves. */
    if (lines[0].len + sh_prompt_len >= MAX_COLS) return 0;
    memmove(&lines[0].cp[sh_caret + 1], &lines[0].cp[sh_caret],
            (lines[0].len - sh_caret) * sizeof(lines[0].cp[0]));
    lines[0].cp[sh_caret] = cp;
    ++lines[0].len;
    sh_caret = sh_atom_next(sh_caret);
    return 1;
}

/* ---- history: at most 64 logical lines, in memory only -------------------
 * Stored as LOGICAL codepoints (no presentation forms).  Consecutive
 * duplicates collapse; the buffer is emptied when the editor exits. */
static void sh_history_push(void) {
    if (lines[0].len == 0) return;
    if (sh_hist_count > 0 &&
        sh_history[sh_hist_count - 1].len == lines[0].len &&
        !memcmp(sh_history[sh_hist_count - 1].cp, lines[0].cp,
                lines[0].len * sizeof(unsigned int)))
        return;                                   /* collapse consecutive dup */
    if (sh_hist_count == SH_HIST_MAX) {
        memmove(&sh_history[0], &sh_history[1],
                (SH_HIST_MAX - 1) * sizeof(sh_history[0]));
        sh_hist_count--;
    }
    sh_history[sh_hist_count] = lines[0];
    sh_hist_count++;
    sh_hist_pos = sh_hist_count;
}
static void sh_history_recall(size_t idx) {
    if (idx >= sh_hist_count) lines[0].len = 0;
    else lines[0] = sh_history[idx];
    sh_caret = lines[0].len;                      /* caret at the logical end */
}
static void sh_history_up(void) {
    if (sh_hist_count == 0) return;
    if (sh_hist_pos == sh_hist_count) sh_draft = lines[0];   /* keep the draft */
    if (sh_hist_pos > 0) sh_hist_pos--;
    sh_history_recall(sh_hist_pos);
}
static void sh_history_down(void) {
    if (sh_hist_pos >= sh_hist_count) return;
    sh_hist_pos++;
    if (sh_hist_pos == sh_hist_count) { lines[0] = sh_draft; sh_caret = lines[0].len; }
    else sh_history_recall(sh_hist_pos);
}

/* ---- caret position, derived from the CELL map, never a codepoint count ---
 * build_cells returns the number of cells to the RIGHT of the caret; that is
 * the caret's distance from the right edge, which is what a right-anchored
 * layout needs.  This is computed inline in sh_paint(). */

/* ---- painting the live region --------------------------------------------
 * The live region is the ONLY repaintable surface: the current line and the
 * caret (more than one row when the shaped line wraps).  The editor never
 * writes above its first row — committed command rows and program-output rows
 * above it are append-only. */
static int sh_paint_row(long crow, size_t cnt, const unsigned int *cl,
                        const unsigned char *co) {
    size_t pad = cnt < terminal_cols ? terminal_cols - cnt : 0, i;
    (void)crow;
    if (bytes("\r\033[K")) return -1;
    for (i = 0; i < pad; ++i) {
        int arrow = pointer_marker_visible() && pointer_cell.x == (long)i &&
                    pointer_cell.y == console_row;
        if (arrow) { painted_cell = pointer_cell; painted_cell.y = console_row; }
        if (arrow && bytes("\033[7m")) return -1;
        if (putcp(arrow && pointer_arrow_glyph ? pointer_arrow_glyph : 0x20)) return -1;
        if (arrow && bytes("\033[27m")) return -1;
    }
    for (i = 0; i < cnt; ++i) {
        int arrow = pointer_marker_visible() && pointer_cell.x == (long)(pad + i) &&
                    pointer_cell.y == console_row;
        unsigned int shown = arrow && pointer_arrow_glyph
            ? pointer_arrow_glyph
            : (co ? draw_codepoint(cl[i], co[i]) : cl[i]);
        if (arrow) { painted_cell = pointer_cell; painted_cell.y = console_row; }
        if (arrow && bytes("\033[7m")) return -1;
        if (putcp(shown)) return -1;
        if (arrow && bytes("\033[27m")) return -1;
    }
    return bytes("\033[0m");
}

/* ---- WO83: the editor's viewport -----------------------------------------
 * One rectangle: from sh_ed_top down to the caret line, which is its LAST row.
 * The window is the tail of the buffer that fits the row budget (terminal_rows
 * - 1, so the region never claims the whole screen and the shell's command row
 * above it stays readable).  A static row is shaped by the SAME engine as the
 * live line and painted by the same sh_paint_row - laid out alone, no prompt,
 * no caret - so the line a static row shows and the same line painted live are
 * the same cells.  Nothing above sh_ed_top is written: the transcript above
 * the region stays append-only, exactly as before this order. */

/* The shaped cell count of a buffer line; for the caret line also the cells to
 * the RIGHT of the caret, which is where the caret's row comes from.  lines[0]
 * is the caret line's live copy, so it is saved and restored around the shape:
 * every caller keeps its own caret. */
static size_t sh_ed_cells(const struct Line *l, int caret_line, size_t *right_out) {
    unsigned int cells[MAX_COLS];
    unsigned char inv[MAX_COLS], conn[MAX_COLS];
    struct Line save = lines[0];
    size_t save_caret = sh_caret, n, right = 0;
    int save_line = (int)cursor_line;
    lines[0] = *l;
    if (caret_line) { cursor_line = 0; cursor_col = sh_caret; }
    else { cursor_line = 1; cursor_col = 0; }    /* out of range: no caret */
    n = build_cells(0, cells, inv, conn, &right);
    lines[0] = save; sh_caret = save_caret; cursor_line = save_line;
    if (right_out) *right_out = right;
    return n;
}
/* Screen rows a buffer line occupies, chunked exactly as sh_paint chunks the
 * live line; an empty line still owns one row. */
static size_t sh_ed_line_rows(const struct Line *l, int caret_line) {
    size_t n, right = 0, content_rows, caret_row;
    n = sh_ed_cells(l, caret_line, &right);
    content_rows = (n + terminal_cols - 1) / terminal_cols;
    if (!caret_line) return content_rows > 0 ? content_rows : 1;
    caret_row = right / terminal_cols;
    return content_rows > caret_row + 1 ? content_rows : caret_row + 1;
}
/* The window: walk back from the caret line until the row budget is spent.
 * Returns the region's row count; the caret line is always in, even alone
 * (a single line taller than the budget). */
static size_t sh_ed_window(size_t *first) {
    size_t cap = terminal_rows > 1 ? terminal_rows - 1 : 1;
    size_t i = sh_ed_row, r = 0, f = sh_ed_row;
    while (1) {
        size_t lr = sh_ed_line_rows(&sh_ed_lines[i], i == sh_ed_row);
        if (r && r + lr > cap) break;
        r += lr; f = i;
        if (i == 0) break;
        --i;
    }
    *first = f;
    return r ? r : 1;
}
/* One screen row of a static line: chunk 0 is the line's first visual row,
 * the same chunking (and the same right anchoring) as the live line's paint. */
static int sh_ed_paint_line_row(const struct Line *l, size_t chunk) {
    unsigned int cells[MAX_COLS];
    unsigned char inv[MAX_COLS], conn[MAX_COLS];
    struct Line save = lines[0];
    size_t save_caret = sh_caret, n, lo, hi, cnt;
    int save_line = (int)cursor_line;
    lines[0] = *l;
    cursor_line = 1; cursor_col = 0;             /* a static row shows no caret */
    n = build_cells(0, cells, inv, conn, &hi);
    lines[0] = save; sh_caret = save_caret; cursor_line = save_line;
    if (chunk * terminal_cols >= n) return sh_paint_row(console_row, 0, NULL, NULL);
    lo = n > (chunk + 1) * terminal_cols ? n - (chunk + 1) * terminal_cols : 0;
    hi = n - chunk * terminal_cols;
    if (hi > n) hi = n;
    cnt = hi - lo;
    return sh_paint_row(console_row, cnt, &cells[lo], &conn[lo]);
}
/* Repaint the whole viewport.  Called at the top of sh_paint, BEFORE the live
 * line: the region paints the static lines above the caret line, leaves the
 * cursor on the caret line's first row, and sets sh_row_origin there - the
 * rest of sh_paint then paints the live line exactly as it always has.  A
 * region that shrank hands the live part the row count to erase below it. */
static int sh_ed_region(void) {
    size_t first, i, k, r = 0, lr;
    if (!sh_ed_active) { sh_ed_top = -1; sh_ed_prev_rows = 0; return 0; }
    if (sh_ed_count == 0) sh_ed_count = 1;
    if (sh_ed_row >= sh_ed_count) sh_ed_row = sh_ed_count - 1;
    (void)sh_ed_window(&first);
    if (sh_ed_top < 0) sh_ed_top = console_row;  /* first paint after open */
    if (move_rows(sh_ed_top - console_row)) return -1;
    for (i = first; i < sh_ed_row; ++i) {
        lr = sh_ed_line_rows(&sh_ed_lines[i], 0);
        for (k = 0; k < lr; ++k) {
            if (sh_ed_paint_line_row(&sh_ed_lines[i], k)) return -1;
            ++r;
            if (bytes("\r\n")) return -1;
            if (console_row == (long)terminal_rows - 1) sh_scrolled(); else console_row++;
        }
    }
    sh_row_origin = console_row;                 /* the caret line's first row */
    sh_prev_rows = sh_ed_prev_rows > r ? sh_ed_prev_rows - r : 0;
    sh_ed_static_now = r;
    return 0;
}

/* ---- WO85 A: the pager's screen ------------------------------------------
 * Layout, painting, movement and search.  The pager paints through sh_paint()
 * - it owns the same live surface the prompt does - so the pointer path and
 * Ctrl+L repaint a page exactly as they repaint the prompt, with no second
 * screen model. */
static int sh_paint(void);                    /* forward: the pager repaints via it */

static size_t sh_pg_budget(void) { return terminal_rows > 2 ? terminal_rows - 1 : 1; }
static size_t sh_pg_first_row(void) { return sh_pg_before[sh_pg_line] + sh_pg_chunk; }

/* Rows a page starting at (line, chunk) consumes: `budget` rows, or fewer at
 * the end of the text.  A line taller than the page is entered row by row -
 * the only way a row below the fold of one long line can be reached. */
static void sh_pg_advance(size_t *line, size_t *chunk, size_t budget) {
    size_t i = *line, ck = *chunk, rows = 0;
    while (i < sh_pg_count && rows < budget) {
        size_t take = sh_pg_rowcnt[i] - ck;
        if (take > budget - rows) take = budget - rows;
        rows += take; ck += take;
        if (ck >= sh_pg_rowcnt[i]) { ++i; ck = 0; }
    }
    *line = i; *chunk = ck;
}
/* One page back: `budget` rows walked backwards, one row at a time. */
static void sh_pg_rewind(size_t *line, size_t *chunk, size_t budget) {
    size_t i = *line, ck = *chunk, rows = 0;
    while (rows < budget && (i > 0 || ck > 0)) {
        if (ck > 0) --ck;
        else { --i; ck = sh_pg_rowcnt[i] - 1; }
        ++rows;
    }
    *line = i; *chunk = ck;
}
static void sh_pg_note_set(const char *text, const unsigned int *q, size_t qn) {
    sh_pg_note_len = sh_utf8_cp(text, sh_pg_note, MAX_COLS);
    if (q && qn && sh_pg_note_len + 2 + qn <= MAX_COLS) {
        sh_pg_note[sh_pg_note_len++] = ':';
        sh_pg_note[sh_pg_note_len++] = ' ';
        memcpy(sh_pg_note + sh_pg_note_len, q, qn * sizeof(q[0]));
        sh_pg_note_len += qn;
    }
}
/* Forward/back one page.  At either end the note says so and the page stays:
 * the pager never wraps, so a reader never loses the place. */
static void sh_pg_move(int dir) {
    size_t first = sh_pg_first_row();
    if (dir > 0) {
        if (first + sh_pg_budget() >= sh_pg_total) {
            sh_pg_note_set("النهاية", NULL, 0);
            return;
        }
        sh_pg_advance(&sh_pg_line, &sh_pg_chunk, sh_pg_budget());
    } else {
        if (first == 0) {
            sh_pg_note_set("البداية", NULL, 0);
            return;
        }
        sh_pg_rewind(&sh_pg_line, &sh_pg_chunk, sh_pg_budget());
    }
    sh_pg_note_len = 0;
}
/* Literal search: the query's codepoints, exactly, inside one display line. */
static int sh_pg_find(size_t from, size_t *found) {
    size_t i, j;
    if (!sh_pg_query_len) return 0;
    for (i = from; i < sh_pg_count; ++i) {
        const struct Line *l = &sh_pg_lines[i];
        if (l->len < sh_pg_query_len) continue;
        for (j = 0; j + sh_pg_query_len <= l->len; ++j)
            if (!memcmp(l->cp + j, sh_pg_query, sh_pg_query_len * sizeof(sh_pg_query[0]))) {
                *found = i;
                return 1;
            }
    }
    return 0;
}
static void sh_pg_goto(size_t hit) {
    sh_pg_line = hit;
    sh_pg_chunk = 0;
    sh_pg_note_len = 0;
}
/* Next occurrence of the standing query, from the line after this page's
 * first line; a miss keeps the page and says so. */
static void sh_pg_search_next(void) {
    size_t hit;
    if (!sh_pg_query_len) return;
    if (sh_pg_find(sh_pg_line + 1, &hit)) sh_pg_goto(hit);
    else sh_pg_note_set("لم يوجد", sh_pg_query, sh_pg_query_len);
}
/* Build one row's cells from its codepoints, by the one engine, on scratch
 * line 1 (the sh_cp_cells pattern): the status row is a shaped row like every
 * other row on this console. */
static size_t sh_pg_shape(const unsigned int *cp, size_t n,
                          unsigned int *cells, unsigned char *conn) {
    unsigned char inv[MAX_COLS];
    size_t unused = 0;
    if (n > MAX_COLS) n = MAX_COLS;
    memcpy(lines[1].cp, cp, n * sizeof(cp[0]));
    lines[1].len = n;
    return build_cells(1, cells, inv, conn, &unused);
}
/* The status row: page number (right-anchored, like every row), then the
 * search input, a note, or the key hints - dropped again in that order if the
 * row would not fit, because the page number is what must stay. */
static int sh_pg_status(void) {
    unsigned int cp[MAX_COLS], cells[MAX_COLS];
    unsigned char conn[MAX_COLS];
    size_t n, cnt, keep, b = sh_pg_budget();
    char head[64];
    size_t page = sh_pg_first_row() / b + 1;
    size_t pages = (sh_pg_total + b - 1) / b;
    if (!pages) pages = 1;
    snprintf(head, sizeof head, "صفحة %zu من %zu", page, pages);
    n = sh_utf8_cp(head, cp, MAX_COLS);
    keep = n;
    if (sh_pg_searching) {
        n += sh_utf8_cp("  / ", cp + n, MAX_COLS - n);
        if (sh_pg_query_len <= MAX_COLS - n) {
            memcpy(cp + n, sh_pg_query, sh_pg_query_len * sizeof(cp[0]));
            n += sh_pg_query_len;
        }
    } else if (sh_pg_note_len) {
        n += sh_utf8_cp("  ", cp + n, MAX_COLS - n);
        if (sh_pg_note_len <= MAX_COLS - n) {
            memcpy(cp + n, sh_pg_note, sh_pg_note_len * sizeof(cp[0]));
            n += sh_pg_note_len;
        }
    } else {
        n += sh_utf8_cp("  مسافة: تالٍ، ن: تالٍ، ب: سابق، /: بحث، ق: خروج",
                        cp + n, MAX_COLS - n);
    }
    cnt = sh_pg_shape(cp, n, cells, conn);
    if (cnt > terminal_cols) {                 /* drop the tail, keep the page */
        cnt = sh_pg_shape(cp, keep, cells, conn);
        n = keep;
    }
    (void)n;
    if (sh_paint_row(console_row, cnt, cells, conn)) return -1;
    return bytes("\r");                        /* park at the row's left edge */
}
/* Repaint the whole page in place.  The cursor travels from wherever it is
 * (the prompt row the command was typed on) up to row 0 and paints downward;
 * every page row is written, blank rows included, so a previous, longer page
 * leaves nothing of itself behind. */
static int sh_pg_paint(void) {
    size_t budget = sh_pg_budget(), r, i = sh_pg_line, ck = sh_pg_chunk;
    if (move_rows(-console_row)) return -1;
    for (r = 0; r < budget; ++r) {
        int painted = 0;
        while (i < sh_pg_count && !painted) {
            if (ck >= sh_pg_rowcnt[i]) { ++i; ck = 0; continue; }
            if (sh_ed_paint_line_row(&sh_pg_lines[i], ck)) return -1;
            painted = 1;
            if (++ck >= sh_pg_rowcnt[i]) { ++i; ck = 0; }
        }
        if (!painted && sh_paint_row(console_row, 0, NULL, NULL)) return -1;
        if (r + 1 < budget) {
            if (bytes("\r\n")) return -1;
            if (console_row == (long)terminal_rows - 1) sh_scrolled();
            else console_row++;
        }
    }
    /* the status row sits one CR LF below the last page row */
    if (console_row != (long)terminal_rows - 1) {
        if (bytes("\r\n")) return -1;
        if (console_row == (long)terminal_rows - 1) sh_scrolled();
        else console_row++;
    }
    if (sh_pg_status()) return -1;
    return flush_out();
}

static int sh_paint(void) {
    unsigned int cells[MAX_COLS], V[MAX_COLS + 16];
    unsigned char inv[MAX_COLS], conn[MAX_COLS], Vconn[MAX_COLS + 16];
    size_t tc, N, i, r, cursor_right = 0, plen = 0;
    size_t content_rows, right, caret_row, target_rows;
    long caret_col;

    if (sh_pg_active) return sh_pg_paint();   /* WO85: the pager owns the screen */

    /* Work order 79 (D1): the prompt is rebuilt from the session's own cwd on
     * every paint.  Work order 80: it is NOT composed into lines[0] any more.
     * The typed line is laid out ALONE - exactly the cells it has with no
     * prompt at all - so nothing introduced for the prompt can reorder it: in
     * Latin mode the whole typed line is one forward island (spaces included)
     * and any Arabic keeps the established word order.  The prompt's own cells
     * are laid out ALONE too (order 79's sh_prompt_run_order still owns that
     * array and nothing else) and appended to the RIGHT of the typed cells,
     * its colon leftmost.  Only the caret is offset by the prompt's cell
     * count, so the typed line grows leftward from the colon exactly as it
     * did.  The typed line is restored before anything else looks at
     * lines[0].  Work order 66 (owner ruling): the mode indicator cell is
     * DROPPED.  With no prompt (the editor's file line) the anchor stays the
     * console's right border, exactly as work order 78 left it. */
    sh_ed_static_now = 0;
    if (sh_ed_region()) return -1;   /* WO83: the editor's viewport repaints first */
    sh_prompt_refresh();
    cursor_line = 0;
    cursor_col = sh_caret;
    tc = build_cells(0, cells, inv, conn, &cursor_right);
    if (sh_prompt_len && lines[0].len + sh_prompt_len <= MAX_COLS) {
        size_t pn, pvis = 0, k;
        unsigned int pcells[MAX_COLS];
        unsigned char pinv[MAX_COLS], pconn[MAX_COLS];
        sh_line_save = lines[0];
        memcpy(lines[0].cp, sh_prompt, sh_prompt_len * sizeof(lines[0].cp[0]));
        lines[0].len = sh_prompt_len;
        cursor_line = 1;            /* out of range: no caret lands in the prompt */
        pn = build_cells(0, pcells, pinv, pconn, &pvis);
        lines[0] = sh_line_save;
        cursor_line = 0;
        if (pn <= MAX_COLS - tc) {
            for (k = 0; k < pn; ++k) { cells[tc + k] = pcells[k]; conn[tc + k] = pconn[k]; }
            plen = pn;
        }
        cursor_right += plen;
    }
    tc += plen;
    N = tc;
    for (i = 0; i < tc; ++i) { V[i] = cells[i]; Vconn[i] = conn[i]; }

    content_rows = (N + terminal_cols - 1) / terminal_cols;
    right = cursor_right;
    caret_row = right / terminal_cols;
    caret_col = (long)(terminal_cols - 1 - (right % terminal_cols));
    target_rows = content_rows > caret_row + 1 ? content_rows : caret_row + 1;

    if (move_rows(sh_row_origin - console_row)) return -1;
    for (r = 0; r < target_rows; ++r) {
        size_t lo = 0, hi = 0, cnt = 0;
        if (r < content_rows) {
            lo = N > (r + 1) * terminal_cols ? N - (r + 1) * terminal_cols : 0;
            hi = N - r * terminal_cols;
            if (hi > N) hi = N;
            cnt = hi - lo;
        }
        if (sh_paint_row(console_row, cnt, r < content_rows ? &V[lo] : NULL,
                         r < content_rows ? &Vconn[lo] : NULL)) return -1;
        if (r + 1 < target_rows) {
            if (bytes("\r\n")) return -1;
            if (console_row == (long)terminal_rows - 1) sh_scrolled(); else console_row++;
        }
    }
    /* clear any rows a previous, longer paint left behind */
    if (sh_prev_rows > target_rows) {
        for (r = target_rows; r < sh_prev_rows; ++r) {
            if (bytes("\r\n")) return -1;
            if (console_row == (long)terminal_rows - 1) sh_scrolled(); else console_row++;
            if (sh_paint_row(console_row, 0, NULL, NULL)) return -1;
        }
    }
    sh_prev_rows = target_rows + sh_ed_static_now;
    if (sh_ed_active) sh_ed_prev_rows = sh_prev_rows;   /* WO83: the whole region */

    if (move_rows(sh_row_origin + (long)caret_row - console_row)) return -1;
    if (bytes("\r")) return -1;
    if (caret_col > 0) {
        char seq[32];
        snprintf(seq, sizeof(seq), "\033[%ldC", caret_col);
        if (bytes(seq)) return -1;
    }
    if (flush_out()) return -1;
    return 0;
}

/* Learn the absolute console row of the live region's first row with a
 * cursor-position report (ESC[6n -> ESC[r;cR), 2 s timeout.  On timeout the
 * previous value is kept and logged, never guessed. */
static long sh_learn_row(void) {
    long deadline, row = 0;
    int st = 0, after_semi = 0;
    if (bytes("\033[6n")) { flush_out(); return -1; }
    if (flush_out()) return -1;
    deadline = now_ms() + 2000;
    while (now_ms() < deadline) {
        struct pollfd pfd;
        int pr;
        unsigned char b;
        pfd.fd = tty_fd; pfd.events = POLLIN; pfd.revents = 0;
        pr = poll(&pfd, 1, 100);
        if (pr <= 0) continue;
        if (read(tty_fd, &b, 1) != 1) continue;
        if (st == 0) { if (b == 0x1b) st = 1; else st = 0; continue; }
        if (st == 1) { if (b == '[') st = 2; else st = 0; continue; }
        if (st == 2) {
            if (b >= '0' && b <= '9') { if (!after_semi) row = row * 10 + (b - '0'); continue; }
            if (b == ';') { after_semi = 1; continue; }   /* skip the column */
            if (b == 'R') return row > 0 ? row - 1 : 0;
            st = 0; row = 0; after_semi = 0;
        }
    }
    return -1;
}

/* Encode a codepoint as UTF-8; returns the byte count. */
static size_t sh_utf8(unsigned int cp, char *o) {
    if (cp <= 0x7f) { o[0] = (char)cp; return 1; }
    if (cp <= 0x7ff) { o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp <= 0xffff) {
        o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63));
        o[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}

/* Work order 78 (owner note 3): a message row the console itself draws
 * anchors at the RIGHT margin, the way the banner and the live line do.  The
 * anchor is THIS ONE SWITCH: the number of blank cells printed before the
 * row's first cell.  A row at least as wide as the terminal gets no pad - it
 * fills the row and the tty's own wrap continues it on the next line.
 * Flipping the anchor back to the left margin is `return 0;` here and nowhere
 * else; the shaper and the message texts are untouched. */
static size_t msg_right_pad(size_t cells) {
    return cells < terminal_cols ? terminal_cols - cells : 0;
}

/* Built-ins use the same shaping engine as the editor.  They emit logical rows
 * through build_cells; no second placement rule is introduced here.
 * `lead_newline` is the only knob: every ordinary message row leads with the
 * blank line the transcript has always had, while the WO82 مساعدة table emits
 * COMPACTLY - its first row keeps the lead newline (so the table starts on a
 * fresh line below the command line) and the rest follow one per line.  The
 * table cannot afford double spacing: its entries doubled would cost far more
 * than the console's 25 rows (19 entries already cost 38 of them, and N4e's 24
 * leave even less room) and the first entries would scroll away before they
 * could be read (measured, REPORT-82 §S1).  Cells, shaping, padding and the scroll
 * bookkeeping are identical on both paths. */
static int sh_emit_row_spaced(const char *text, int lead_newline) {
    size_t i, n, pos = 0; unsigned int cp, acc = 0, cells[MAX_COLS];
    unsigned char need = 0, inv[MAX_COLS], conn[MAX_COLS];
    lines[0].len = 0;
    while (text[pos] && lines[0].len < MAX_COLS) {
        unsigned char b = (unsigned char)text[pos++];
        if (need && (b & 0xc0) != 0x80) { need = 0; acc = 0; }
        if (!need) { if (b < 0x80) { cp=b; } else if ((b&0xe0)==0xc0) { acc=b&31; need=1; continue; } else if ((b&0xf0)==0xe0) { acc=b&15; need=2; continue; } else if ((b&0xf8)==0xf0) { acc=b&7; need=3; continue; } else continue; }
        else { acc=(acc<<6)|(b&63); if (--need) continue; cp=acc; }
        lines[0].cp[lines[0].len++] = cp;
    }
    cursor_line=0; cursor_col=lines[0].len; n=build_cells(0,cells,inv,conn,&pos);
    if (lead_newline) {
        if (bytes("\r\n")) return -1;
        if (console_row == (long)terminal_rows-1) sh_scrolled(); else console_row++;
    }
    /* WO78 D2: right-anchor the row (one switch, msg_right_pad). */
    for (i=0;i<msg_right_pad(n);i++) if (putcp(0x20)) return -1;
    for (i=0;i<n;i++) if (putcp(draw_codepoint(cells[i],conn[i]))) return -1;
    if (bytes("\r\n")) return -1;
    if (console_row == (long)terminal_rows-1) sh_scrolled(); else console_row++;
    lines[0].len=0; cursor_col=0; return 0;
}
static int sh_emit_row(const char *text) { return sh_emit_row_spaced(text, 1); }

/* Re-anchor the live region below whatever was just written and repaint it.
 * The shared return path: the CPR gives the true row after the emitted rows
 * (a child may also have changed the termios). */
static void sh_return_to_live(void) {
    if (flush_out()) return;
    tcsetattr(tty_fd, TCSANOW, &sh_raw);   /* شغّل may have run a child */
    if (is_vt_console()) {
        long r0 = sh_learn_row();
        if (r0 >= 0) { sh_row_origin = r0; console_row = r0; }
        else sh_log("cpr timeout after built-in: keeping row_origin=%ld", sh_row_origin);
    } else {
        sh_row_origin = console_row;   /* pty: the tracked row after the CR LF */
    }
    /* WO83: whatever was just written (a message, a command's output) sits
     * ABOVE the region, so the editor's viewport re-anchors to the row it was
     * given and repaints from there; the region's old rows above are stale and
     * deliberately not erased (the transcript rule every message already had). */
    if (sh_ed_active) { sh_ed_top = sh_row_origin; sh_ed_prev_rows = 0; }
    pointer_visible = pointer_device_count > 0;
    sh_prev_rows = 0;
    (void)sh_paint();
}

/* ---- WO79 D2: the console file editor (حرّر) -----------------------------
 * The file's lines live in memory; everything before the current line is
 * drawn through sh_emit_row (the same shaped, right-anchored rows every other
 * message uses, marks and all), and the CURRENT line IS lines[0] - so typing,
 * shaping, marks, the RTL line, the cursor, Backspace/Delete and the
 * Latin toggle behave exactly as they already do on the shell's own line.
 * Enter commits the current line as a row and starts the next one, the same
 * append-only move a committed command makes.  Ctrl+S writes every line as
 * UTF-8, one LF per line, in logical order; Ctrl+Q leaves (a second Ctrl+Q
 * when the buffer changed without saving); Ctrl+C never touches the buffer. */
static int sh_write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}
static void sh_ed_emit_line(const struct Line *l) {
    char buf[MAX_COLS * 4 + 4];
    size_t i, n = 0;
    for (i = 0; i < l->len; ++i) n += sh_utf8(l->cp[i], buf + n);
    buf[n] = 0;
    (void)sh_emit_row(buf);
}
/* A message row emitted while the editor owns lines[0]: sh_emit_row uses
 * lines[0] as scratch (it zeroes it), so the file's current line - and the
 * caret inside it - are saved and restored around it.  Without this the
 * editor's own warning row would destroy the buffer it is warning about. */
static void sh_ed_msg(const char *text) {
    struct Line keep = lines[0];
    size_t keep_caret = sh_caret;
    (void)sh_emit_row(text);
    lines[0] = keep;
    sh_caret = keep_caret;
}
/* حرّر <file>: open an existing file or start a new one.  The caps are
 * refusals, never silent truncation: a file the buffer cannot hold whole is
 * reported and NOT opened, so Ctrl+S can never write back a shortened file. */
static int sh_edit_open(const char *name) {
    char row[4600], *buf = NULL;
    size_t cap = SH_ED_MAX * MAX_COLS * 4, used = 0, i, linestart = 0, n;
    unsigned int tmp[MAX_COLS + 1];
    int fd; ssize_t got;

    if (sh_ed_active) { sh_ed_msg("حرّر: محرر مفتوح بالفعل"); return 1; }
    fd = open(name, O_RDONLY);
    if (fd < 0 && errno != ENOENT) {
        snprintf(row, sizeof row, "حرّر: تعذر فتح '%s': %s", name, sh_err_ar(errno));
        sh_emit_row(row); return 1;
    }
    sh_ed_count = 0;
    if (fd >= 0) {
        buf = malloc(cap + 1);
        if (!buf) { close(fd); sh_emit_row("حرّر: لا توجد ذاكرة"); return 1; }
        while (used < cap) {
            got = read(fd, buf + used, cap - used);
            if (got < 0) {
                if (errno == EINTR) continue;
                snprintf(row, sizeof row, "حرّر: تعذر فتح '%s': %s", name, sh_err_ar(errno));
                free(buf); close(fd); sh_emit_row(row); return 1;
            }
            if (got == 0) break;
            used += (size_t)got;
        }
        {   /* a file exactly at the cap must still be proven complete */
            char probe;
            close(fd);
            fd = open(name, O_RDONLY);
            if (used == cap && fd >= 0) {
                off_t off = lseek(fd, (off_t)cap, SEEK_SET);
                if (off < 0 || read(fd, &probe, 1) > 0) {
                    if (fd >= 0) close(fd);
                    free(buf); sh_emit_row("حرّر: الملف كبير جدًا"); return 1;
                }
            }
            if (fd >= 0) close(fd);
        }
        buf[used] = 0;
        for (i = 0; i <= used; ++i) {
            if (i != used && buf[i] != '\n') continue;
            n = i - linestart;
            if (n && buf[linestart + n - 1] == '\r') --n;   /* CRLF files */
            buf[linestart + n] = 0;
            if (sh_ed_count >= SH_ED_MAX) {
                free(buf); sh_emit_row("حرّر: الملف كبير جدًا"); return 1;
            }
            {   /* n is the line's BYTE length; the codepoint count decides */
                size_t cps = sh_utf8_cp(buf + linestart, tmp, MAX_COLS + 1);
                if (cps > MAX_COLS) {
                    free(buf); sh_emit_row("حرّر: الملف كبير جدًا"); return 1;
                }
                /* WO95 D2: the same refusal rule for a line the codepoint
                 * model cannot hold WHOLE.  If the line's bytes are not
                 * exactly what its codepoints re-encode to - an embedded NUL,
                 * a malformed or truncated sequence, an overlong that decodes
                 * to NUL - the dropped bytes would be silently lost, and this
                 * loader's contract is that a file it cannot hold whole is
                 * reported and NOT opened, never written back shortened.  So
                 * such a file is refused UNCHANGED, exactly like one over the
                 * cap.  A line whose bytes DO round-trip is accepted as
                 * before, CRLF normalization included. */
                {
                    char chk[MAX_COLS * 4 + 4];
                    size_t k, nn = 0;
                    for (k = 0; k < cps; ++k) nn += sh_utf8(tmp[k], chk + nn);
                    if (nn != n || memcmp(chk, buf + linestart, n)) {
                        free(buf); sh_emit_row("حرّر: ملف غير نصي"); return 1;
                    }
                }
                memcpy(sh_ed_lines[sh_ed_count].cp, tmp, cps * sizeof(tmp[0]));
                sh_ed_lines[sh_ed_count].len = cps;
            }
            ++sh_ed_count;
            linestart = i + 1;
        }
        /* "abc\n" is ONE line, not one line plus an empty second: the final
         * LF TERMINATES the last line, it does not start a new one.  Without
         * this, open-then-Ctrl+S would append a blank line to such a file. */
        if (used && buf[used - 1] == '\n' && sh_ed_count > 1 &&
            sh_ed_lines[sh_ed_count - 1].len == 0)
            --sh_ed_count;
        free(buf);
    }
    if (sh_ed_count == 0) { sh_ed_lines[0].len = 0; sh_ed_count = 1; }
    snprintf(sh_ed_path, sizeof sh_ed_path, "%s", name);
    snprintf(sh_ed_name, sizeof sh_ed_name, "%s", name);
    /* WO83: the file is NOT emitted as rows any more - the viewport shows the
     * lines around the caret and repaints them.  The caret starts on the last
     * line (the same line the old append-only editor edited live); the region
     * anchors itself at the row this open leaves the cursor on. */
    sh_ed_row = sh_ed_count - 1;
    lines[0] = sh_ed_lines[sh_ed_row];
    sh_caret = lines[0].len;
    sh_ed_top = -1; sh_ed_prev_rows = 0; sh_ed_goal = (size_t)-1;
    sh_ed_active = 1; sh_ed_dirty = 0; sh_ed_warned = 0;
    return 1;
}

/* ---- WO83: moving inside the buffer --------------------------------------
 * The live line IS the buffer's caret line (lines[0] == sh_ed_lines[sh_ed_row]
 * by invariant).  These are the two directions of that mapping and nothing
 * else: commit the live line into its buffer row before the caret leaves it,
 * load the row the caret lands on into the live line. */
static void sh_ed_commit(void) {
    sh_ed_lines[sh_ed_row] = lines[0];         /* WO83-MAP */
}
static void sh_ed_load(void) {
    lines[0] = sh_ed_lines[sh_ed_row];
}
/* Move the caret line by dy, clamped to the buffer, keeping the goal atom
 * index (clamped to the target line, snapped to an atom boundary so a mark is
 * never orphaned).  Navigation is not an edit: the dirty flag does not move. */
static void sh_ed_move(long dy) {
    long n = (long)sh_ed_count, row;
    size_t want;
    if (n <= 1) return;
    if (sh_ed_goal == (size_t)-1) sh_ed_goal = sh_caret;
    sh_ed_commit();
    row = (long)sh_ed_row + dy;
    if (row < 0) row = 0;
    if (row > n - 1) row = n - 1;
    sh_ed_row = (size_t)row;
    sh_ed_load();
    want = sh_ed_goal < lines[0].len ? sh_ed_goal : lines[0].len;
    while (want > 0 && want < lines[0].len && transparent(lines[0].cp[want])) --want;
    sh_caret = want;
}
/* PageUp/PageDown: one viewport of lines, still bounded by the buffer. */
static void sh_ed_page(long dir) {
    long page = (long)terminal_rows - 1;
    if (page < 1) page = 1;
    sh_ed_move(dir * page);
}
/* Enter in the editor (WO83): SPLIT the line at the caret - the tail moves to
 * the line below and the caret starts it.  A caret already at the end of the
 * line therefore appends an empty line, which is exactly what the old
 * commit-and-append did, so the WO81 byte semantics are unchanged. */
static void sh_ed_newline(void) {
    size_t tail;
    if (sh_ed_count >= SH_ED_MAX) {            /* refuse, never drop a line */
        sh_ed_msg("حرّر: الملف كبير جدًا");
        sh_return_to_live(); return;
    }
    sh_ed_commit();                            /* the mapping: live line -> row */
    tail = lines[0].len - sh_caret;
    memmove(&sh_ed_lines[sh_ed_row + 2], &sh_ed_lines[sh_ed_row + 1],
            (sh_ed_count - sh_ed_row - 1) * sizeof(sh_ed_lines[0]));
    memcpy(sh_ed_lines[sh_ed_row + 1].cp, lines[0].cp + sh_caret,
           tail * sizeof(lines[0].cp[0]));
    sh_ed_lines[sh_ed_row + 1].len = tail;
    sh_ed_lines[sh_ed_row].len = sh_caret;
    ++sh_ed_count; ++sh_ed_row;
    sh_ed_load();
    sh_caret = 0;
    sh_ed_dirty = 1; sh_ed_goal = (size_t)-1;
    (void)sh_paint();                          /* the region repaints in place */
}
/* Backspace at column 0: join the line above into the caret's line.  NOTHING
 * but the line break is removed - only the atom the caret is at is ever
 * deleted, and there is none before it - so the caret lands on the join and
 * the bytes are exactly the two lines' bytes.  This is the only rule under
 * which the owner's requirement holds for BOTH join gestures: Backspace at
 * the start of the lower part and Delete at the end of the upper part must
 * both restore a line that was split in the middle.  (man 1 ed in this image
 * is GNU ed's stub page and documents no join semantics; the owner
 * requirement is the binding one.) */
static void sh_ed_join_back(void) {
    struct Line *up, *cur;
    size_t join;
    sh_ed_commit();
    up = &sh_ed_lines[sh_ed_row - 1];
    cur = &sh_ed_lines[sh_ed_row];
    join = up->len;                            /* the join point: the whole upper line */
    if (join + cur->len > MAX_COLS) {
        sh_ed_msg("حرّر: السطر طويل جدًا");     /* refuse: never drop a line */
        sh_return_to_live(); return;
    }
    memcpy(up->cp + join, cur->cp, cur->len * sizeof(cur->cp[0]));
    up->len = join + cur->len;
    memmove(cur, cur + 1, (sh_ed_count - sh_ed_row - 1) * sizeof(sh_ed_lines[0]));
    --sh_ed_count; --sh_ed_row;
    sh_ed_load();
    sh_caret = join;
    sh_ed_dirty = 1; sh_ed_goal = (size_t)-1;
    (void)sh_paint();
}
/* Delete at the end of a line: join the line below into this one.  Nothing is
 * deleted by this join; the caret stays where the two lines met. */
static void sh_ed_join_fwd(void) {
    struct Line *cur, *nx;
    size_t add;
    sh_ed_commit();
    cur = &sh_ed_lines[sh_ed_row];
    nx = &sh_ed_lines[sh_ed_row + 1];
    add = nx->len;
    if (cur->len + add > MAX_COLS) {
        sh_ed_msg("حرّر: السطر طويل جدًا");     /* refuse: never drop a line */
        sh_return_to_live(); return;
    }
    memcpy(cur->cp + cur->len, nx->cp, add * sizeof(nx->cp[0]));
    cur->len += add;
    memmove(nx, nx + 1, (sh_ed_count - sh_ed_row - 2) * sizeof(sh_ed_lines[0]));
    --sh_ed_count;
    sh_ed_load();
    sh_caret = lines[0].len - add;
    sh_ed_dirty = 1; sh_ed_goal = (size_t)-1;
    (void)sh_paint();
}
/* Ctrl+S: write the file - UTF-8, one LF per line, logical order.
 * WO81 D4: the write is ATOMIC - every line goes to a temporary file in the
 * SAME directory and rename() puts it in place, so a crash, a full disk or a
 * failed write can never leave a half-written file where the old one was: the
 * old file stays intact until the rename succeeds, and a failed save removes
 * the temporary.  The temporary takes the target's own mode (and a symlink is
 * resolved first, so saving through one rewrites its target, not the link),
 * so an atomic save can neither loosen the mode nor replace the link.
 * WO95 D1: the temporary's name is UNPREDICTABLE and its creation EXCLUSIVE
 * (mkstemp's O_CREAT|O_EXCL retry loop) in that same resolved directory, so a
 * leftover of an earlier run - and a link planted at any candidate name - can
 * never be opened, let alone truncated before the write; the unlink below can
 * therefore only ever remove the temporary THIS save created.  The mechanism
 * is mkstemp + fchmod, and the replacement is a rename, which replaces the
 * directory entry only: another hard link to the old inode keeps that inode
 * and its bytes (a symlink is resolved first, so saving through one still
 * rewrites its target, not the link). */
static void sh_ed_save_err(int e) {
    char row[4600];
    snprintf(row, sizeof row, "حفظ: تعذر الكتابة '%s': %s", sh_ed_name, sh_err_ar(e));
    sh_ed_msg(row); sh_return_to_live();         /* stay in the editor: buffer kept */
}
static int sh_ed_save(void) {
    char row[4600], tmp[4200], real[4096];
    const char *target = sh_ed_path;
    struct stat st;
    mode_t mode = 0644;
    int fd, e;
    size_t i;
    sh_ed_lines[sh_ed_row] = lines[0];
    if (lstat(target, &st) == 0 && S_ISLNK(st.st_mode) && realpath(target, real)) target = real;
    if (stat(target, &st) == 0 && S_ISREG(st.st_mode)) mode = st.st_mode & 07777;
    snprintf(tmp, sizeof tmp, "%s.tmp-XXXXXX", target);
    fd = mkstemp(tmp);                         /* O_CREAT|O_EXCL, random tail */
    if (fd < 0) { sh_ed_save_err(errno); return 0; }
    if (fchmod(fd, mode) != 0) { e = errno; close(fd); unlink(tmp); sh_ed_save_err(e); return 0; }
    for (i = 0; i < sh_ed_count; ++i) {
        char buf[MAX_COLS * 4 + 4];
        size_t j, n = 0;
        for (j = 0; j < sh_ed_lines[i].len; ++j)
            n += sh_utf8(sh_ed_lines[i].cp[j], buf + n);
        if (sh_write_all(fd, buf, n) || sh_write_all(fd, "\n", 1)) {
            e = errno; close(fd); unlink(tmp); sh_ed_save_err(e); return 0;
        }
    }
    /* WO95 D1: every failure path CLOSES the descriptor - an fsync failure
     * used to short-circuit the close() and leak the open temporary. */
    if (fsync(fd) != 0) { e = errno; close(fd); unlink(tmp); sh_ed_save_err(e); return 0; }
    if (close(fd) != 0) { e = errno; unlink(tmp); sh_ed_save_err(e); return 0; }
    if (rename(tmp, target) != 0) { e = errno; unlink(tmp); sh_ed_save_err(e); return 0; }
    sh_ed_active = 0; sh_ed_dirty = 0; sh_ed_warned = 0;
    snprintf(row, sizeof row, "حفظ: %s", sh_ed_name);
    (void)sh_emit_row(row);                    /* the owner's word: حفظ */
    lines[0].len = 0; sh_caret = 0;            /* AFTER the row: back at the prompt */
    sh_return_to_live();
    return 1;
}
/* Ctrl+Q: leave; changed-without-saving warns once and stays. */
static void sh_ed_quit(void) {
    if (sh_ed_dirty && !sh_ed_warned) {
        sh_ed_warned = 1;
        sh_ed_msg("تحذير: تعديلات غير محفوظة - Ctrl+S للحفظ أو Ctrl+Q مرة أخرى للخروج");
        sh_return_to_live();                   /* stay in the editor */
        return;
    }
    sh_ed_active = 0; sh_ed_dirty = 0; sh_ed_warned = 0;
    lines[0].len = 0; sh_caret = 0;
    sh_return_to_live();                       /* the live line becomes the prompt */
}

/* ---- WO85 B: the pager's session plumbing --------------------------------
 * One display line holds at most MAX_COLS codepoints.  A line that long folds
 * fold(1)-style: at its last space when it has one, else at the cap; a mark
 * arriving at a full line takes its base letter along, so no atom is ever cut
 * apart.  The caps are REFUSALS - in Arabic, with nothing opened - never
 * truncation: text the pager cannot hold whole is reported and not shown. */
static int sh_pg_put(struct Line *ln, size_t *lines, unsigned int c) {
    if (ln[*lines].len == MAX_COLS) {
        struct Line *l = &ln[*lines];
        size_t cut = 0, k;
        if (transparent(c)) {                 /* c belongs to the last atom */
            k = l->len;
            while (k > 0 && transparent(l->cp[k - 1])) --k;
            if (k > 0) --k;                   /* the base letter */
            cut = k;
        } else {
            for (k = 1; k < l->len; ++k)
                if (l->cp[k] == 0x20) cut = k + 1;    /* after the last space */
            if (cut == 0) cut = l->len;               /* no space: at the cap */
        }
        if (*lines + 1 >= SH_PG_LINES_MAX) return -1;
        ++*lines;
        ln[*lines].len = l->len - cut;
        memcpy(ln[*lines].cp, l->cp + cut, ln[*lines].len * sizeof(l->cp[0]));
        l->len = cut;
        if (ln[*lines].len >= MAX_COLS) {     /* a line of marks only: fresh one */
            if (*lines + 1 >= SH_PG_LINES_MAX) return -1;
            ++*lines;
            ln[*lines].len = 0;
        }
    }
    ln[*lines].cp[ln[*lines].len++] = c;
    return 0;
}
/* Load LOGICAL text into display lines.  A byte stream that is not text (a
 * NUL or any control byte outside tab/LF/CR) is refused in Arabic before
 * anything is opened, and so is text over either cap. */
static int sh_pg_load(const char *buf, size_t n, const char *what) {
    size_t i, ncp, lines = 0;
    unsigned int *cp;
    struct Line *ln;
    size_t *rc, *bf;
    int capped = 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 0x20 || c == 0x7f) {
            char row[4600];
            snprintf(row, sizeof row, "متصفح الصفحات: ليس نصاً: %s", what);
            sh_emit_row(row);
            return -1;
        }
    }
    cp = malloc((n + 1) * sizeof(cp[0]));
    ln = malloc(SH_PG_LINES_MAX * sizeof(ln[0]));
    rc = malloc(SH_PG_LINES_MAX * sizeof(rc[0]));
    bf = malloc(SH_PG_LINES_MAX * sizeof(bf[0]));
    if (!cp || !ln || !rc || !bf) {
        free(cp); free(ln); free(rc); free(bf);
        sh_emit_row("متصفح الصفحات: لا توجد ذاكرة");
        return -1;
    }
    ncp = sh_utf8_cp(buf, cp, n);
    ln[0].len = 0;
    for (i = 0; i < ncp; ++i) {
        unsigned int c = cp[i];
        if (c == 0x0a) {                          /* a line break */
            if (ln[lines].len && ln[lines].cp[ln[lines].len - 1] == 0x0d)
                --ln[lines].len;                  /* CRLF is one break */
            if (lines + 1 >= SH_PG_LINES_MAX) { capped = 1; break; }
            ln[++lines].len = 0;
            continue;
        }
        if (sh_pg_put(ln, &lines, c)) { capped = 1; break; }
    }
    free(cp);
    if (capped) {
        free(ln); free(rc); free(bf);
        sh_emit_row("متصفح الصفحات: النص كبير جدًا");
        return -1;
    }
    sh_pg_lines = ln; sh_pg_rowcnt = rc; sh_pg_before = bf;
    sh_pg_count = lines + 1;
    sh_pg_total = 0;
    for (i = 0; i < sh_pg_count; ++i) {
        rc[i] = sh_ed_line_rows(&ln[i], 0);       /* the one engine measures */
        bf[i] = sh_pg_total;
        sh_pg_total += rc[i];
    }
    return 0;
}
/* Read a whole file as it is on disk.  A directory, an unreadable file and
 * one over the cap are all refusals in Arabic, before the pager opens. */
static int sh_pg_read_file(const char *path, char **out, size_t *outn) {
    struct stat st;
    char *buf;
    size_t used = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char row[4600];
        snprintf(row, sizeof row, "متصفح الصفحات: تعذر فتح '%s': %s", path, sh_err_ar(errno));
        sh_emit_row(row);
        return -1;
    }
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        char row[4600];
        close(fd);
        snprintf(row, sizeof row, "متصفح الصفحات: '%s': هو دليل", path);
        sh_emit_row(row);
        return -1;
    }
    buf = malloc(SH_PG_BYTES_MAX + 2);
    if (!buf) { close(fd); sh_emit_row("متصفح الصفحات: لا توجد ذاكرة"); return -1; }
    while (used < SH_PG_BYTES_MAX) {
        ssize_t r = read(fd, buf + used, SH_PG_BYTES_MAX - used);
        if (r < 0) {
            if (errno == EINTR) continue;
            { char row[4600];
              snprintf(row, sizeof row, "متصفح الصفحات: تعذر فتح '%s': %s", path, sh_err_ar(errno));
              sh_emit_row(row); }
            free(buf); close(fd);
            return -1;
        }
        if (r == 0) break;
        used += (size_t)r;
    }
    if (used == SH_PG_BYTES_MAX) {                /* prove EOF, not truncation */
        char probe;
        if (read(fd, &probe, 1) > 0) {
            free(buf); close(fd);
            sh_emit_row("متصفح الصفحات: النص كبير جدًا");
            return -1;
        }
    }
    close(fd);
    buf[used] = 0;
    *out = buf; *outn = used;
    return 0;
}
/* PATH lookup, the rule execvp applies to a bare name: a name with a '/' in it
 * is used as it stands; otherwise each PATH directory is tried in order for an
 * executable.  Used by the pager's program mode (and by nothing else: the
 * ordinary line still goes through the شكل wrapper, order 84's contract). */
static int sh_program_path(const char *name, char *out, size_t outsz) {
    const char *p = getenv("PATH"), *s;
    size_t nl = strlen(name);
    if (strchr(name, '/')) { snprintf(out, outsz, "%s", name); return 1; }
    if (!p || !*p) p = "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin";
    while (*p) {
        size_t len;
        char cand[4096];
        s = p;
        while (*p && *p != ':') ++p;
        len = (size_t)(p - s);
        if (len && len + 1 + nl + 1 <= sizeof cand) {
            memcpy(cand, s, len);
            cand[len] = '/';
            memcpy(cand + len + 1, name, nl + 1);
            if (access(cand, X_OK) == 0) { snprintf(out, outsz, "%s", cand); return 1; }
        }
        if (*p == ':') ++p;
    }
    return 0;
}
/* Run a program and capture its LOGICAL stdout+stderr.  The child is exec'd
 * directly, so what lands in the pipe is the program's OWN text - and the
 * layer-1 tools' contract (tools/ar_util.h) is exactly that: they emit
 * logical UTF-8, because the شكل wrapper alone owns shaping, direction, cells
 * and width.  The pager therefore never puts the wrapper's display bytes into
 * a pipe: it shapes the captured text itself, page by page, with the one
 * engine.  While the capture lasts, Ctrl+C keeps its one meaning - SIGINT to
 * the program's own process group - and every other console byte is
 * discarded, exactly as an ordinary command's wait does it. */
static int sh_pg_run_capture(int argc, char **argv, char **out, size_t *outn) {
    char path[4096], *buf;
    int pfd[2], status = 0, done = 0;
    pid_t pid;
    size_t used = 0;
    if (!sh_program_path(argv[0], path, sizeof path)) {
        /* the operand is neither a file the filesystem has nor a program in
         * PATH: say so in the session's own words, never run anything. */
        sh_emit_row("متصفح الصفحات: لا يوجد ملف أو برنامج بهذا الاسم");
        return -1;
    }
    if (pipe(pfd) != 0) { sh_emit_row("متصفح الصفحات: تعذر تشغيل البرنامج"); return -1; }
    pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        sh_emit_row("متصفح الصفحات: تعذر تشغيل البرنامج");
        return -1;
    }
    if (pid == 0) {
        int nfd = open("/dev/null", O_RDONLY);
        close(pfd[0]);
        if (nfd >= 0) { if (dup2(nfd, 0) < 0) _exit(127); if (nfd != 0) close(nfd); }
        if (dup2(pfd[1], 1) < 0) _exit(127);
        if (dup2(pfd[1], 2) < 0) _exit(127);
        if (pfd[1] != 1) close(pfd[1]);
        setpgid(0, 0);
        execv(path, argv);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);
    buf = malloc(SH_PG_BYTES_MAX + 2);
    if (!buf) {
        kill(pid, SIGKILL); waitpid(pid, &status, 0); close(pfd[0]);
        sh_emit_row("متصفح الصفحات: لا توجد ذاكرة");
        return -1;
    }
    while (!done) {
        struct pollfd pf[2];
        int pr;
        pf[0].fd = tty_fd; pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = pfd[0]; pf[1].events = POLLIN; pf[1].revents = 0;
        pr = poll(pf, 2, 100);
        if (pr > 0 && (pf[0].revents & POLLIN)) {
            unsigned char in[256];
            ssize_t r = read(tty_fd, in, sizeof in), k;
            for (k = 0; k < r; ++k) if (in[k] == 0x03) kill(-pid, SIGINT);
        }
        if (pr > 0 && (pf[1].revents & (POLLIN | POLLHUP))) {
            for (;;) {
                ssize_t r = read(pfd[0], buf + used, SH_PG_BYTES_MAX - used);
                if (r > 0) { used += (size_t)r; continue; }
                if (r == 0) done = 1;             /* every writer closed */
                break;                            /* EAGAIN: nothing just now */
            }
            if (used == SH_PG_BYTES_MAX) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                close(pfd[0]); free(buf);
                sh_emit_row("متصفح الصفحات: النص كبير جدًا");
                return -1;
            }
        }
        if (!done) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) done = 1;               /* reaped: drain once more, done */
        }
    }
    close(pfd[0]);
    buf[used] = 0;
    *out = buf; *outn = used;
    return 0;
}
/* ق / q / Ctrl+C leave the pager.  The page on screen becomes the last thing
 * in the transcript and the live line comes back below it, exactly where an
 * ordinary built-in return puts it. */
static void sh_pg_quit(void) {
    sh_pg_active = 0;
    free(sh_pg_lines); free(sh_pg_rowcnt); free(sh_pg_before);
    sh_pg_lines = NULL; sh_pg_rowcnt = sh_pg_before = NULL;
    sh_pg_count = sh_pg_total = 0;
    sh_pg_line = sh_pg_chunk = 0;
    sh_pg_searching = 0; sh_pg_query_len = 0; sh_pg_note_len = 0;
    if (bytes("\r\n")) { flush_out(); return; }
    if (console_row == (long)terminal_rows - 1) sh_scrolled(); else console_row++;
    sh_return_to_live();
}
/* The pager's keys, in both scripts the console offers: the ASCII key, the
 * Arabic letter sharing the same key, the Latin letter the keymap's own
 * translation produces in Latin mode, and the freed physical sentinels.
 * Space and ف forward, ب back, / searches, ن the next hit, ق and q and Ctrl+C
 * leave, ESC 0x20 toggles the Arabic/Latin mode as at the prompt, and
 * PageUp/PageDown page like ب and space.  WO94 (N3a) made the dispatch a
 * single normalise-then-switch; see sh_pg_feed. */
static int sh_pg_feed(unsigned char b) {
    static int st = 0, p1 = 0;
    static unsigned int qacc = 0;
    static int qneed = 0;
    unsigned int cp;
    int got;
    if (st == 1) {                                /* after ESC */
        st = 0;
        if (b == '[') { st = 2; p1 = 0; return 0; }
        if (b == ' ') { latin_mode = !latin_mode; return sh_paint(); }
        return 0;
    }
    if (st == 2) {                                /* CSI */
        if (b >= '0' && b <= '9') { if (p1 < 100000) p1 = p1 * 10 + b - '0'; return 0; }
        if (b == ';') return 0;
        if (b == '~' && (p1 == 5 || p1 == 6)) {
            int dir = p1 == 6 ? 1 : -1;
            st = 0; p1 = 0;
            sh_pg_move(dir);
            return sh_paint();
        }
        st = 0;
        return 0;
    }
    if (b == 0x1b) { st = 1; return 0; }
    if (b == 0x03) { sh_pg_quit(); return 0; }
    if (sh_pg_searching) {
        if (b == '\r' || b == '\n') {
            size_t hit;
            sh_pg_searching = 0;
            if (!sh_pg_query_len) sh_pg_note_len = 0;
            else if (sh_pg_find(sh_pg_line, &hit)) sh_pg_goto(hit);
            else sh_pg_note_set("لم يوجد", sh_pg_query, sh_pg_query_len);
            return sh_paint();
        }
        if (b == 8 || b == 127) {
            if (sh_pg_query_len) --sh_pg_query_len;   /* the query holds no marks */
            return sh_paint();
        }
        got = decode_byte(b, &cp, &qacc, &qneed);
        if (got == 1) {
            if (translate_freed_b(&cp)) {
                if (latin_mode) (void)translate_latin(&cp);
                if (sh_pg_query_len < MAX_COLS) sh_pg_query[sh_pg_query_len++] = cp;
            }
            return sh_paint();
        }
        return 0;
    }
    /* WO94 (N3a / D4): ONE normalisation, ONE dispatch.
     *
     * What stood here was two comparisons bracketing the decode, and that
     * split WAS the defect.  The literal-ASCII test ran BEFORE the decode, so
     * it only ever matched bytes arriving raw; and the post-decode test
     * compared the ARABIC codepoints only - after translate_latin had already
     * turned the letter into its US form.  In Latin mode the keypress
     * therefore matched neither table and the pager ignored every command
     * key: off the generated pair table (ar_type_latin.h) the ب key arrives
     * as 'f', ق as 'r', ن as 'k' and ف as 't', none of which the old code
     * looked at.  The freed physical sentinels were worse: translate_freed_b
     * yields ASCII 'b'/'B' for U+E100/U+E101 in Latin mode, and a bare 'b'
     * matched nothing in the post-decode block at all.
     *
     * Normalising first, then dispatching once, puts every route on the same
     * transition: a bare ASCII byte, the Arabic letter, the Latin letter the
     * keymap's own translation produces, and the freed physical sentinels.
     * Space, '/', Ctrl+C, the ESC 0x20 toggle and the CSI 5~/6~ sequences are
     * handled by the state machine above and are untouched. */
    got = decode_byte(b, &cp, &qacc, &qneed);
    if (got != 1) return 0;
    {
        int freed = translate_freed_b(&cp);
        if (freed == 0) return 0;      /* Arabic mode: the sentinel is consumed */
        if (latin_mode) (void)translate_latin(&cp);
        switch (cp) {
        case 0x20: sh_pg_move(1); return sh_paint();                     /* space */
        case '/':  sh_pg_searching = 1; sh_pg_query_len = 0;
                   sh_pg_note_len = 0; return sh_paint();                /* search */
        case 'q': case 'Q': case 0x0642: sh_pg_quit(); return 0;         /* ق / q */
        case 'b': case 'B': case 0x0628: sh_pg_move(-1); return sh_paint();  /* ب / b */
        case 'n': case 'N': case 0x0646: sh_pg_search_next(); return sh_paint(); /* ن / n */
        case 'f': case 'F': case 0x0641: sh_pg_move(1); return sh_paint();   /* ف / f */
        }
    }
    return 0;
}

/* ---- WO85 D3: مساعدة lookup ---------------------------------------------
 * The table was refreshed by the owner's 2026-10-13 decision.  It has one
 * row per command (with genuine alternatives sharing a row), plus operational
 * hints.  The owner's 2026-10-13 round-5 decision restores three hints and
 * folds both navigation examples into the command row.  All 27 rows fit the
 * mini's 33x120 console after its six-row profile banner.
 * `مساعدة <word>` answers
 * the two questions the table alone cannot:
 *   * what is command X?  - an exact name, including the alternatives a "/"
 *     row carries ("اعرض الأذنات / اضبط الأذنات");
 *   * which commands mention W?  - W occurring in a name or a description.
 * A query that matches nothing keeps the shipped answer of the pinned case
 * ("مساعدة: ليس أمراً معروفاً: ") and no row is ever invented. */
static const char *const sh_help_rows[] = {
    "الأوامر:",
    "انتقل - غيّر دليل الجلسة؛ انتقل المنزل / انتقل لأعلى - إلى دليلك أو إلى الدليل الأعلى",
    "المسار - اعرض دليل الجلسة",
    "اعرض - قائمة ما في الدليل",
    "اطبع - اعرض محتوى ملف نصي",
    "أنشئ - أنشئ دليلاً",
    "انسخ - انسخ ملفاً أو دليلاً",
    "انقل / أعد التسمية - انقل بين الأدلة أو غيّر الاسم",
    "احذف - احذف ملفاً أو دليلاً فارغاً",
    "اعرض الأذونات / اضبط الأذونات - اعرض أذونات ملف أو دليل أو اضبطها",
    "ابحث - ابحث عن ملف باسمه أو نوعه",
    "فتّش - فتّش عن نص داخل الملفات",
    "حدّث الحزم - زامن المستودعات وحدّث الحزم المثبتة",
    "ثبّت حزمة - ثبّت حزمة أو أكثر",
    "أزل حزمة - أزل حزمة أو أكثر",
    "شغّل - شغّل برنامجاً واعرض حالة الخروج",
    "حرّر - افتح ملفًا نصيًا في المحرر",
    "متصفح - افتح متصفح الصفحات لملف أو خرج برنامج",
    "شكل - شكّل خرج برنامج عربي في الطرفية",
    "تاريخ - اعرض التاريخ الهجري",
    "مساعدة - هذه القائمة",
    "اخرج - أنه الجلسة",
    "البيئة / اضبط البيئة - اعرض متغيرات الجلسة أو اضبط متغيراً بقيمة حرفية",
    "تسويغ: AR_JUSTIFY_WIDTH=N يمد كلمة عربية منفردة إلى N خلايا بخلايا وصل",
    "حفظ: Ctrl+S",
    "الشرطة المائلة: Shift+م أو AltGr+ظ",
    "للمساعدة: مساعدة اسم الأمر",
};
#define SH_HELP_ROWS (sizeof(sh_help_rows) / sizeof(sh_help_rows[0]))
/* Owner correction 2026-10-14: fold marks only while comparing a command
 * name.  Keep every operand, filename, and pattern byte exactly as typed. */
static size_t sh_name_fold(const char *s, size_t n, char *out, size_t cap) {
    size_t i = 0, j = 0;
    while (i < n) {
        unsigned char a = (unsigned char)s[i], b = i + 1 < n ? (unsigned char)s[i + 1] : 0;
        if (a == 0xd9 && ((b >= 0x8b && b <= 0x92) || b == 0xb0)) {
            i += 2; continue;       /* U+064B..U+0652, U+0670 */
        }
        if (j + 1 >= cap) return 0;
        out[j++] = s[i++];
    }
    if (j >= 2 && (unsigned char)out[j-2] == 0xd9 &&
        (unsigned char)out[j-1] == 0x80) j -= 2; /* trailing U+0640 */
    out[j] = 0;
    return j;
}
static int sh_name_eq_n(const char *a, size_t an, const char *b, size_t bn) {
    char af[1024], bf[1024];
    size_t al = sh_name_fold(a, an, af, sizeof af);
    size_t bl = sh_name_fold(b, bn, bf, sizeof bf);
    return al && al == bl && !memcmp(af, bf, al);
}
static int sh_name_eq(const char *a, const char *b) {
    return sh_name_eq_n(a, strlen(a), b, strlen(b));
}
/* 1 when `q` is exactly one of the row's names: everything before the " - "
 * is the name part, and " / " separates alternatives. */
static int sh_help_name_is(const char *row, const char *q) {
    const char *dash = strstr(row, " - ");
    const char *end = dash ? dash : row + strlen(row);
    const char *p = row;
    size_t ql = strlen(q);
    if (!ql) return 0;
    while (p < end) {
        const char *sep = end, *s;
        size_t seg;
        for (s = p; s + 3 <= end; ++s)
            if (s[0] == ' ' && s[1] == '/' && s[2] == ' ') { sep = s; break; }
        seg = (size_t)(sep - p);
        if (sh_name_eq_n(p, seg, q, ql)) return 1;
        if (sep == end) break;
        p = sep + 3;
    }
    return 0;
}
static int sh_help_lookup(const char *q) {
    size_t i;
    int any = 0;
    for (i = 1; i < SH_HELP_ROWS; ++i)            /* the header is not a name */
        if (sh_help_name_is(sh_help_rows[i], q)) {
            if (sh_emit_row(sh_help_rows[i])) return 1;
            any = 1;
        }
    if (any) return 1;                            /* the entry that was asked for */
    for (i = 1; i < SH_HELP_ROWS; ++i)
        if (strstr(sh_help_rows[i], q)) {
            if (sh_emit_row(sh_help_rows[i])) return 1;
            any = 1;
        }
    if (!any) sh_emit_row("مساعدة: ليس أمراً معروفاً: ");
    return 1;
}

/* ---- WO85 D4: completion on the prompt line ------------------------------
 * Tab completes the LAST word of the line (the caret is at its end):
 *   * the FIRST word is a command name - the session's built-ins plus every
 *     executable in PATH, read from the filesystem with readdir(3) right now,
 *     never a hardcoded list, so a program installed later completes;
 *   * a later word is a PATH - readdir(3) on the directory part of the word as
 *     typed, filtered by the part before its last '/'.  Arabic directory names
 *     complete in Arabic: the word is decoded and replaced through the same
 *     UTF-8 the typed line uses, and the completion is inserted as a logical
 *     word, never as a painted cell.
 * A name that is not one plain word (it contains a space) is inserted QUOTED,
 * in the form WO84's tokenizer reads back verbatim.  A completion replaces the
 * word and NEVER runs, links, expands or aliases anything; several candidates
 * are listed as Arabic rows instead of guessed at.  The editor's save path and
 * viewport are untouched: Tab stays inert while the editor owns the line. */
#define SH_COMP_MAX 512                       /* candidates gathered */
#define SH_COMP_LIST 23                       /* candidates listed per Tab */
struct sh_comp_list {
    char name[SH_COMP_MAX][300];
    unsigned char isdir[SH_COMP_MAX];
    size_t n;
};
static struct sh_comp_list sh_comp;           /* reused: one Tab at a time */
static const char *const sh_builtin_names[] = {
    "اخرج", "المسار", "انتقل", "حرّر", "مساعدة", "شغّل", "متصفح",
    "البيئة",
};
static int sh_comp_row_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}
static void sh_comp_add(struct sh_comp_list *l, const char *name, int isdir) {
    size_t i;
    if (l->n >= SH_COMP_MAX || !name[0]) return;
    if (strlen(name) >= sizeof(l->name[0])) return;
    for (i = 0; i < l->n; ++i)
        if (!strcmp(l->name[i], name)) return;   /* one entry per name */
    snprintf(l->name[l->n], sizeof(l->name[0]), "%s", name);
    l->isdir[l->n] = isdir ? 1 : 0;
    ++l->n;
}
/* Every executable in PATH (readdir + access X_OK; a directory is not a
 * command), plus the session's own built-ins. */
static void sh_comp_commands(struct sh_comp_list *l) {
    const char *p = getenv("PATH");
    size_t i;
    for (i = 0; i < sizeof(sh_builtin_names) / sizeof(sh_builtin_names[0]); ++i)
        sh_comp_add(l, sh_builtin_names[i], 0);
    if (!p || !*p) p = "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin";
    while (*p) {
        const char *s = p;
        char dir[4096];
        size_t len;
        DIR *d;
        struct dirent *e;
        while (*p && *p != ':') ++p;
        len = (size_t)(p - s);
        if (len && len < sizeof dir) {
            memcpy(dir, s, len);
            dir[len] = 0;
            d = opendir(dir);
            if (d) {
                while ((e = readdir(d))) {
                    char cand[4400];
                    int isdir = e->d_type == DT_DIR;
                    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                    if (snprintf(cand, sizeof cand, "%s/%s", dir, e->d_name) >= (int)sizeof cand)
                        continue;
                    if (e->d_type == DT_UNKNOWN) {
                        struct stat st;
                        if (stat(cand, &st) == 0) isdir = S_ISDIR(st.st_mode);
                    }
                    if (isdir) continue;          /* a directory is not a command */
                    if (access(cand, X_OK) == 0) sh_comp_add(l, e->d_name, 0);
                }
                closedir(d);
            }
        }
        if (*p == ':') ++p;
    }
}
/* The word as WO84's tokenizer would read it back: the quoting (Quotes and \c)
 * removed.  The completion runs on this reading, so a second Tab inside a name
 * this completion itself quoted - `اطبع 'دليل تجارب'/مل…` - still finds the
 * directory; the inserted text keeps the type form. */
static size_t sh_comp_dequote(const char *in, char *out, size_t outsz) {
    size_t i = 0, o = 0;
    int q = 0;                                    /* 0 none, 1 ', 2 " */
    while (in[i] && o + 1 < outsz) {
        char c = in[i];
        if (q == 1) {                             /* inside '...': literal */
            if (c == '\'') { q = 0; ++i; continue; }
            out[o++] = c; ++i; continue;
        }
        if (q == 2) {                             /* inside "...": \" and \\ */
            if (c == '"') { q = 0; ++i; continue; }
            if (c == '\\' && (in[i + 1] == '"' || in[i + 1] == '\\')) c = in[++i];
            out[o++] = c; ++i; continue;
        }
        if (c == '\'') { q = 1; ++i; continue; }
        if (c == '"') { q = 2; ++i; continue; }
        if (c == '\\' && in[i + 1]) { out[o++] = in[i + 1]; i += 2; continue; }
        out[o++] = c; ++i;
    }
    out[o] = 0;
    return o;
}
/* Names in the word's directory whose name starts with the word's filter part
 * (everything after its last '/'; "" = the session's own directory), told
 * apart from the directory part, which is kept as typed.  `word` is the
 * DE-QUOTED word; `*dirlen` receives the byte length of the typed directory
 * part (the quotes are in the typed text, so both lengths are needed). */
static void sh_comp_paths(struct sh_comp_list *l, const char *word, int *dirlen) {
    const char *slash = strrchr(word, '/');
    const char *filter;
    char dir[4096];
    size_t dl = 0, fl;
    DIR *d;
    struct dirent *e;
    if (slash) {
        dl = (size_t)(slash - word) + 1;          /* keep the '/' */
        if (dl >= sizeof dir) dl = sizeof dir - 1;
        memcpy(dir, word, dl);
    }
    dir[dl] = 0;
    *dirlen = (int)dl;
    filter = word + dl;
    fl = strlen(filter);
    d = opendir(dl ? dir : ".");
    if (!d) return;
    while ((e = readdir(d))) {
        int isdir = e->d_type == DT_DIR;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (strncmp(e->d_name, filter, fl)) continue;
        if (e->d_type == DT_UNKNOWN) {
            char cand[4400];
            struct stat st;
            if (snprintf(cand, sizeof cand, "%s%s", dl ? dir : "", e->d_name) < (int)sizeof cand
                && stat(cand, &st) == 0)
                isdir = S_ISDIR(st.st_mode);
        }
        sh_comp_add(l, e->d_name, isdir);
    }
    closedir(d);
}
/* The safe quoting WO84's tokenizer reads back verbatim: a name with a space
 * or tab is single-quoted; a name that also carries a quote (or any byte the
 * tokenizer reads specially) keeps the plain form and backslash-escapes each
 * such byte (the tokenizer's \c reads the byte literally, and so does bash on
 * the ordinary path). */
static void sh_comp_quote(const char *name, char *out, size_t outsz) {
    size_t i, o = 0;
    int need = 0, tricky = 0;
    for (i = 0; name[i]; ++i)
        if (name[i] == ' ' || name[i] == '\t') { need = 1; break; }
    if (!need) { snprintf(out, outsz, "%s", name); return; }
    for (i = 0; name[i]; ++i)
        if (strchr("'\"\\|&;<>`$()", name[i])) { tricky = 1; break; }
    if (!tricky) { snprintf(out, outsz, "'%s'", name); return; }
    for (i = 0; name[i] && o + 2 < outsz; ++i) {
        if (name[i] == ' ' || name[i] == '\t' || strchr("'\"\\|&;<>`$()", name[i]))
            out[o++] = '\\';
        out[o++] = name[i];
    }
    out[o] = 0;
}
/* Replace the word [start, sh_caret) with `text` (codepoints via the same
 * UTF-8 the line uses).  The prompt's room is respected; with no room the
 * word is left exactly as it was. */
static void sh_comp_insert(const char *text, size_t start) {
    unsigned int cps[MAX_COLS];
    size_t n = sh_utf8_cp(text, cps, MAX_COLS);
    if (start + n > MAX_COLS - sh_prompt_len) return;
    memmove(&lines[0].cp[start + n], &lines[0].cp[sh_caret],
            (lines[0].len - sh_caret) * sizeof(lines[0].cp[0]));
    memcpy(&lines[0].cp[start], cps, n * sizeof(cps[0]));
    lines[0].len = lines[0].len - (sh_caret - start) + n;
    sh_caret = start + n;
}
static void sh_complete(void) {
    size_t start, i, wl;
    char word[MAX_COLS * 4 + 1], dword[MAX_COLS * 4 + 1], prevb[MAX_COLS * 4 + 1];
    int first = 1, cmdmode = 0, dirlen = 0;
    if (sh_caret != lines[0].len) return;         /* only at the line's end */
    /* The completed argument is found with WO84's own word rule, not by
     * splitting on spaces: a space (or tab) ends the word only at quote level
     * zero and adjacent quoted or escaped parts concatenate.  `اطبع
     * 'دليل تجارب'/مل` is therefore ONE word (the old space split cut it at
     * the quoted name's space, so a second Tab inside such a directory lost
     * the path).  `word` is the argument as TYPED (quotes and \c kept - the
     * insert keeps the user's form); dword is how the tokenizer reads it. */
    {
        size_t j, wstart = 0, pstart = 0, pend = 0;
        int cur = 0, have_prev = 0;
        unsigned char q = 0;
        for (j = 0; j < sh_caret; ++j) {
            unsigned int c = lines[0].cp[j];
            if (q) {
                if (c == q) q = 0;
                else if (q == '"' && c == '\\' && j + 1 < sh_caret) ++j;
                continue;
            }
            if (c == ' ' || c == '\t') {
                if (cur) { pstart = wstart; pend = j; have_prev = 1; cur = 0; }
                continue;
            }
            if (!cur) { cur = 1; wstart = j; }
            if (c == '\'' || c == '"') q = (unsigned char)c;
            else if (c == '\\' && j + 1 < sh_caret) ++j;
        }
        start = cur ? wstart : sh_caret;
        first = have_prev ? 0 : 1;
        {   size_t o = 0;
            for (i = start; i < sh_caret && o + 5 < sizeof word; ++i)
                o += sh_utf8(lines[0].cp[i], word + o);
            word[o] = 0; }
        sh_comp_dequote(word, dword, sizeof dword);
        /* the word before the caret's, for the pager's two-word name */
        {   size_t o = 0;
            for (i = pstart; i < pend && o + 5 < sizeof prevb; ++i)
                o += sh_utf8(lines[0].cp[i], prevb + o);
            prevb[o] = 0; }
    }
    sh_comp.n = 0;
    if (first) {
        if (!word[0]) return;                     /* an empty word lists nothing */
        cmdmode = 1;
        sh_comp_commands(&sh_comp);
    } else if (!strcmp(prevb, "متصفح")) {
        cmdmode = 1; sh_comp_add(&sh_comp, "الصفحات", 0);
    } else {
        if (!word[0]) return;
        sh_comp_paths(&sh_comp, dword, &dirlen);
    }
    if (cmdmode) {                                /* keep the typed prefix */
        wl = strlen(dword);
        for (i = 0; i < sh_comp.n; ) {
            if (wl && strncmp(sh_comp.name[i], dword, wl)) {
                sh_comp.name[i][0] = 0;           /* mark for removal */
                ++i;
            } else ++i;
        }
        {   size_t k = 0;
            for (i = 0; i < sh_comp.n; ++i) {
                if (!sh_comp.name[i][0]) continue;
                if (k != i) {
                    memcpy(sh_comp.name[k], sh_comp.name[i], sizeof(sh_comp.name[0]));
                    sh_comp.isdir[k] = sh_comp.isdir[i];
                }
                ++k;
            }
            sh_comp.n = k;
        }
    }
    qsort(sh_comp.name, sh_comp.n, sizeof(sh_comp.name[0]), sh_comp_row_cmp);
    if (sh_comp.n == 1) {                         /* one candidate: insert it */
        char quoted[360], ins[MAX_COLS * 4 + 400];
        /* Keep the typed directory part VERBATIM - it may carry quotes the
         * user typed, and dequoting never removes '/', so the last '/' of the
         * typed word is the split point (dirlen is measured dequoted). */
        const char *sl = strrchr(word, '/');
        size_t dl = sl ? (size_t)(sl - word) + 1 : 0;
        sh_comp_quote(sh_comp.name[0], quoted, sizeof quoted);
        if (dl + 2 >= sizeof ins) return;
        memcpy(ins, word, dl);
        snprintf(ins + dl, sizeof ins - dl, "%s%s", quoted,
                 sh_comp.isdir[0] ? "/" : " ");
        sh_comp_insert(ins, start);
        (void)sh_paint();
        return;
    }
    if (sh_comp.n > 1) {
        /* Ambiguous: list the candidates as Arabic rows.  sh_emit_row uses
         * lines[0] as scratch, so the typed line and its caret are saved and
         * restored around the rows (the editor's message-row discipline). */
        struct Line keep = lines[0];
        size_t keep_caret = sh_caret;
        size_t listed = sh_comp.n < SH_COMP_LIST ? sh_comp.n : SH_COMP_LIST;
        sh_emit_row("المرشحات:");
        for (i = 0; i < listed; ++i) {
            char row[380], quoted[360];
            sh_comp_quote(sh_comp.name[i], quoted, sizeof quoted);
            snprintf(row, sizeof row, "%s%s", quoted, sh_comp.isdir[i] ? "/" : "");
            if (sh_emit_row(row)) break;
        }
        if (sh_comp.n > listed) {
            char row[64];
            snprintf(row, sizeof row, "المزيد: %zu", sh_comp.n - listed);
            sh_emit_row(row);
        }
        lines[0] = keep;
        sh_caret = keep_caret;
        sh_return_to_live();
    }
}
static int sh_exit_requested;

/* ---- WO84: ONE argument contract (plan step S8) -------------------------
 * The session's own tokenizer, used by every line the session executes
 * itself: the built-ins and شغّل.  The grammar is bounded and explicit:
 *
 *   line  := word (space|tab word)*        (leading/trailing space ignored)
 *   word  := ('...' | "..." | \c | plain)* (adjacent parts concatenate)
 *   '...'   literal, no escapes inside
 *   "..."   literal except \" and \\
 *   \c      the byte c, literal (a trailing \ is literal, as bash reads it)
 *   ''      a real EMPTY argument
 *
 * An unmatched quote refuses the whole line BEFORE anything is forked or
 * executed - no partial execution, no child (D3).
 *
 * WO99 N4a adds the composition grammar on top of those words - still ONE
 * operator table, still validated in full before anything runs:
 *
 *   pipeline := stage ('|' stage)*
 *   stage    := (word | redir)+          (at least one word: an empty stage
 *                                         and a bare operator are refused)
 *   redir    := ('<' | '>' | '>>' | '2>' | '2>>') word
 *
 * The parse builds a bounded representation (SH_STAGE_MAX stages, each with
 * SH_ARGV_MAX words and SH_REDIR_MAX redirections) that the executor runs
 * DIRECTLY: those words are the argv handed to the program, never a re-read
 * of the user's line.  An operator is an operator only when it is unquoted
 * and standalone - quoted or escaped it is an ordinary byte, so a path may
 * still contain any of them, and '' is still a real empty argument.  `2>` is
 * the error stream only at the start of a word; elsewhere the 2 is a byte
 * (`x2>f` redirects the stdout of `x2`).
 *
 * What the plan's CUTS list defers is still refused BY NAME rather than
 * silently mis-read: background &, command lists ;, command substitution `
 * and $ (which also refuses parameter expansion and $( )), grouping ( ),
 * and the here-document / over-append forms << and >>>.  State-changing
 * built-ins are refused inside a composition by the executor, before
 * anything runs.
 *
 * The ordinary (non-built-in) line still reaches bash -c with its own POSIX
 * quoting, exactly as before this order; the fixture measures that both
 * layers agree on the two contract predicates (quotes group; an unmatched
 * quote executes nothing). */
#define SH_ARGV_MAX 32
#define SH_STAGE_MAX 8                  /* stages in one pipeline */
#define SH_REDIR_MAX 4                  /* redirections on one stage */
#define SH_SPLIT_QUOTE   (-2)   /* unmatched quote at end of line */
#define SH_SPLIT_BADCHAR (-3)   /* an unsupported composition operator */
#define SH_SPLIT_COUNT   (-4)   /* more words than the session takes */
#define SH_SPLIT_NEED    (-5)   /* empty stage / operator without its word */
#define SH_SPLIT_MANY    (-6)   /* a composition limit (stages/redirections) */
static unsigned char sh_split_bad;      /* the refused operator, for the row */
static int sh_split_limit;              /* the limit that was hit, for the row */

enum { SH_RIN = 1, SH_ROUT, SH_RAPP, SH_RERR, SH_RERRAPP };
struct ShRedir { int kind; const char *path; };
struct ShStage { int argc; const char *argv[SH_ARGV_MAX]; int nredir;
                 struct ShRedir redir[SH_REDIR_MAX]; };
struct ShPipe { int nstage; struct ShStage stage[SH_STAGE_MAX]; };

/* Resolve only a known command name.  A two-word installed program may be
 * typed as a phrase or quoted as one word.  Operands are never folded. */
static int sh_resolve_stage_name(struct ShStage *sg) {
    static const char *const names[] = {
        "اعرض الأذونات", "اضبط الأذونات", "أعد التسمية",
        "حدّث الحزم", "ثبّت حزمة", "أزل حزمة",
        "اخرج", "المسار", "انتقل", "حرّر", "مساعدة", "شغّل",
        "متصفح", "البيئة", "اضبط", "اعرض", "اطبع", "أنشئ",
        "انسخ", "انقل", "احذف", "ابحث", "فتّش", "شكل", "تاريخ"
    };
    size_t i;
    if (!sg->argc) return 0;
    for (i = 0; i < sizeof names / sizeof names[0]; ++i) {
        const char *name = names[i], *space = strchr(name, ' ');
        int joined = 0, j;
        if (sh_name_eq(sg->argv[0], name)) {
            if (!strcmp(sg->argv[0], name)) return 0;
        } else if (space && sg->argc >= 2 &&
                   sh_name_eq_n(sg->argv[0], strlen(sg->argv[0]),
                                name, (size_t)(space - name)) &&
                   sh_name_eq(sg->argv[1], space + 1)) {
            joined = 1;
        } else continue;
        sg->argv[0] = name;
        if (joined) {
            for (j = 1; j < sg->argc; ++j) sg->argv[j] = sg->argv[j + 1];
            --sg->argc;
        }
        return 1;
    }
    return 0;
}

/* Only a resolved external command needs a rewritten shell line.  Quote each
 * parsed word so a marked path or pattern reaches the child byte for byte. */
static int sh_stage_shell_line(const struct ShStage *sg, char *out, size_t cap) {
    size_t n = 0;
    int i;
    for (i = 0; i < sg->argc; ++i) {
        const unsigned char *p = (const unsigned char *)sg->argv[i];
        if (n + 3 >= cap) return -1;
        if (i) out[n++] = ' ';
        out[n++] = '\'';
        while (*p) {
            if (*p == '\'') {
                if (n + 4 >= cap) return -1;
                memcpy(out + n, "'\\''", 4); n += 4;
            } else {
                if (n + 2 >= cap) return -1;
                out[n++] = (char)*p;
            }
            ++p;
        }
        out[n++] = '\'';
    }
    out[n] = 0;
    return 0;
}

/* One finished word: the pending redirection's target when one is waiting,
 * else the stage's next operand.  The word's bytes end where the NUL is
 * written and `inword` is cleared. */
static int sh_word_close(char *buf, size_t *n, size_t bufsz, int *inword,
                         size_t wstart, struct ShPipe *p, int st, int *redir) {
    struct ShStage *sg = &p->stage[st];
    if (!*inword) return 0;
    if (*n + 1 >= bufsz) return SH_SPLIT_COUNT;
    buf[(*n)++] = 0;
    if (*redir) {
        if (sg->nredir >= SH_REDIR_MAX) { sh_split_limit = SH_REDIR_MAX; return SH_SPLIT_MANY; }
        sg->redir[sg->nredir].kind = *redir;
        sg->redir[sg->nredir].path = buf + wstart;
        ++sg->nredir;
        *redir = 0;
    } else {
        if (sg->argc + 1 >= SH_ARGV_MAX) return SH_SPLIT_COUNT;
        sg->argv[sg->argc++] = buf + wstart;
        sg->argv[sg->argc] = NULL;
    }
    *inword = 0;
    return 0;
}

/* The ONE line parser (WO84's word rules, WO99's composition grammar).  It
 * runs to completion - or refuses - BEFORE the caller opens a redirection
 * target, forks a stage or executes anything, so a malformed or unsupported
 * line cannot partially run.  Returns 0, or one of the SH_SPLIT_* codes. */
static int sh_parse(const char *line, char *buf, size_t bufsz, struct ShPipe *p) {
    size_t i = 0, n = 0, wstart = 0;
    unsigned char quote = 0;
    int inword = 0, redir = 0, st = 0, rc;

    memset(p, 0, sizeof *p);
    while (line[i]) {
        unsigned char c = (unsigned char)line[i];
        int word = 0;
        if (quote == '\'') {                 /* inside '...' */
            if (c == '\'') { quote = 0; ++i; continue; }
            ++i; word = 1;
        } else if (quote == '"') {           /* inside "..." */
            if (c == '"') { quote = 0; ++i; continue; }
            if (c == '\\' && (line[i+1] == '"' || line[i+1] == '\\'))
                c = (unsigned char)line[++i];
            ++i; word = 1;
        } else if (c == ' ' || c == '\t') {
            ++i;
            if ((rc = sh_word_close(buf, &n, bufsz, &inword, wstart, p, st, &redir))) return rc;
            continue;
        } else if (c == '\'' || c == '"') {
            if (!inword) { if (n + 1 >= bufsz) return SH_SPLIT_COUNT;
                           inword = 1; wstart = n; }
            quote = c; ++i; continue;
        } else if (c == '\\') {
            if (!inword) { if (n + 1 >= bufsz) return SH_SPLIT_COUNT;
                           inword = 1; wstart = n; }
            if (line[i+1]) { c = (unsigned char)line[++i]; }
            ++i; word = 1;                   /* trailing \ is a literal \ */
        } else if (c == '|') {               /* pipeline separator */
            ++i;
            if ((rc = sh_word_close(buf, &n, bufsz, &inword, wstart, p, st, &redir))) return rc;
            if (redir) return SH_SPLIT_NEED;
            if (p->stage[st].argc == 0) return SH_SPLIT_NEED;
            if (st + 1 >= SH_STAGE_MAX) { sh_split_limit = SH_STAGE_MAX; return SH_SPLIT_MANY; }
            ++st;
            continue;
        } else if (c == '<' || c == '>') {   /* redirection operator */
            int kind;
            if (c == '<') {
                if (line[i+1] == '<') { sh_split_bad = '<'; return SH_SPLIT_BADCHAR; }
                ++i; kind = SH_RIN;
            } else {
                ++i; kind = SH_ROUT;
                if (line[i] == '>') {
                    ++i; kind = SH_RAPP;
                    if (line[i] == '>') { sh_split_bad = '>'; return SH_SPLIT_BADCHAR; }
                }
            }
            if ((rc = sh_word_close(buf, &n, bufsz, &inword, wstart, p, st, &redir))) return rc;
            if (redir) return SH_SPLIT_NEED;         /* two operators, no word */
            redir = kind;
            continue;
        } else if (c == '2' && !inword && line[i+1] == '>') {   /* 2> | 2>> */
            int kind = SH_RERR;
            i += 2;
            if (line[i] == '>') {
                ++i; kind = SH_RERRAPP;
                if (line[i] == '>') { sh_split_bad = '>'; return SH_SPLIT_BADCHAR; }
            }
            if (redir) return SH_SPLIT_NEED;
            redir = kind;
            continue;
        } else if (strchr("&;`$()", (int)c)) {
            sh_split_bad = c; return SH_SPLIT_BADCHAR;
        } else { ++i; word = 1; }
        if (word) {
            if (!inword) { if (n + 1 >= bufsz) return SH_SPLIT_COUNT;
                           inword = 1; wstart = n; }
            if (n + 1 >= bufsz) return SH_SPLIT_COUNT;
            buf[n++] = (char)c;
        }
    }
    if (quote) return SH_SPLIT_QUOTE;
    if ((rc = sh_word_close(buf, &n, bufsz, &inword, wstart, p, st, &redir))) return rc;
    if (redir) return SH_SPLIT_NEED;         /* a redirection with no target */
    if (p->stage[st].argc == 0) return SH_SPLIT_NEED;   /* an empty stage */
    p->nstage = st + 1;
    return 0;
}

/* Every refusal is one Arabic row in the command's own voice, and nothing
 * runs.  The two new fragments (this order's extension of TASK-67 §4.3's
 * table: `علامة تنصيص غير مغلقة` and `غير مدعوم`) are the only way the
 * session can FAIL CLEARLY on what the bounded grammar does not take;
 * `عدد المعاملات غير صحيح` is the shipped wording of tools/mujammi.c:243. */
static void sh_arg_refuse(const char *cmd, int rc) {
    char row[128];
    if (rc == SH_SPLIT_QUOTE)
        snprintf(row, sizeof row, "%s: علامة تنصيص غير مغلقة", cmd);
    else if (rc == SH_SPLIT_BADCHAR)
        snprintf(row, sizeof row, "%s: غير مدعوم: '%c'", cmd, sh_split_bad);
    else if (rc == SH_SPLIT_NEED)
        snprintf(row, sizeof row, "%s: تركيب غير مكتمل: معامل مفقود", cmd);
    else if (rc == SH_SPLIT_MANY)
        snprintf(row, sizeof row, "%s: تركيب طويل جداً: الحد %d", cmd, sh_split_limit);
    else
        snprintf(row, sizeof row, "%s: عدد المعاملات غير صحيح", cmd);
    sh_emit_row(row);
}

/* WO99 N4b: a built-in that is interactive or changes the session's own state
 * (اخرج closes the session, انتقل moves it, حرّر takes over the screen,
 * شغّل and متصفح take over the console) cannot be one stage of a composition:
 * refused before anything runs. */
static void sh_arg_refuse_composition(const char *cmd) {
    char row[128];
    snprintf(row, sizeof row, "%s: غير مدعوم داخل التركيب", cmd);
    sh_emit_row(row);
}

/* ---- WO85 D2: متصفح الصفحات <ملف|برنامج> [معاملات…] ---------------------
 * The pager's command.  A file the filesystem has - anything stat(2) finds,
 * or anything with a '/' in it - is paged as a file; anything else is looked
 * up in PATH and run, and its logical output is paged.  The caps and the
 * refusals live with the pager's loader (SH_PG_*); the operand count uses the
 * tokenizer's own refusal, so both layers fail the same way. */
static int sh_pg_open(int argc, char **argv) {
    char *buf = NULL;
    size_t n = 0;
    struct stat st;
    int as_file;
    if (argc < 1 || !argv[0] || !argv[0][0]) {
        sh_emit_row("متصفح الصفحات: الاستخدام: متصفح الصفحات ملف");
        return 1;
    }
    as_file = strchr(argv[0], '/') != NULL || stat(argv[0], &st) == 0;
    if (as_file) {
        if (argc > 1) { sh_arg_refuse("متصفح الصفحات", SH_SPLIT_COUNT); return 1; }
        if (sh_pg_read_file(argv[0], &buf, &n)) return 1;
    } else {
        if (sh_pg_run_capture(argc, argv, &buf, &n)) return 1;
    }
    if (sh_pg_load(buf, n, argv[0])) { free(buf); return 1; }
    free(buf);
    sh_pg_line = sh_pg_chunk = 0;
    sh_pg_query_len = 0; sh_pg_searching = 0; sh_pg_note_len = 0;
    lines[0].len = 0; sh_caret = 0;               /* the command row is done */
    sh_pg_active = 1;
    sh_prev_rows = 0;
    return 1;                                     /* the caller repaints */
}

/* ---- N4d: ONE child lifecycle for every route ---------------------------
 * An ordinary line, شغّل and every stage of a composition now start their child
 * the same way and end it the same way.  What "the same way" means, exactly:
 *
 *  * the child is its own process-group leader, so ONE signal reaches it and
 *    the descendants that stayed in its group;
 *  * a child whose stdin the user has NOT redirected reads the CONSOLE.  The
 *    parent owns one pipe into it and, while the child lives, moves the
 *    console's bytes there (sh_console_bytes).  The child's own reader
 *    delimits the lines; when the child is hosted by the شكل wrapper the
 *    bytes reach a real pty - the wrapper forwards them - so an interactive
 *    program keeps its terminal, and when a stage is exec'd directly they
 *    arrive as the plain bytes a pipe carries.
 *  * ^C (0x03) is SIGINT to the child's whole process group; ^D (0x04) ENDS
 *    the child's input by closing the pipe - the one EOF a pipe can carry;
 *    every other byte is the child's stdin, verbatim, in the order typed.
 *    A write the reader no longer wants (EPIPE - the child closed its own
 *    input first) is never an error: SIGPIPE is ignored (see main), so the
 *    byte is dropped and input stays finished for that child.  A child that
 *    stops reading is ordinary terminal backpressure, not a deadlock: this
 *    loop only ever writes, and the child's output never comes back through
 *    it;
 *  * the exit status is reported by ONE row, in Arabic, as the REAL value:
 *    the code, or 128+signal when a signal ended the child.
 *
 * The composition executor keeps its own multi-child reap loop (it must wait
 * for many), but shares these three pieces, so the routes cannot drift. */

/* N4d: typed bytes the child has not read yet.  The feed end is non-blocking,
 * so a child that stops reading backs up here instead of freezing this loop -
 * a frozen loop could not even deliver the ^C that would end the wait.  The
 * buffer is bounded; past it the OLDEST byte goes and the log says so (losing
 * a keystroke is bad, a console that cannot be interrupted is worse). */
static unsigned char sh_feed_pend[4096];
static size_t sh_feed_n;

/* Only the feed END changes: a pipe's two ends carry their own flags, so the
 * child's read end stays blocking, exactly like a terminal. */
static void sh_feed_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void sh_feed_flush(int *feed) {
    size_t done = 0;
    if (!feed || *feed < 0) { sh_feed_n = 0; return; }
    while (done < sh_feed_n) {
        ssize_t w = write(*feed, sh_feed_pend + done, sh_feed_n - done);
        if (w > 0) { done += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        close(*feed); *feed = -1; sh_feed_n = 0; return;   /* the reader is gone */
    }
    if (done) {
        memmove(sh_feed_pend, sh_feed_pend + done, sh_feed_n - done);
        sh_feed_n -= done;
    }
}

static void sh_feed_byte(int *feed, unsigned char c) {
    if (!feed || *feed < 0) return;
    if (sh_feed_n == 0) {                    /* the common case: straight through */
        ssize_t w = write(*feed, &c, 1);
        if (w == 1) return;
        if (w < 0 && !(errno == EINTR || errno == EAGAIN ||
                       errno == EWOULDBLOCK)) {
            close(*feed); *feed = -1; return;  /* the reader is gone */
        }
    }
    if (sh_feed_n == sizeof sh_feed_pend) {
        memmove(sh_feed_pend, sh_feed_pend + 1, sh_feed_n - 1);
        --sh_feed_n;
        sh_log("console input over %u bytes: oldest byte dropped",
               (unsigned)sizeof sh_feed_pend);
    }
    sh_feed_pend[sh_feed_n++] = c;
}

/* The console's bytes, while a child owns them. */
static void sh_console_bytes(const unsigned char *in, ssize_t n, pid_t pgid,
                             int *feed) {
    ssize_t k;
    for (k = 0; k < n; ++k) {
        if (in[k] == 0x03) {
            if (pgid > 0) kill(-pgid, SIGINT);
            continue;
        }
        if (in[k] == 0x04) {                 /* EOF: close the one write end */
            if (feed && *feed >= 0) { close(*feed); *feed = -1; }
            sh_feed_n = 0;
            continue;
        }
        sh_feed_byte(feed, in[k]);
    }
}

/* Wait for ONE child - this session's own fork - while the console feeds it. */
static void sh_child_wait(pid_t pid, pid_t pgid, int *feed, int *status) {
    while (1) {
        struct pollfd pf;
        int pr;
        if (feed && *feed >= 0) sh_feed_flush(feed);   /* retry a backed-up child */
        pf.fd = tty_fd; pf.events = POLLIN; pf.revents = 0;
        pr = poll(&pf, 1, 50);
        if (pr > 0 && (pf.revents & POLLIN)) {
            unsigned char in[256];
            ssize_t r = read(tty_fd, in, sizeof in);
            if (r > 0) sh_console_bytes(in, r, pgid, feed);
        }
        {
            pid_t w = waitpid(pid, status, WNOHANG);
            if (w == pid) break;
            if (w < 0 && errno != EINTR) break;
        }
    }
}

/* One finished child's real status, in Arabic - the same wording for every
 * route, so a program cannot report one code through شغّل and another through
 * an ordinary line. */
static void sh_status_row(int status) {
    char row[128];
    snprintf(row, sizeof row, "حالة الخروج: %d",
             WIFEXITED(status) ? WEXITSTATUS(status)
                               : (WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                                      : status));
    sh_emit_row(row);
    sh_log("child status=%d exited=%d sig=%d", status,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : -1);
}

/* ---- WO84: شغّل runs its whole argument vector through the SAME execution
 * path as an ordinary line - the شكل wrapper - passing the vector itself,
 * never a re-quoted string: {wrapper, bash, "-c", "$@", "bash", operands…}.
 * bash's "$@" re-expands to the original words verbatim (no re-splitting, no
 * globbing, no re-quoting), so every operand arrives exactly and in order;
 * bash's $0 is "bash" so the missing-program diagnostic keeps the exact
 * `bash: line N: …` shape tools/arsh.c's Route B already translates (the
 * operand's own argv[0] is the program word, as in any exec).
 *
 * The child's stdin is a PIPE; while the run lasts the parent copies the
 * console's bytes into it, and the wrapper forwards them into the child's
 * pty, whose line discipline turns them into lines (canonical, echo on).
 * Ctrl+D sends the pty's EOF byte.  Ctrl+C keeps its one meaning - SIGINT to
 * the wrapper's process group - and the wrapper hands it to the pty, so the
 * interrupt reaches the PROGRAM's own process group, not the wrapper.
 *
 * The exit-status row reports the real value: the program's code, or
 * 128+signal when it was killed (not a glyph, not a boolean). */
static void sh_run_program(int argc, char **argv) {
    char *wargv[SH_ARGV_MAX + 5];
    int pfd[2], status = 0, feed, i;
    pid_t pid;

    wargv[0] = sh_wrapper; wargv[1] = sh_shell;
    wargv[2] = (char *)"-c"; wargv[3] = (char *)"\"$@\"";
    wargv[4] = (char *)"bash";
    for (i = 1; i < argc; ++i) wargv[4 + i] = argv[i];
    wargv[4 + argc] = NULL;
    if (pipe(pfd) != 0) { sh_emit_row("شغّل: تعذر تشغيل البرنامج"); return; }
    /* N4d: only the parent keeps the feed end past an exec; a child of this
     * session must never hold it open, or the reader would wait for an EOF
     * that only this loop can deliver. */
    fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    sh_feed_nonblock(pfd[1]);
    pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        sh_emit_row("شغّل: تعذر تشغيل البرنامج"); return;
    }
    if (pid == 0) {
        close(pfd[1]);
        if (dup2(pfd[0], 0) < 0) _exit(127);
        if (pfd[0] != 0) close(pfd[0]);
        setpgid(0, 0);
        setenv("ARSH_SHELL", "1", 1);
        execv(sh_wrapper, wargv);
        sh_log("execv %s failed: %s", sh_wrapper, strerror(errno));
        _exit(127);
    }
    close(pfd[0]);
    /* Both sides name the group (the usual fork/setpgid race): the child does
     * it for itself, the parent here, so a ^C that arrives before the child
     * has run a single instruction still reaches -pid. */
    setpgid(pid, pid);
    feed = pfd[1];
    sh_child_wait(pid, pid, &feed, &status);       /* N4d: the one console loop */
    if (feed >= 0) close(feed);
    sh_status_row(status);
}

/* ---- WO99 N4b: the bounded composition executor --------------------------
 * A parsed pipeline becomes real processes.  Every stage gets its three
 * descriptors explicitly - its own pipe ends, its redirection targets, the
 * console, /dev/null - and the PARSED argv is handed to the program: the
 * user's line is never re-read, no word is ever re-split, re-quoted or
 * expanded.  Descriptor ownership, stated exactly:
 *
 *  - A stage whose stdout or stderr is terminal-bound is HOSTED by the
 *    installed شكل wrapper, so exactly that stream is shaped exactly once.
 *    The wrapper's ARSH_* switches (tools/arsh.c) keep the stage's other
 *    descriptors exactly as this executor set them, and the wrapper's own
 *    shaped bytes go to the console, never into a pipeline.  Every other
 *    stage is exec'd DIRECTLY, so what crosses a pipe or lands in a
 *    redirection target is the producer's LOGICAL bytes: no presentation
 *    forms, no escape sequences, no right-anchor padding.
 *  - Redirections are opened by the parent BEFORE the first child exists -
 *    and only after the whole line has parsed and validated, so a refusal, a
 *    missing stage or an unopenable target runs nothing.
 *  - The read-only built-ins (المسار، مساعدة) have a LOGICAL output path:
 *    the same text their session rows carry, written raw to a pipe or a
 *    file, and painted as rows only when the console is their target.
 *  - Unused pipe ends are closed as soon as the children exist and every
 *    child is reaped; the pipeline's status is the LAST stage's, reported as
 *    حالة الخروج, and a stage that cannot be launched is reported by name -
 *    the child sends its index and errno down a CLOEXEC failure pipe, which a
 *    successful exec closes silently.
 *
 * This path is taken only when the line really composes (a pipe, or any
 * redirection): the ordinary single-stage line keeps its shipped path byte
 * for byte. */

/* 1 = a read-only built-in that composes (it has a logical output path);
 * 2 = a built-in that is interactive or changes the session's own state and
 * cannot be one stage of a composition; 0 = not a built-in. */
static int sh_builtin_class(const char *cmd) {
    if (!cmd) return 0;
    if (sh_name_eq(cmd, "المسار") || sh_name_eq(cmd, "مساعدة")) return 1;
    if (sh_name_eq(cmd, "اخرج") || sh_name_eq(cmd, "انتقل") || sh_name_eq(cmd, "حرّر") ||
        sh_name_eq(cmd, "شغّل") || sh_name_eq(cmd, "متصفح")) return 2;
    /* N4e: البيئة owns the parent session's own state (and its children's
     * environment), so it is not a composition stage either.  Its setter is
     * the TWO-word `اضبط البيئة`, which this one-word table cannot see: that
     * refusal is drawn in sh_pipe_validate, where the whole stage is in hand,
     * so a shipped program named اضبط ... keeps its own meaning. */
    if (sh_name_eq(cmd, "البيئة")) return 2;
    return 0;
}

/* WO99 N4a: the parsed representation of the committed line, one line to the
 * session's own log - stages, each stage's words, each redirection with its
 * kind and target.  The parse can therefore be read back and asserted on its
 * own, independently of what ran; fields are separated by U+001F, as the
 * fixtures' own records are. */
static void sh_pipe_trace(const struct ShPipe *p, const char *line) {
    static const char *const kind_name[] = { "", "IN", "OUT", "APP", "ERR", "ERRAPP" };
    char buf[16384];
    size_t used = 0;
    int i, j, k, n;

#define TR(...) do { \
        n = snprintf(buf + used, sizeof buf - used, __VA_ARGS__); \
        if (n < 0 || (size_t)n >= sizeof buf - used) return; \
        used += (size_t)n; \
    } while (0)
    TR("[WO99] parse n=%d line=%s", p->nstage, line);
    for (i = 0; i < p->nstage; ++i) {
        const struct ShStage *sg = &p->stage[i];
        for (j = 0; j < sg->argc; ++j) TR("\037w%d.%d=%s", i, j, sg->argv[j]);
        for (k = 0; k < sg->nredir; ++k)
            TR("\037r%d.%d=%s:%s", i, k, kind_name[sg->redir[k].kind],
               sg->redir[k].path);
    }
#undef TR
    sh_log("%s", buf);
}

/* Append one logical line (text plus its newline) to a growing output. */
static int sh_text_add(char *buf, size_t cap, size_t *used, const char *s) {
    size_t n = strlen(s);
    if (*used + n + 1 >= cap) return -1;
    memcpy(buf + *used, s, n);
    *used += n;
    buf[(*used)++] = '\n';
    buf[*used] = 0;
    return 0;
}

/* The read-only built-ins' LOGICAL output: المسار's working directory,
 * مساعدة's table rows or lookup answers - the same text the session rows
 * carry, as UTF-8 lines with no shaping, no direction and no pad.  Returns 1
 * when argv[0] is such a built-in (*out is the text, *outn its length), 0
 * when it is not one, -1 when the built-in itself was used wrongly (its own
 * Arabic row has been emitted; nothing runs). */
static int sh_builtin_logical(int argc, const char *const *argv, char **out, size_t *outn) {
    const char *cmd = argv[0];
    size_t cap = 16384, used = 0, i;
    char *buf;
    if (sh_builtin_class(cmd) != 1) return 0;
    *out = NULL; *outn = 0;
    if (sh_name_eq(cmd, "المسار")) {
        char p[4096];
        size_t n;
        if (argc != 1) { sh_arg_refuse(cmd, SH_SPLIT_COUNT); return -1; }
        if (!getcwd(p, sizeof p)) { sh_emit_row("المسار: لا يوجد ملف أو دليل بهذا الاسم"); return -1; }
        n = strlen(p);
        buf = malloc(n + 2);
        if (!buf) { sh_emit_row("المسار: لا توجد ذاكرة"); return -1; }
        memcpy(buf, p, n); buf[n] = '\n'; buf[n + 1] = 0;
        *out = buf; *outn = n + 1;
        return 1;
    }
    /* مساعدة: the table, the pinned اعرض usage row, or the entries whose
     * name (then whose text) matches - sh_help_lookup's own text, in its own
     * order. */
    if (argc > 2) { sh_arg_refuse(cmd, SH_SPLIT_COUNT); return -1; }
    buf = malloc(cap);
    if (!buf) { sh_emit_row("مساعدة: لا توجد ذاكرة"); return -1; }
    buf[0] = 0;
    if (argc == 1) {
        for (i = 0; i < SH_HELP_ROWS; ++i)
            if (sh_text_add(buf, cap, &used, sh_help_rows[i])) goto oom;
    } else if (sh_name_eq(argv[1], "اعرض")) {
        if (sh_text_add(buf, cap, &used, "اعرض: الاستخدام: اعرض مسار")) goto oom;
    } else {
        int any = 0;
        for (i = 1; i < SH_HELP_ROWS; ++i)
            if (sh_help_name_is(sh_help_rows[i], argv[1])) {
                any = 1;
                if (sh_text_add(buf, cap, &used, sh_help_rows[i])) goto oom;
            }
        if (!any)
            for (i = 1; i < SH_HELP_ROWS; ++i)
                if (strstr(sh_help_rows[i], argv[1])) {
                    any = 1;
                    if (sh_text_add(buf, cap, &used, sh_help_rows[i])) goto oom;
                }
        if (!any && sh_text_add(buf, cap, &used, "مساعدة: ليس أمراً معروفاً: ")) goto oom;
    }
    *out = buf; *outn = used;
    return 1;
oom:
    free(buf);
    sh_emit_row("مساعدة: لا توجد ذاكرة");
    return -1;
}

/* Open one redirection target, in the parent, before any child exists: a
 * target that cannot be opened refuses the whole composition and nothing
 * runs. */
static int sh_redir_open(const struct ShRedir *r) {
    int fd;
    switch (r->kind) {
    case SH_RIN:  fd = open(r->path, O_RDONLY); break;
    case SH_ROUT: case SH_RERR:
        fd = open(r->path, O_WRONLY | O_CREAT | O_TRUNC, 0644); break;
    default:      fd = open(r->path, O_WRONLY | O_CREAT | O_APPEND, 0644); break;
    }
    if (fd < 0) {
        char row[4600];
        snprintf(row, sizeof row, "إعادة التوجيه: تعذر فتح '%s': %s",
                 r->path, sh_err_ar(errno));
        sh_emit_row(row);
    }
    return fd;
}

/* The whole composition's own semantics, still before a single side effect:
 * every stage's program word is resolved, the read-only built-ins produce
 * their text, and a built-in that cannot compose is refused by name.  Returns
 * 1 when the command is refused with its row emitted (nothing runs). */
static int sh_pipe_validate(const struct ShPipe *p, char paths[][4096],
                            char *logical[SH_STAGE_MAX], size_t logn[SH_STAGE_MAX]) {
    int i;
    for (i = 0; i < p->nstage; ++i) {
        const char *cmd = p->stage[i].argv[0];
        int cls = sh_builtin_class(cmd);
        if (cls == 2) { sh_arg_refuse_composition(cmd); return 1; }
        /* N4e: the session's environment setter is the two words اضبط البيئة,
         * and the one-word class table above can only see اضبط - which is also
         * how a shipped program whose file name carries a space starts.  The
         * refusal is therefore drawn HERE, on the whole stage, by its own two
         * words: any other second word is not this built-in and keeps its
         * meaning. */
        if (cls == 0 && p->stage[i].argc >= 2 && sh_name_eq(cmd, "اضبط") &&
            sh_name_eq(p->stage[i].argv[1], "البيئة")) {
            sh_arg_refuse_composition(cmd);
            return 1;
        }
        if (cls == 1) {
            if (sh_builtin_logical(p->stage[i].argc, p->stage[i].argv,
                                   &logical[i], &logn[i]) < 0) return 1;
            continue;
        }
        if (!sh_program_path(cmd, paths[i], sizeof paths[i])) {
            char row[4600];
            snprintf(row, sizeof row, "%s: الأمر غير موجود", cmd);
            sh_emit_row(row);
            return 1;
        }
    }
    return 0;
}

/* Run one validated, fully parsed pipeline.  Returns 0 when it ran (every
 * child reaped, its rows emitted), 1 when it was refused and nothing ran. */
static int sh_pipe_execute(const struct ShPipe *p) {
    char paths[SH_STAGE_MAX][4096], row[4600];
    char *logical[SH_STAGE_MAX];
    size_t logn[SH_STAGE_MAX];
    int infd[SH_STAGE_MAX], outfd[SH_STAGE_MAX], errfd[SH_STAGE_MAX];
    int pipes[SH_STAGE_MAX][2], fails[2] = { -1, -1 };
    int cfeed = -1, console_in = -1;    /* N4d: the console, into stage 0 */
    int allfds[SH_STAGE_MAX * 4 + 4];
    pid_t pids[SH_STAGE_MAX], pgid = 0;
    int status[SH_STAGE_MAX];
    unsigned char fbuf[2 * SH_STAGE_MAX];       /* launch failures, as sent */
    int nfb = 0, feed_to_pipe = 0;
    int i, k, nall = 0, npid = 0, last = -1, rc = 0;

    for (i = 0; i < p->nstage; ++i) {
        infd[i] = outfd[i] = errfd[i] = -1;
        pids[i] = -1; status[i] = 0;
        logical[i] = NULL; logn[i] = 0; paths[i][0] = 0;
        pipes[i][0] = pipes[i][1] = -1;
    }
    if (sh_pipe_validate(p, paths, logical, logn)) { rc = 1; goto out; }

    /* every redirection target, opened here and nowhere else; the last one of
     * a kind wins, which is the shell rule */
    for (i = 0; i < p->nstage; ++i) {
        int j;
        for (j = 0; j < p->stage[i].nredir; ++j) {
            const struct ShRedir *r = &p->stage[i].redir[j];
            int fd = sh_redir_open(r);
            if (fd < 0) { rc = 1; goto out; }
            allfds[nall++] = fd;
            if (r->kind == SH_RIN) infd[i] = fd;
            else if (r->kind == SH_ROUT || r->kind == SH_RAPP) outfd[i] = fd;
            else errfd[i] = fd;
        }
    }
    /* the pipes, one between each pair of neighbours */
    for (i = 0; i + 1 < p->nstage; ++i) {
        if (pipe(pipes[i]) != 0) {
            snprintf(row, sizeof row, "الأنبوب: تعذر الإنشاء: %s", sh_err_ar(errno));
            sh_emit_row(row);
            rc = 1; goto out;
        }
        allfds[nall++] = pipes[i][0];
        allfds[nall++] = pipes[i][1];
        if (outfd[i] < 0) outfd[i] = pipes[i][1];
        if (infd[i + 1] < 0) infd[i + 1] = pipes[i][0];
    }
    /* N4d: does the feed's reader (stage 0) write into its neighbour's pipe?
     * If it has a '>' of its own, its output goes to that file and stays
     * useful even when every later stage has exited; if it does not, its
     * write end has no reader left the moment the last neighbour dies. */
    feed_to_pipe = (p->nstage > 1 && outfd[0] == pipes[0][1]);
    /* N4d: the FIRST UNREDIRECTED STAGE reads the console.  Stage 0 is the
     * only stage that can be unredirected - every later stage's stdin is its
     * neighbour's pipe - so when stage 0 carries no '<' of its own, the parent
     * owns one pipe into it and moves the console's bytes there while the
     * pipeline runs (sh_console_bytes), exactly as the ordinary and شغّل
     * routes do.  A '<' on stage 0 is the user's own answer: then there is no
     * console input and the bytes are only looked at for ^C.  The parent's
     * write end is CLOEXEC, so no stage - and no descendant of one - inherits
     * it and holds the reader's EOF open. */
    if (infd[0] < 0 && p->stage[0].argc &&
        sh_builtin_class(p->stage[0].argv[0]) != 1) {
        int cp[2];
        if (pipe(cp) != 0) {
            snprintf(row, sizeof row, "الأنبوب: تعذر الإنشاء: %s", sh_err_ar(errno));
            sh_emit_row(row);
            rc = 1; goto out;
        }
        fcntl(cp[1], F_SETFD, FD_CLOEXEC);
        sh_feed_nonblock(cp[1]);
        allfds[nall++] = cp[0];
        infd[0] = cp[0];
        console_in = cp[0];
        cfeed = cp[1];
    }

    /* the failure pipe: a stage writes (index, errno) here when it cannot be
     * launched; a successful exec closes it (FD_CLOEXEC), so silence means
     * every stage started */
    if (pipe(fails) != 0) {
        snprintf(row, sizeof row, "الأنبوب: تعذر الإنشاء: %s", sh_err_ar(errno));
        sh_emit_row(row);
        rc = 1; goto out;
    }

    /* the live line is done: freeze it, move to a fresh row, hide the arrow -
     * all before the first child can paint */
    lines[0].len = 0; sh_caret = 0;
    if (bytes("\r\n")) { rc = -1; goto out; }
    if (console_row == (long)terminal_rows - 1) sh_scrolled(); else console_row++;
    if (flush_out()) { rc = -1; goto out; }
    pointer_visible = 0;

    for (i = 0; i < p->nstage; ++i) {
        const struct ShStage *sg = &p->stage[i];
        int cls = sh_builtin_class(sg->argv[0]);
        int hosted = (outfd[i] < 0 || errfd[i] < 0);
        pid_t pid;
        if (cls == 1) continue;                    /* the parent writes its text */
        pid = fork();
        if (pid < 0) {
            snprintf(row, sizeof row, "%s: تعذر تشغيل البرنامج", sg->argv[0]);
            sh_emit_row(row);
            if (pgid > 0) kill(-pgid, SIGTERM);    /* the stages that did start */
            break;
        }
        if (pid == 0) {
            int cfd = -1;
            /* the console, duplicated BEFORE any dup2 can clobber it: the
             * wrapper's own shaped bytes still go there when the stage's
             * stdout is redirected */
            if (hosted && outfd[i] >= 0) {
                cfd = fcntl(tty_fd, F_DUPFD, 3);
                if (cfd < 0) goto fail;
            }
            if (infd[i] >= 0) {
                if (dup2(infd[i], 0) < 0) goto fail;
            } else {
                int nfd = open("/dev/null", O_RDONLY);
                if (nfd < 0) goto fail;
                if (dup2(nfd, 0) < 0) goto fail;
                if (nfd != 0) close(nfd);
            }
            if (outfd[i] >= 0 && dup2(outfd[i], 1) < 0) goto fail;
            if (errfd[i] >= 0 && dup2(errfd[i], 2) < 0) goto fail;
            for (k = 0; k < nall; ++k) if (allfds[k] > 2) close(allfds[k]);
            setpgid(0, pgid);                      /* the first stage leads */
            fcntl(fails[1], F_SETFD, FD_CLOEXEC);
            if (hosted) {
                char *wargv[SH_ARGV_MAX + 6], fdbuf[16];
                int m = 0;
                wargv[m++] = sh_wrapper;
                wargv[m++] = sh_shell;
                wargv[m++] = (char *)"-c";
                wargv[m++] = (char *)"\"$@\"";
                wargv[m++] = (char *)"bash";
                for (k = 0; k < sg->argc; ++k) wargv[m++] = (char *)sg->argv[k];
                wargv[m] = NULL;
                setenv("ARSH_SHELL", "1", 1);
                /* The wrapper reads the caller's stdin only when that stdin is
                 * the CONSOLE feed (N4d): then the stage gets a real pty with
                 * its line discipline, exactly as the ordinary and شغّل routes
                 * give theirs.  A '<' file, a neighbour's pipe or /dev/null
                 * stays the stage's own stdin and the wrapper must not touch
                 * it. */
                if (infd[i] >= 0 && infd[i] != console_in)
                    setenv("ARSH_STDIN", "own", 1);
                if (errfd[i] >= 0) setenv("ARSH_STDERR", "own", 1);
                if (cfd > 2) {
                    snprintf(fdbuf, sizeof fdbuf, "%d", cfd);
                    setenv("ARSH_OUT_FD", fdbuf, 1);
                }
                execv(sh_wrapper, wargv);
            } else {
                execv(paths[i], (char *const *)sg->argv);
            }
        fail:
            /* launch failure: index and errno for the parent's row, which
             * names the stage; a successful exec closes this fd instead */
            {
                unsigned char fb[2];
                fb[0] = (unsigned char)i;
                fb[1] = (unsigned char)errno;
                (void)!write(fails[1], fb, 2);
            }
            _exit(127);
        }
        if (pgid <= 0) pgid = pid;
        setpgid(pid, pgid);                        /* both sides, the usual race */
        pids[i] = pid;
        ++npid;
    }
    sh_log("composition stages=%d forked=%d", p->nstage, npid);

    /* a read-only built-in writes its text while the children are alive, so a
     * reader always drains the pipe; then the parent drops every copy it
     * holds - an unclosed write end would leave the reader waiting for EOF */
    for (i = 0; i < p->nstage; ++i) {
        size_t pos = 0;
        if (sh_builtin_class(p->stage[i].argv[0]) != 1 || !logical[i]) continue;
        if (outfd[i] < 0) {                        /* the console: real rows */
            int first = 1;
            while (pos < logn[i]) {
                size_t e = pos, len;
                char line[4600];
                while (e < logn[i] && logical[i][e] != '\n') ++e;
                len = e - pos;
                if (len >= sizeof line) len = sizeof line - 1;
                memcpy(line, logical[i] + pos, len);
                line[len] = 0;
                if (sh_emit_row_spaced(line, first)) break;
                first = 0;
                pos = e + 1;
            }
        } else {
            ssize_t w = write(outfd[i], logical[i], logn[i]);
            (void)w;
        }
    }
    for (k = 0; k < nall; ++k) if (allfds[k] > 2) close(allfds[k]);
    nall = 0;                                      /* the parent holds none */
    if (fails[1] >= 0) { close(fails[1]); fails[1] = -1; }

    /* wait for every stage; the console belongs to the first unredirected
     * stage (N4d): its bytes go to that stage's stdin, ^C is SIGINT to the
     * pipeline's own process group and ^D ends that stage's input. */
    while (npid > 0) {
        struct pollfd pf[2];
        int npf = 1, pr;
        if (cfeed >= 0) sh_feed_flush(&cfeed);      /* retry a backed-up stage */
        pf[0].fd = tty_fd; pf[0].events = POLLIN; pf[0].revents = 0;
        if (fails[0] >= 0) {
            pf[1].fd = fails[0]; pf[1].events = POLLIN; pf[1].revents = 0;
            npf = 2;
        }
        pr = poll(pf, npf, 50);
        if (pr > 0 && (pf[0].revents & POLLIN)) {
            unsigned char in[256];
            ssize_t r = read(tty_fd, in, sizeof in);
            if (r > 0) sh_console_bytes(in, r, pgid, &cfeed);
        }
        /* N4d: a stage that never started ENDS the console feed.  The feed
         * exists for a reader that is part of a working pipeline; with a
         * stage that could not be launched the pipeline has a hole, so the
         * reader's bytes have nowhere to arrive and no downstream writer can
         * ever hand it the EOF that finishes it.  One EOF - the same one ^D
         * gives - lets that reader finish, so the launch failure is reported
         * and the status row printed instead of the shell outwaiting a stage
         * that can no longer be fed. */
        if (npf == 2 && (pf[1].revents & (POLLIN | POLLHUP)) &&
            nfb + 2 <= (int)sizeof fbuf) {
            ssize_t r = read(fails[0], fbuf + nfb, sizeof fbuf - (size_t)nfb);
            if (r > 0) nfb += (int)r;
            if (nfb >= 2 && cfeed >= 0) { close(cfeed); cfeed = -1; }
        }
        for (i = 0; i < p->nstage; ++i) {
            pid_t w;
            if (pids[i] <= 0) continue;
            w = waitpid(pids[i], &status[i], WNOHANG);
            if (w == pids[i] || (w < 0 && errno != EINTR)) { pids[i] = -1; --npid; }
        }
        /* N4d: when the feed's reader is the only stage left and it writes
         * into its neighbour's pipe, its bytes can no longer arrive anywhere:
         * the last reader of that pipe is gone.  One EOF - the same one ^D
         * gives - lets it finish, so a stage that died (or could never be
         * launched) is followed by its status row instead of the shell
         * outwaiting a pipeline that is already over.  Stage 0 with a '>' of
         * its own keeps its feed: its output is a file, not the dead pipe. */
        if (cfeed >= 0 && feed_to_pipe) {
            int others = 0;
            for (i = 1; i < p->nstage; ++i) if (pids[i] > 0) others = 1;
            if (!others) { close(cfeed); cfeed = -1; }
        }
    }

    /* a stage that could not be launched: its own row, by name.  Some pairs
     * may already have been read during the wait (the first one ended the
     * console feed), so they are taken from the accumulator, and the pipe is
     * drained once more for any that arrived after the last reap. */
    {
        ssize_t r;
        while (nfb + 2 <= (int)sizeof fbuf &&
               (r = read(fails[0], fbuf + nfb, sizeof fbuf - (size_t)nfb)) > 0)
            nfb += (int)r;
        for (k = 0; k + 1 < nfb; k += 2) {
            int idx = fbuf[k], e = fbuf[k + 1];
            if (idx < 0 || idx >= p->nstage) continue;
            snprintf(row, sizeof row, "%s: %s", p->stage[idx].argv[0], sh_err_ar(e));
            sh_emit_row(row);
        }
        close(fails[0]); fails[0] = -1;
    }

    /* the pipeline's status is the LAST stage's; a single redirection-only
     * stage never reaches here (it keeps the ordinary path).  Same row, same
     * Arabic wording and the same log line as the other two routes (N4d): the
     * pipeline is not a second kind of execution. */
    last = p->nstage - 1;
    sh_log("composition last=%d status=%d exited=%d sig=%d", last, status[last],
           WIFEXITED(status[last]) ? WEXITSTATUS(status[last]) : -1,
           WIFSIGNALED(status[last]) ? WTERMSIG(status[last]) : -1);
    sh_status_row(status[last]);

out:
    if (cfeed >= 0) { close(cfeed); cfeed = -1; }   /* the console feed, once */
    for (i = 0; i < nall; ++i) if (allfds[i] > 2) close(allfds[i]);
    if (fails[0] >= 0) close(fails[0]);
    if (fails[1] >= 0) close(fails[1]);
    for (i = 0; i < p->nstage; ++i) if (logical[i]) free(logical[i]);
    return rc;
}

/* Return to the prompt after a command has run: CRLF, re-assert the raw
 * termios, re-learn the row by CPR, re-arm the arrow, repaint the live
 * region.  ONE path, used by the ordinary spawn and by the executor. */
static void sh_command_return(void) {
    if (bytes("\r\n")) { flush_out(); return; }
    if (console_row == (long)terminal_rows - 1) sh_scrolled(); else console_row++;
    if (flush_out()) return;
    tcsetattr(tty_fd, TCSANOW, &sh_raw);          /* a child may have changed it */
    if (is_vt_console()) {
        long r0 = sh_learn_row();
        if (r0 >= 0) { sh_row_origin = r0; console_row = r0; }
        else sh_log("cpr timeout after command: keeping row_origin=%ld", sh_row_origin);
    } else {
        sh_row_origin = console_row;   /* pty: the tracked row after CRLF is correct */
    }
    pointer_visible = pointer_device_count > 0;
    sh_prev_rows = 0;
    (void)sh_paint();
}

/* ---- N4e: the session's own environment ----------------------------------
 * The console has no expansion syntax and grows none here: the setter takes a
 * NAME word and a VALUE word - exactly the two words the WO99 tokenizer
 * already produced - and keeps the value LITERALLY, byte for byte, whatever
 * it looks like.  setenv(3) is what carries it onward: every route forks from
 * this process and execs its child, so a later ordinary command, a شغّل and a
 * composition stage all inherit it.  The bounded list beside it is only the
 * session's own memory, so `البيئة` can show back what this session assigned,
 * in assignment order. */
#define SH_ENV_MAX 32
#define SH_ENV_NAME 64
#define SH_ENV_VALUE 256
static char sh_env_name[SH_ENV_MAX][SH_ENV_NAME];
static char sh_env_value[SH_ENV_MAX][SH_ENV_VALUE];
static int sh_env_n;

/* The name rule, stated once: ASCII letters, digits and '_', never starting
 * with a digit, at most 63 bytes.  A name outside the rule is refused by name
 * and nothing is assigned. */
static int sh_env_name_ok(const char *n) {
    size_t i, len = strlen(n);
    if (!len || len >= SH_ENV_NAME) return 0;
    if (n[0] >= '0' && n[0] <= '9') return 0;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static void sh_env_set(const char *name, const char *value) {
    char row[4600];
    int i;
    if (!sh_env_name_ok(name)) {
        snprintf(row, sizeof row, "اضبط البيئة: اسم غير صالح: %s", name);
        sh_emit_row(row);
        sh_emit_row("الاسم: حروف لاتينية وأرقام و _ فقط، ولا يبدأ برقم");
        return;
    }
    if (strlen(value) >= SH_ENV_VALUE) {
        sh_emit_row("اضبط البيئة: القيمة أطول من المسموح");
        return;
    }
    for (i = 0; i < sh_env_n; ++i) if (!strcmp(sh_env_name[i], name)) break;
    if (i == sh_env_n) {
        if (sh_env_n == SH_ENV_MAX) {
            sh_emit_row("اضبط البيئة: امتلأت متغيرات الجلسة");
            return;
        }
        snprintf(sh_env_name[i], SH_ENV_NAME, "%s", name);
        ++sh_env_n;
    }
    snprintf(sh_env_value[i], SH_ENV_VALUE, "%s", value);   /* literal, as typed */
    if (setenv(name, value, 1) != 0) {
        snprintf(row, sizeof row, "اضبط البيئة: تعذر الضبط: %s", name);
        sh_emit_row(row);
        return;
    }
    snprintf(row, sizeof row, "اضبط البيئة: %s=%s", name, value);
    sh_emit_row(row);
}

static void sh_env_show(void) {
    char row[4600];
    int i;
    if (!sh_env_n) { sh_emit_row("البيئة: لا متغيرات في هذه الجلسة"); return; }
    for (i = 0; i < sh_env_n; ++i) {
        snprintf(row, sizeof row, "%s=%s", sh_env_name[i], sh_env_value[i]);
        sh_emit_row(row);
    }
}

/* ---- WO109 / WO115: the navigation operand -------------------------------
 * `انتقل` takes ONE path.  The console's tokenizer splits on spaces and tabs
 * only (sh_parse), so a run of path characters typed without one arrives
 * whole.  Exactly ONE multiple-word form is accepted: the owner's own split
 * «. .» (REPORT-109 §109.1c - the typed separator between the dots), two
 * words that are each exactly one dot, resolved to the parent like «..».
 * Every OTHER line carrying a second operand is refused here - before any
 * path is built, before the phrase-level checks and before any filesystem
 * call - whatever its words are made of.
 * WO115 (audit F4).  Order 109 glued any run of words made solely of the
 * path-significant characters `.`, `/`, `~` into one path and let the
 * filesystem decide, so `انتقل .. ..` MOVED whenever a directory named `....`
 * existed and `انتقل / .` always moved to `/`: a genuine second operand was
 * accepted, not refused, and REPORT-109's "still refused" was conditional on
 * the glued path not existing.  Nothing is glued any more; a word that is not
 * the single dot is a genuine second operand.  Literal names stay reachable
 * through explicit relative paths (`.دليل`, `./المنزل`, `./~`): those are
 * single words and are untouched.
 * The words «المنزل» (home; TERMS.md, the owner's word of 2026-10-07) and
 * «لأعلى» (the parent; TERMS.md) and the prompt's own display form `~` are
 * resolved BEFORE any filesystem call: the home comes from the session's
 * environment (getenv HOME, the same source sh_dir_display compares by
 * device+inode), never re-derived from a prompt string.
 * WO115: an expansion that does not FIT the caller's path buffer is refused
 * (SH_SPLIT_MANY, the row naming the limit) rather than truncated into some
 * shorter path that might name a different directory.  Returns 0 with the
 * resolved path in `out`, or a refusal code for the consumer. */
static int sh_nav_fit(char *out, size_t outsz, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(out, outsz, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= outsz) {          /* the result was cut: refuse */
        out[0] = 0;
        sh_split_limit = (int)outsz - 1;
        return SH_SPLIT_MANY;
    }
    return 0;
}

static int sh_nav_target(int argc, char **argv, char *out, size_t outsz, int *joined) {
    static const char *const HOME_W = "المنزل", *const UP_W = "لأعلى";
    const char *home = getenv("HOME");
    const char *target = argv[1];
    size_t pl, nl;
    if (!home) home = "";
    *joined = 0;
    if (argc > 2) {                          /* more than one operand */
        if (argc != 3 || strcmp(argv[1], ".") || strcmp(argv[2], "."))
            return SH_SPLIT_COUNT;           /* a genuine extra operand */
        *joined = 1;                         /* the documented `. .` form only */
        return sh_nav_fit(out, outsz, "..");
    }
    pl = strlen(HOME_W); nl = strlen(UP_W);
    if (!strcmp(target, HOME_W)) return sh_nav_fit(out, outsz, "%s", home);
    if (!strncmp(target, HOME_W, pl) && target[pl] == '/')
        return sh_nav_fit(out, outsz, "%s/%s", home, target + pl + 1);
    if (!strcmp(target, UP_W)) return sh_nav_fit(out, outsz, "..");
    if (!strncmp(target, UP_W, nl) && target[nl] == '/')
        return sh_nav_fit(out, outsz, "../%s", target + nl + 1);
    if (target[0] == '~') {
        if (!target[1]) return sh_nav_fit(out, outsz, "%s", home);
        if (target[1] == '/') return sh_nav_fit(out, outsz, "%s/%s", home, target + 2);
    }
    return sh_nav_fit(out, outsz, "%s", target);
}

static int sh_builtin(int argc, char **argv) {
    const char *cmd = argv[0];
    const char *arg = argc > 1 ? argv[1] : NULL;
    if (!cmd) return 0;
    /* WO84: operands a built-in cannot take are refused, never dropped. */
    if (sh_name_eq(cmd,"اخرج")) { if (argc != 1) sh_arg_refuse(cmd, SH_SPLIT_COUNT);
                               sh_exit_requested=1; return 2; }
    if (sh_name_eq(cmd,"المسار")) { char p[4096]; if (argc != 1) { sh_arg_refuse(cmd, SH_SPLIT_COUNT); return 1; } if(!getcwd(p,sizeof p)) { sh_emit_row("المسار: لا يوجد ملف أو دليل بهذا الاسم"); } else sh_emit_row(p); return 1; }
    if (sh_name_eq(cmd,"انتقل")) {
        /* WO109: one operand, resolved by sh_nav_target (words, ~, a joined
         * run); the paths and the refusal rows are otherwise unchanged. */
        char target[4096], row[4600];
        int joined = 0, rc;
        if (!arg) { sh_emit_row("انتقل: معامل مفقود"); return 1; }
        rc = sh_nav_target(argc, argv, target, sizeof target, &joined);
        if (rc) { sh_arg_refuse(cmd, rc); return 1; }
        sh_log("[WO109] nav argc=%d typed=%s resolved=%s joined=%d", argc, arg, target, joined);
        if (chdir(target)) {
            /* A joined run that is not a directory is refused in the command's
             * own multi-operand voice; a single operand quotes itself. */
            if (joined) { sh_arg_refuse(cmd, SH_SPLIT_COUNT); return 1; }
            snprintf(row,sizeof row,"انتقل: تعذر الوصول إلى '%s': %s",arg,errno==EACCES?"الإذن مرفوض":(errno==ENOTDIR?"ليس دليلاً":"لا يوجد ملف أو دليل بهذا الاسم"));
            sh_emit_row(row);
        } else { char p[4096]; if(getcwd(p,sizeof p)){snprintf(row,sizeof row,"انتقل: %s",p);sh_emit_row(row);} }
        return 1;
    }
    /* WO79 D2: حرّر opens the file in the console editor.  A leading operand
     * is the file's path as typed, resolved against the session's cwd; the
     * row حفظ: Ctrl+S in مساعدة is the owner's ruling of 2026-10-02. */
    if (sh_name_eq(cmd,"حرّر")) { if(!arg) { sh_emit_row("حرّر: معامل مفقود"); return 1; } if (argc > 2) { sh_arg_refuse(cmd, SH_SPLIT_COUNT); return 1; } return sh_edit_open(arg); }
    /* The refreshed table emits compactly (see sh_emit_row_spaced): its 27
     * rows fit the mini's 33x120 console after the six-row banner.  The lookup code and
     * its pinned اعرض usage / unknown-query answers remain unchanged. */
    if (sh_name_eq(cmd,"مساعدة")) { size_t i; if (argc > 2) { sh_arg_refuse(cmd, SH_SPLIT_COUNT); return 1; } if (!arg) { for(i=0;i<SH_HELP_ROWS;i++) if(sh_emit_row_spaced(sh_help_rows[i],i==0))return 1; return 1; } if(sh_name_eq(arg,"اعرض"))sh_emit_row("اعرض: الاستخدام: اعرض مسار"); return sh_help_lookup(arg); }
    /* 73: the operand runs through the SAME wrapped child path as ordinary
     * commands ({sh_wrapper, sh_shell, "-c", arg}, ARSH_SHELL=1, stdin
     * /dev/null, its own process group) so its output is shaped exactly once
     * by شكل.  The exit-status row keeps its meaning: the wrapper exits with
     * the child's status. */
    if (sh_name_eq(cmd,"شغّل")) { if(!arg){sh_emit_row("شغّل: معامل مفقود");return 1;} sh_run_program(argc, argv); return 1; }
    /* WO85 D2: متصفح الصفحات <ملف|برنامج> ... opens the pager on logical
     * text - a file the filesystem has, or a program's captured stdout. */
    if (sh_name_eq(cmd,"متصفح")) { if (argc < 2 || !sh_name_eq(argv[1],"الصفحات")) { sh_emit_row("متصفح الصفحات: الاستخدام: متصفح الصفحات ملف"); return 1; } return sh_pg_open(argc - 2, argv + 2); }
    /* N4e: the parent session's environment.  `البيئة` shows what this
     * session assigned; `اضبط البيئة NAME VALUE` sets one.  The setter's
     * command word is اضبط and البيئة is the word that selects this built-in,
     * so any other second word falls through to the ordinary spawn and a
     * shipped program named اضبط ... (out/اضبط الأذونات) keeps its meaning.
     * The value is stored literally: بلا توسيع. */
    if (sh_name_eq(cmd,"البيئة")) { if (argc != 1) { sh_arg_refuse(cmd, SH_SPLIT_COUNT); return 1; } sh_env_show(); return 1; }
    if (sh_name_eq(cmd,"اضبط") && argc >= 2 && sh_name_eq(argv[1],"البيئة")) {
        if (argc < 4) { sh_emit_row("اضبط البيئة: الاستخدام: اضبط البيئة اسم قيمة"); return 1; }
        if (argc > 4) { sh_arg_refuse(cmd, SH_SPLIT_COUNT); return 1; }
        sh_env_set(argv[2], argv[3]);
        return 1;
    }
    return 0;
}

/* Run the committed line.  N4d: the child gets its stdin from the CONSOLE
 * through the same pipe-and-pump an ordinary composition stage and شغّل use -
 * the shell no longer hands it /dev/null and no longer throws typed bytes
 * away.  stdout/stderr are the real console tty, it leads its own process
 * group, and it execs the شكل wrapper: {wrapper, shell, "-c", line} with
 * ARSH_SHELL=1 exported.  Its exit status is reported on the same row شغّل
 * uses, so a program does not report one code through one route and another
 * through the other. */
static void sh_run_command(void) {
    char linebuf[MAX_COLS * 4 + 4], resolved_line[16384], cname[256];
    const char *spawn_line = linebuf;
    size_t n = 0, i;
    pid_t pid;
    int pfd[2], status = 0, feed = -1;

    for (i = 0; i < lines[0].len; ++i) n += sh_utf8(lines[0].cp[i], linebuf + n);
    linebuf[n] = 0;
    cname[0] = 0;

    /* WO107 107.1: an Enter on an EMPTY line is a SUBMISSION, not a no-op.
     * Order 106 §3.2 measured the shipped build leaving the screen byte-identical
     * for it (no fresh prompt, no حالة الخروج row) while every real submission
     * produced both.  There is no word for the tokenizer to take, so the layer
     * says so in its OWN voice — nothing is forked, nothing runs — and then
     * reports the status an empty command has (0, exactly what bash reports for
     * an empty line), the same two rows every other submission produces, before
     * returning to a fresh prompt.  An empty line is not pushed to history, as
     * in bash. */
    if (n == 0) {
        sh_emit_row("سطر فارغ");
        sh_status_row(0);
        sh_return_to_live();
        return;
    }

    sh_history_push();
    /* WO84: the session's tokenizer owns the line first.  A refusal (unmatched
     * quote, an unsupported composition operator, too many arguments) draws
     * its row and returns to the prompt: nothing is forked, nothing runs. */
    {
        char argbuf[MAX_COLS * 4 + 4];
        struct ShPipe pp;
        int changed = 0, stage;
        int rc = sh_parse(linebuf, argbuf, sizeof argbuf, &pp);
        if (rc < 0) {
            sh_arg_refuse(pp.stage[0].argc ? pp.stage[0].argv[0] : "", rc);
            if (!sh_ed_active) { lines[0].len = 0; sh_caret = 0; }
            sh_return_to_live();
            return;
        }
        for (stage = 0; stage < pp.nstage; ++stage) {
            int one = sh_resolve_stage_name(&pp.stage[stage]);
            if (stage == 0) changed = one;
        }
        /* N4d: the command's own name, kept for a launch failure's row now
         * that the spawn happens outside this block. */
        if (pp.stage[0].argc) snprintf(cname, sizeof cname, "%s", pp.stage[0].argv[0]);
        /* WO99 N4a: the parsed representation, logged before anything runs. */
        sh_pipe_trace(&pp, linebuf);
        /* WO99 N4b: a line that really composes - more than one stage, or any
         * redirection - takes the executor; everything else keeps the shipped
         * single-stage path byte for byte. */
        {
            int i, composes = pp.nstage > 1;
            for (i = 0; i < pp.nstage && !composes; ++i)
                if (pp.stage[i].nredir > 0) composes = 1;
            if (composes) {
                int r = sh_pipe_execute(&pp);
                if (!sh_ed_active) { lines[0].len = 0; sh_caret = 0; }
                if (r) sh_return_to_live();
                else sh_command_return();
                return;
            }
        }
        if (sh_builtin(pp.stage[0].argc, (char **)pp.stage[0].argv)) {
        if (sh_exit_requested) return;
        /* WO79 D2: حرّر hands the line to the editor - the file's current
         * line, not an empty prompt, is what the live region must paint. */
        if (!sh_ed_active) { lines[0].len = 0; sh_caret = 0; }
        /* D3F: sh_emit_row() now ends every built-in row with a real CR LF, so
         * the cursor is already on the row below it.  Re-learn the live
         * region's row here the way the post-command return below does.
         * Without it the paint below moves the cursor back UP to the old
         * origin, and the next built-in overwrites the row just written
         * (measured on the guest: `انتقل /إعدادات` and then `انتقل /برامج`
         * drew the same physical row). */
        sh_return_to_live();
        return;
        }
        if (changed) {
            if (sh_stage_shell_line(&pp.stage[0], resolved_line, sizeof resolved_line)) {
                sh_arg_refuse(pp.stage[0].argv[0], SH_SPLIT_COUNT);
                sh_return_to_live();
                return;
            }
            spawn_line = resolved_line;
        }
    }
    lines[0].len = 0; sh_caret = 0;              /* clear BEFORE the spawn */

    /* The live region is already shaped on screen — freeze it where it
     * stands and move to a fresh row for the child's output. */
    if (bytes("\r\n")) { flush_out(); return; }
    if (console_row == (long)terminal_rows - 1) sh_scrolled(); else console_row++;
    if (flush_out()) return;

    /* Erase the arrow before the child: a child can scroll the screen in
     * ways the editor cannot count. */
    pointer_visible = 0;

    /* N4d: the console is the child's stdin, through the one pipe-and-pump the
     * other two routes use.  The write end is CLOEXEC so no descendant of this
     * session can inherit it and hold the reader's EOF open. */
    if (pipe(pfd) != 0) {
        char row[512];
        snprintf(row, sizeof row, "%s: تعذر تشغيل البرنامج: %s", cname,
                 sh_err_ar(errno));
        sh_emit_row(row);
        sh_command_return();
        return;
    }
    fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    sh_feed_nonblock(pfd[1]);
    pid = fork();
    if (pid < 0) {
        char row[512];
        close(pfd[0]); close(pfd[1]);
        snprintf(row, sizeof row, "%s: تعذر تشغيل البرنامج: %s", cname,
                 sh_err_ar(errno));
        sh_emit_row(row);
        sh_command_return();
        return;
    }
    if (pid == 0) {
        char *argv[5];
        close(pfd[1]);
        if (dup2(pfd[0], 0) < 0) _exit(127);
        if (pfd[0] != 0) close(pfd[0]);
        setpgid(0, 0);
        setenv("ARSH_SHELL", "1", 1);
        argv[0] = sh_wrapper; argv[1] = sh_shell;
        argv[2] = (char *)"-c"; argv[3] = (char *)spawn_line; argv[4] = NULL;
        execv(sh_wrapper, argv);
        sh_log("execv %s failed: %s", sh_wrapper, strerror(errno));
        _exit(127);
    }
    close(pfd[0]);
    setpgid(pid, pid);                             /* with the child's own */
    feed = pfd[1];

    /* While waiting, the console belongs to the child: sh_child_wait moves its
     * bytes to the child's stdin and honours ^C (SIGINT to the child's group)
     * and ^D (end of input). */
    sh_child_wait(pid, pid, &feed, &status);
    if (feed >= 0) close(feed);
    sh_status_row(status);

    /* Return to the prompt, in order: CRLF, re-assert raw termios, re-learn
     * row_origin by CPR, re-arm and repaint the arrow, paint the fresh live
     * region. */
    sh_command_return();
}

/* ---- pointer: the arrow tracks, but shell mode has no click-to-place or
 * drag selection (out of scope).  Only the arrow position is maintained. */
static int sh_pointer_event(struct PointerDevice *d, const struct input_event *ev) {
    if (d->kind != POINTER_MICE) pointer_evdev_delivered = 1;
    if (ev->type == EV_ABS && (ev->code == ABS_X || ev->code == ABS_Y)) {
        if (ev->code == ABS_X) d->abs_x = ev->value; else d->abs_y = ev->value;
        pointer_cell = pointer_map(d, d->abs_x, d->abs_y);
        pointer_visible = 1;
        return 1;
    }
    if (ev->type == EV_REL && (ev->code == REL_X || ev->code == REL_Y)) {
        if (d->kind == POINTER_MICE && pointer_evdev_delivered) return 0;
        pointer_cell = pointer_map_delta(d, ev->code == REL_X ? ev->value : 0,
                                         ev->code == REL_Y ? ev->value : 0);
        pointer_visible = 1;
        return 1;
    }
    return 0;   /* buttons ignored in shell mode */
}
static int sh_pointer_events(struct PointerDevice *d) {
    int changed = 0;
    struct input_event ev;
    if (d->fd < 0) return 0;
    if (d->kind == POINTER_MICE) {
        unsigned char buf[64]; ssize_t nr;
        while ((nr = read(d->fd, buf, sizeof(buf))) > 0) {
            size_t i;
            for (i = 0; i < (size_t)nr; ++i) {
                d->packet[d->packet_have++] = buf[i];
                if (d->packet_have == 3) {
                    struct input_event move = {.type = EV_REL, .code = REL_X,
                                               .value = (signed char)d->packet[1]};
                    struct input_event ymove = {.type = EV_REL, .code = REL_Y,
                                                .value = -(signed char)d->packet[2]};
                    changed |= sh_pointer_event(d, &move);
                    changed |= sh_pointer_event(d, &ymove);
                    d->packet_have = 0;
                }
            }
        }
        if(nr==0||(nr<0&&dead_pointer_errno(errno)))drop_pointer_device(d);
        return changed;
    }
    for(;;){ssize_t nr=read(d->fd,&ev,sizeof(ev));if(nr==(ssize_t)sizeof(ev)){changed|=sh_pointer_event(d,&ev);continue;}if(nr==0||(nr<0&&dead_pointer_errno(errno)))drop_pointer_device(d);break;}
    return changed;
}
static int sh_pointer_step(void) {
    int any = 0;
    size_t i;
    struct Cell before = pointer_cell;
    for (i = 0; i < pointer_device_count; ++i)
        if (sh_pointer_events(&pointer_devices[i])) any = 1;
    if (!any) return 0;
    if (pointer_visible && before.x == pointer_cell.x && before.y == pointer_cell.y)
        return 0;
    return sh_paint();
}

/* ---- the main loop ------------------------------------------------------- */
static int shell_loop(void) {
    enum { NORMAL, ESC, CSI, SS3 } state = NORMAL;
    unsigned char b;
    unsigned int cp, acc = 0;
    int need = 0, got, p1 = 0, p2 = 0, field = 0;

    while (1) {
        struct pollfd fds[3 + MAX_POINTER_DEVICES];
        int pr;
        size_t i;
        pointer_scan_if_due();
        fds[0].fd = tty_fd; fds[0].events = POLLIN;
        fds[1].fd = input_listen_fd; fds[1].events = POLLIN;
        fds[2].fd = input_client_fd; fds[2].events = POLLIN;
        for (i = 0; i < MAX_POINTER_DEVICES; ++i) {
            fds[3+i].fd = i < pointer_device_count ? pointer_devices[i].fd : -1;
            fds[3+i].events = POLLIN;
        }
        pr = poll(fds, 3 + MAX_POINTER_DEVICES, pointer_poll_timeout(-1));
        if (pr < 0) continue;
        if(pr==0){pointer_scan_if_due();continue;}
        if (sh_pointer_step()) return -1;
        for(i=0;i<pointer_device_count;++i)if(fds[i+3].revents&(POLLERR|POLLNVAL|POLLHUP))drop_pointer_device(&pointer_devices[i]);
        if ((fds[2].revents & (POLLHUP|POLLERR|POLLNVAL)) && !(fds[2].revents & POLLIN)) { close(input_client_fd); input_client_fd=-1; }
        if (fds[1].revents & POLLIN) { input_socket_accept_newest(); continue; }
        if (fds[2].revents & POLLIN) { got=(int)input_socket_byte(&b); if(!got)continue; }
        else if (fds[0].revents & POLLIN) got=read(tty_fd,&b,1);
        else continue;
        if (got != 1) continue;

        {
            int again = 1;
            while (again) {
                again = 0;
                /* WO85: while the pager is open it owns every byte - its own
                 * ESC/CSI handling and its own key set - so the prompt's
                 * decoder is not fed at all and the line stays untouched. */
                if (sh_pg_active) { if (sh_pg_feed(b)) return -1; continue; }
                if (need && (b & 0xc0) != 0x80) { need = 0; acc = 0; again = 1; continue; }
                if (state == ESC) {
                    if (b == '[') { state = CSI; p1 = p2 = field = 0; continue; }
                    if (b == 'O') { state = SS3; continue; }
                    if (b == ' ') { latin_mode = !latin_mode; state = NORMAL;
                                    if (sh_paint()) return -1; continue; }
                    state = NORMAL; continue;
                }
                if (state == SS3) {
                    state = NORMAL;
                    if (b == 'D') sh_caret = sh_atom_next(sh_caret);
                    else if (b == 'C') sh_caret = sh_atom_prev(sh_caret);
                    else if (b == 'H') sh_caret = 0;
                    else if (b == 'F') sh_caret = lines[0].len;
                    else continue;
                    if (sh_paint()) return -1; continue;
                }
                if (state == CSI) {
                    if (b >= '@' && b <= '~') {
                        int p = field ? p2 : p1;
                        int shifted = (p1 == 1 && p2 == 2);
                        state = NORMAL;
                        if (shifted) continue;   /* Shift+Arrow: out of scope (no selection) */
                        /* WO79 D2 built the editor around ONE line, so Up/Down
                         * had no history to recall there and were dropped.
                         * WO83: in the editor Up/Down walk the buffer's lines
                         * (the viewport follows); at the prompt they still
                         * recall the shell's history.  PageUp/PageDown move by
                         * a viewport of lines and are inert at the prompt.
                         * Home/End and the horizontal moves land the caret on
                         * an atom boundary and clear the vertical goal. */
                        if (b == 'A') { if (sh_ed_active) sh_ed_move(-1); else sh_history_up(); }
                        else if (b == 'B') { if (sh_ed_active) sh_ed_move(1); else sh_history_down(); }
                        else if (b == '~' && p == 5) { if (!sh_ed_active) continue; sh_ed_page(-1); }
                        else if (b == '~' && p == 6) { if (!sh_ed_active) continue; sh_ed_page(1); }
                        else if (b == 'D') { sh_caret = sh_atom_next(sh_caret);
                                             if (sh_ed_active) sh_ed_goal = (size_t)-1; }
                        else if (b == 'C') { sh_caret = sh_atom_prev(sh_caret);
                                             if (sh_ed_active) sh_ed_goal = (size_t)-1; }
                        else if (b == 'H' || (b == '~' && (p == 1 || p == 7))) { sh_caret = 0;
                                             if (sh_ed_active) sh_ed_goal = (size_t)-1; }
                        else if (b == 'F' || (b == '~' && (p == 4 || p == 8))) { sh_caret = lines[0].len;
                                             if (sh_ed_active) sh_ed_goal = (size_t)-1; }
                        else if (b == '~' && p == 3) { size_t bl = lines[0].len; sh_delete_fwd();
                                                       if (sh_ed_active && lines[0].len != bl) sh_ed_dirty = 1; }
                        else continue;
                        if (sh_paint()) return -1; continue;
                    }
                    if (b == ';') { field = 1; continue; }
                    if (b >= '0' && b <= '9') { int *q = field ? &p2 : &p1;
                        if (*q < 1000000) *q = *q * 10 + b - '0'; continue; }
                    continue;
                }
                /* WO85 D4: Tab completes the prompt line's last word.  While
                 * the editor owns the line Tab keeps its old byte meaning -
                 * this order does not touch the editor's own contract. */
                if (b == 9 && !need && !sh_ed_active) { sh_complete(); continue; }
                /* Ctrl+Space (0x00): the Arabic/Latin toggle, as in the typing window. */
                if (b == 0 && !need) { latin_mode = !latin_mode;
                                       if (sh_paint()) return -1; continue; }
                if (b == 0x1b && !need) { state = ESC; continue; }
                /* WO79 D2: Ctrl+S saves the file being edited, Ctrl+Q leaves it.
                 * Outside the editor both keys do nothing (no shell meaning). */
                if (b == 0x13 && !need) { if (sh_ed_active) (void)sh_ed_save(); continue; }
                if (b == 0x11 && !need) { if (sh_ed_active) sh_ed_quit(); continue; }
                /* Ctrl+D exits ONLY on an empty line; otherwise byte-for-byte
                 * nothing - and never from the editor (WO79 D2: a stray EOF
                 * must not lose the buffer). */
                if (b == 4 && !need) { if (!sh_ed_active && lines[0].len == 0) return 0; continue; }
                /* Ctrl+C at the prompt abandons the line (never exits); in the
                 * editor it must not lose the buffer, so it does nothing. */
                if (b == 3 && !need) { if (sh_ed_active) continue;
                                       lines[0].len = 0; sh_caret = 0;
                                       if (sh_paint()) return -1; continue; }
                /* Ctrl+L repaints the live region in place (never clears the screen). */
                if (b == 12 && !need) { if (sh_paint()) return -1; continue; }
                /* Ctrl+U clears the current line. */
                if (b == 21 && !need) { size_t bl = lines[0].len;
                                        lines[0].len = 0; sh_caret = 0;
                                        if (sh_ed_active) { sh_ed_dirty = bl ? 1 : sh_ed_dirty;
                                                            sh_ed_goal = (size_t)-1; }
                                        if (sh_paint()) return -1; continue; }
                if ((b == 8 || b == 127) && !need) { size_t bl = lines[0].len;
                    sh_backspace();
                    if (sh_ed_active && lines[0].len != bl) sh_ed_dirty = 1;
                    if (sh_paint()) return -1; continue; }
                if ((b == '\r' || b == '\n') && !need) {
                    if (sh_ed_active) sh_ed_newline();   /* WO79 D2: append a file line */
                    else sh_run_command();   /* WO107 107.1: an empty line submits too */
                    if (sh_exit_requested) return 0;
                    continue;
                }
                got = decode_byte(b, &cp, &acc, &need);
                if (got == 1) {
                    int freed = translate_freed_b(&cp);
                    if (freed == 0) continue;
                    if (latin_mode) (void)translate_latin(&cp);
                    if (sh_insert(cp)) { if (sh_ed_active) sh_ed_dirty = 1;
                                         if (sh_paint()) return -1; }
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    struct termios raw;
    struct winsize ws;
    const char *wenv, *senv;
    (void)argc; (void)argv;

    load_justify_width();

    start_debug = getenv("AR_TYPE_START_DEBUG") != NULL;
    start_trace("shell startup: begin pid=%ld", (long)getpid());

    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) { perror("ar-shell: tty"); return 1; }
    if (input_socket_start() < 0) { perror("ar-shell: input socket"); return 1; }
    atexit(input_socket_stop);
    if (tcgetattr(tty_fd, &saved_termios)) { perror("ar-shell: tty"); return 1; }
    {
        int got_ws = 0;
        if (!ioctl(tty_fd, TIOCGWINSZ, &ws)) {
            got_ws = 1;
            if (ws.ws_col) terminal_cols = ws.ws_col;
            if (ws.ws_row) terminal_rows = ws.ws_row;
        }
        if (!terminal_cols) terminal_cols = 80;
        if (!terminal_rows) terminal_rows = 25;
        start_trace("shell window size: ioctl=%s %zux%zu",
                    got_ws ? "ok" : "failed", terminal_cols, terminal_rows);
    }
    read_framebuffer_size();
    pointer_debug = getenv("AR_TYPE_POINTER_DEBUG") != NULL;
    configure_pointer_drawing();
    pointer_arrow_glyph = font_has_cp(0xE020) ? 0xE020 : 0;

    have_termios = 1;
    raw = saved_termios;
    cfmakeraw(&raw);
    sh_raw = raw;
    if (tcsetattr(tty_fd, TCSANOW, &raw)) return 1;
    atexit(restore_terminal);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    /* WO84: شغّل feeds its child through a pipe; a child that exits first must
     * make the write fail with EPIPE, not kill the session with SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    /* Environment overrides for the child argv (the suite substitutes
     * recording stubs): AR_SHELL_WRAPPER (default /usr/local/bin/شكل) and
     * AR_SHELL_SHELL (default /bin/bash). */
    wenv = getenv("AR_SHELL_WRAPPER");
    senv = getenv("AR_SHELL_SHELL");
    snprintf(sh_wrapper, sizeof(sh_wrapper), "%s",
             wenv && *wenv ? wenv : "/usr/local/bin/\xD8\xB4\xD9\x83\xD9\x84");
    snprintf(sh_shell, sizeof(sh_shell), "%s", senv && *senv ? senv : "/bin/bash");

    discover_pointer();

    /* Start the live region on a fresh row, then learn its absolute row. */
    if (bytes("\r\n")) { flush_out(); return 1; }
    if (console_row == (long)terminal_rows - 1) sh_scrolled(); else console_row++;
    if (flush_out()) return 1;
    if (is_vt_console()) {
        long r0 = sh_learn_row();
        if (r0 >= 0) { sh_row_origin = r0; console_row = r0; }
        else sh_log("cpr timeout at startup: origin stays 0");
    } else {
        sh_row_origin = console_row;   /* pty: the tracked row after CRLF is correct */
    }

    /* Paint the startup arrow from the tablet's registered position. */
    if (pointer_device_count > 0) {
        size_t i;
        for (i = 0; i < pointer_device_count; ++i)
            if (pointer_devices[i].kind == POINTER_ABS) break;
        if (i < pointer_device_count) {
            pointer_cell = pointer_map(&pointer_devices[i],
                                       pointer_devices[i].abs_x,
                                       pointer_devices[i].abs_y);
            pointer_visible = 1;
        }
    }

    if (sh_paint()) return 1;
    (void)shell_loop();   /* returns 0 on Ctrl+D exit */
    return 0;
}
#endif /* AR_TYPE_SHELL */
