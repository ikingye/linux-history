/*
 * selection.h
 *
 * Interface between console.c, tty_io.c, vt.c, vc_screen.c and selection.c
 */

#ifndef _LINUX_SELECTION_H_
#define _LINUX_SELECTION_H_

#include <linux/vt_buffer.h>

extern int sel_cons;

extern void clear_selection(void);
extern int set_selection(const unsigned long arg, struct tty_struct *tty, int user);
extern int paste_selection(struct tty_struct *tty);
extern int sel_loadlut(const unsigned long arg);
extern int mouse_reporting(void);
extern void mouse_report(struct tty_struct * tty, int butt, int mrx, int mry);

#define video_num_columns	(vc_cons[currcons].d->vc_cols)
#define video_num_lines		(vc_cons[currcons].d->vc_rows)
#define video_size_row		(vc_cons[currcons].d->vc_size_row)
#define video_screen_size	(vc_cons[currcons].d->vc_screenbuf_size)
#define can_do_color		(vc_cons[currcons].d->vc_can_do_color)

extern int console_blanked;

extern unsigned char color_table[];
extern int default_red[];
extern int default_grn[];
extern int default_blu[];

extern void do_unblank_screen(void);
extern unsigned short *screen_pos(int currcons, int w_offset, int viewed);
extern unsigned short screen_word(int currcons, int offset, int viewed);
extern int scrw2glyph(unsigned short scr_word);
extern void complement_pos(int currcons, int offset);
extern void invert_screen(int currcons, int offset, int count, int shift);

extern void getconsxy(int currcons, char *p);
extern void putconsxy(int currcons, char *p);

#endif