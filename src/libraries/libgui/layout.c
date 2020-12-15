#include <assert.h>
#include <fcntl.h>
#include <libgui/bmp.h>
#include <libgui/layout.h>
#include <libgui/msgui.h>
#include <libgui/psf.h>
#include <math.h>
#include <mqueue.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void init_fonts()
{
	uint32_t fd = open("/usr/share/fonts/ter-powerline-v16n.psf", O_RDONLY);

	struct stat *stat = calloc(1, sizeof(struct stat));
	fstat(fd, stat);

	char *buf = calloc(stat->st_size, sizeof(char));
	read(fd, buf, stat->st_size);
	psf_init(buf, stat->st_size);

	free(stat);
	close(fd);
}

int get_character_width(char ch)
{
	struct psf_t *font = get_current_font();
	return (ch == '\t' ? 4 : 1) * font->width;
}

int get_character_height(char ch)
{
	struct psf_t *font = get_current_font();
	return font->height;
}

static struct window *find_child_element_from_position(struct window *win, int32_t px, int32_t py, int32_t mx, int32_t my)
{
	struct window *iter_win;
	list_for_each_entry(iter_win, &win->children, sibling)
	{
		int32_t cx = px + iter_win->graphic.x;
		int32_t cy = py + iter_win->graphic.y;

		if (cx < mx && mx < cx + iter_win->graphic.width &&
			cy < my && my < cy + iter_win->graphic.height)
		{
			struct window *w = find_child_element_from_position(iter_win, cx, cy, mx, my);
			return w ? w : iter_win;
		}
	}
	return NULL;
}

static struct window *get_top_level_window(struct window *win)
{
	struct window *top = win;
	while (top->parent)
		top = top->parent;
	return top;
}

static void add_event_handler(struct window *win, char *event_name, EVENT_HANDLER handler)
{
	hashmap_put(&win->events, event_name, handler);
}

void gui_draw_retangle(struct window *win, int x, int y, unsigned int width, unsigned int height, uint32_t bg)
{
	int py = min_t(int, y + height, win->graphic.height);
	int px = min_t(int, x + width, win->graphic.width);
	for (int i = y; i < py; i += 1)
		for (int j = x; j < px; j += 1)
			*(uint32_t *)(win->graphic.buf + (i * win->graphic.width + j) * 4) = bg;
}

static void gui_create_window(struct window *parent, struct window *win, int32_t x, int32_t y, uint32_t width, uint32_t height, bool transparent, struct ui_style *style)
{
	char *pid = calloc(sizeof(char), WINDOW_NAME_LENGTH);
	itoa(getpid(), 10, pid);

	struct msgui *msgui_sender = calloc(1, sizeof(struct msgui));
	msgui_sender->type = MSGUI_WINDOW;
	struct msgui_window *msgwin = (struct msgui_window *)msgui_sender->data;
	msgwin->x = x;
	msgwin->y = y;
	msgwin->width = width;
	msgwin->height = height;
	msgwin->transparent = transparent;
	if (parent)
		memcpy(msgwin->parent, parent->name, WINDOW_NAME_LENGTH);
	memcpy(msgwin->sender, pid, WINDOW_NAME_LENGTH);
	free(pid);
	int32_t sfd = mq_open(WINDOW_SERVER_QUEUE, O_WRONLY, &(struct mq_attr){
															 .mq_msgsize = sizeof(struct msgui),
															 .mq_maxmsg = 32,
														 });
	mq_send(sfd, (char *)msgui_sender, 0, sizeof(struct msgui));
	mq_close(sfd);
	free(msgui_sender);

	win->graphic.x = x;
	win->graphic.y = y;
	win->graphic.width = width;
	win->graphic.height = height;
	win->style = style;
	win->add_event_listener = add_event_handler;
	win->parent = parent;

	INIT_LIST_HEAD(&win->children);
	hashmap_init(&win->events, hashmap_hash_string, hashmap_compare_string, 0);

	if (parent)
		list_add_tail(&win->sibling, &parent->children);

	int32_t wfd = mq_open(msgwin->sender, O_RDONLY | O_CREAT, &(struct mq_attr){
																  .mq_msgsize = WINDOW_NAME_LENGTH,
																  .mq_maxmsg = 32,
															  });
	mq_receive(wfd, win->name, 0, WINDOW_NAME_LENGTH);
	mq_close(wfd);

	uint32_t buf_size = width * height * 4;
	int32_t fd = shm_open(win->name, O_RDWR, 0);
	win->graphic.buf = (char *)mmap(NULL, buf_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
}

static void gui_label_set_text(struct ui_label *label, char *text)
{
	memset(label->window.graphic.buf, 0, label->window.graphic.width * label->window.graphic.height * 4);

	label->text = strdup(text);
	uint32_t scanline = label->window.graphic.width * 4;
	psf_puts(label->text, label->window.style->padding_left, label->window.style->padding_top, 0xffffffff, 0x00, label->window.graphic.buf, scanline);
}

void gui_create_label(struct window *parent, struct ui_label *label, int32_t x, int32_t y, uint32_t width, uint32_t height, char *text, struct ui_style *style)
{
	gui_create_window(parent, &label->window, x, y, width, height, false, style);
	label->set_text = gui_label_set_text;
	label->set_text(label, text);
}

void gui_create_input(struct window *parent, struct ui_input *input, int32_t x, int32_t y, uint32_t width, uint32_t height, char *content)
{
	gui_create_window(parent, &input->window, x, y, width, height, false, NULL);
}

void gui_create_button(struct window *parent, struct ui_button *button, int32_t x, int32_t y, uint32_t width, uint32_t height, bool transparent, struct ui_style *style)
{
	gui_create_window(parent, &button->window, x, y, width, height, transparent, style);
}

void gui_create_block(struct window *parent, struct ui_block *block, int32_t x, int32_t y, uint32_t width, uint32_t height, bool transparent, struct ui_style *style)
{
	gui_create_window(parent, &block->window, x, y, width, height, transparent, style);
}

void gui_render(struct window *win)
{
	struct msgui *msgui = calloc(1, sizeof(struct msgui));
	msgui->type = MSGUI_RENDER;
	struct msgui_render *msgrender = (struct msgui_render *)msgui->data;
	memcpy(msgrender->sender, win->name, WINDOW_NAME_LENGTH);

	int32_t sfd = mq_open(WINDOW_SERVER_QUEUE, O_WRONLY, &(struct mq_attr){
															 .mq_msgsize = sizeof(struct msgui),
															 .mq_maxmsg = 32,
														 });
	mq_send(sfd, (char *)msgui, 0, sizeof(struct msgui));
	mq_close(sfd);
	free(msgui);
}

void gui_focus(struct window *win)
{
	struct msgui *msgui = calloc(1, sizeof(struct msgui));
	msgui->type = MSGUI_FOCUS;
	struct msgui_focus *msgfocus = (struct msgui_focus *)msgui->data;
	memcpy(msgfocus->sender, win->name, WINDOW_NAME_LENGTH);

	int32_t sfd = mq_open(WINDOW_SERVER_QUEUE, O_WRONLY, &(struct mq_attr){
															 .mq_msgsize = sizeof(struct msgui),
															 .mq_maxmsg = 32,
														 });
	mq_send(sfd, (char *)msgui, 0, sizeof(struct msgui));
	mq_close(sfd);
	free(msgui);
}

void gui_close(struct window *win)
{
	struct msgui *msgui = calloc(1, sizeof(struct msgui));
	msgui->type = MSGUI_CLOSE;
	struct msgui_close *msgclose = (struct msgui_close *)msgui->data;
	memcpy(msgclose->sender, win->name, WINDOW_NAME_LENGTH);

	int32_t sfd = mq_open(WINDOW_SERVER_QUEUE, O_WRONLY, &(struct mq_attr){
															 .mq_msgsize = sizeof(struct msgui),
															 .mq_maxmsg = 32,
														 });
	mq_send(sfd, (char *)msgui, 0, sizeof(struct msgui));
	mq_close(sfd);
	free(msgui);
}

char *load_bmp(char *path)
{
	int32_t fd = open(path, O_RDONLY);

	struct stat *stat = calloc(1, sizeof(struct stat));
	fstat(fd, stat);

	char *buf = calloc(stat->st_size, sizeof(char));
	read(fd, buf, stat->st_size);
	close(fd);
	free(stat);

	return buf;
}

void set_background_color(struct window *win, uint32_t bg)
{
	for (int i = 0; i < win->graphic.height; ++i)
	{
		char *ibuf = win->graphic.buf + i * win->graphic.width * 4;
		for (int j = 0; j < win->graphic.width; ++j)
		{
			(*(uint32_t *)ibuf) = bg;
			ibuf += 4;
		}
	}
}

void close_window(struct window *btn_win)
{
	struct window *app = get_top_level_window(btn_win);
	gui_close(app);
	exit(0);
}

void init_window_bar(struct window *win)
{
	struct ui_block *block = calloc(1, sizeof(struct ui_block));
	gui_create_block(win, block, 0, 0, win->graphic.width, 24, false, NULL);
	set_background_color(&block->window, 0xFF282C32);

	struct ui_button *btn_close = calloc(1, sizeof(struct ui_button));
	char *close_buf = load_bmp("/usr/share/images/close.bmp");
	btn_close->icon = close_buf;
	gui_create_button(&block->window, btn_close, win->graphic.width - 20, 4, 16, 16, true, NULL);
	bmp_draw(&btn_close->window.graphic, close_buf, 4, 4);
	btn_close->window.add_event_listener(&btn_close->window, WINDOW_EVENT_CLICK, close_window);

	struct ui_button *btn_minus = calloc(1, sizeof(struct ui_button));
	char *minus_buf = load_bmp("/usr/share/images/minus.bmp");
	btn_minus->icon = minus_buf;
	gui_create_button(&block->window, btn_minus, win->graphic.width - 38, 4, 16, 16, true, NULL);
	bmp_draw(&btn_minus->window.graphic, minus_buf, 4, 4);
}

void init_window_body(struct window *win)
{
	struct ui_block *block = calloc(1, sizeof(struct ui_block));
	gui_create_block(win, block, 0, 24, win->graphic.width, win->graphic.height - 24, false, NULL);
	set_background_color(&block->window, 0xFF000000);
}

struct window *init_window(int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	init_fonts();
	struct window *win = calloc(1, sizeof(struct window));
	gui_create_window(NULL, win, x, y, width, height, false, NULL);
	init_window_bar(win);
	init_window_body(win);

	return win;
}

#define MAX_FD 10
void enter_event_loop(struct window *win, void (*event_callback)(struct xevent *evt), int *fds, unsigned int nfds, void (*fds_callback)(struct pollfd *, unsigned int))
{
	gui_focus(win);

	struct xevent *event = calloc(1, sizeof(struct xevent));
	int32_t wfd = mq_open(win->name, O_RDONLY | O_CREAT, &(struct mq_attr){
															 .mq_msgsize = sizeof(struct xevent),
															 .mq_maxmsg = 32,
														 });
	memset(event, 0, sizeof(struct xevent));

	struct pollfd pfds[MAX_FD] = {
		{.fd = wfd, .events = POLLIN},
	};
	for (int i = 1; i < MAX_FD; ++i)
	{
		pfds[i].fd = -1;
		pfds[i].events = POLLIN;
	}

	while (true)
	{
		for (unsigned int i = 0; i < nfds; ++i)
			pfds[i + 1].fd = fds[i];

		int nr = poll(pfds, MAX_FD);
		if (nr <= 0)
			continue;

		for (int32_t i = 0; i < MAX_FD; ++i)
		{
			if (!(pfds[i].revents & POLLIN))
				continue;

			if (pfds[i].fd == wfd)
			{
				mq_receive(wfd, (char *)event, 0, sizeof(struct xevent));

				if (event->type == XBUTTON_EVENT)
				{
					struct xbutton_event *bevent = (struct xbutton_event *)event->data;

					if (bevent->action == XBUTTON_PRESS)
					{
						struct window *active_win = find_child_element_from_position(win, win->graphic.x, win->graphic.y, bevent->x, bevent->y);
						if (active_win)
						{
							EVENT_HANDLER handler = hashmap_get(&active_win->events, WINDOW_EVENT_CLICK);
							if (handler)
								handler(active_win);
						}
					}
				}
				if (event_callback)
					event_callback(event);
				memset(event, 0, sizeof(struct xevent));
			}
			else if (fds_callback)
				fds_callback(pfds, MAX_FD);
		};
	}

	mq_close(wfd);
	free(event);
}
