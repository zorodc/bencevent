#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int itoa(char* out, long in) { return sprintf(out, "%ld", in); }
#define BC_PROVIDED_MEMCPY memcpy
#define BC_PROVIDED_STRCAT strcat
#define BC_PROVIDED_ITOA     itoa
#define STRLEN strlen
#include "../bencode.h"

struct bc_squawker;
void sq_on_lst_enter(struct bc_listener* const *);
void sq_on_lst_leave(struct bc_listener* const *);
void sq_on_dct_enter(struct bc_listener* const *);
void sq_on_dct_leave(struct bc_listener* const *);
void sq_on_int_value(struct bc_listener* const *, long);
void sq_on_str_chunk(struct bc_listener* const *, const char*, unsigned long);
enum sq_events { sq_lst_enter, sq_lst_leave,
                 sq_dct_enter, sq_dct_leave,
                 sq_int_value, sq_str_chunk, _sq_events_max };

#define sq_report_strs (const char*[])	 \
	{ "lst_enter ",      "lst_leave ",   \
	  "dct_enter ",      "dct_leave ",   \
	  "int_value{%ld} ", "str_chunk{%ld;%s} " }

#define sq_parrot_strs (const char*[])	 \
	{ "l",    "e",                       \
	  "d",    "e",                       \
	  "i%lde", "%ld:%s" }

enum sq_last { WASNT_STR, WAS_STR };
struct bc_squawker {
	struct bc_client base;
	enum sq_last     last;
	unsigned         ccnt; /* character count. */
	FILE*            logs; /* Where to squawk. */
	const char*     *strs;// of length [_sq_events_max];
	char             buff[1024];
} bc_squawker_proto = {
	.base.listener = &(struct bc_listener) {
		sq_on_lst_enter,
		sq_on_lst_leave,
		sq_on_dct_enter,
		sq_on_dct_leave,
		sq_on_int_value,
		sq_on_str_chunk
	},
	.last = WASNT_STR, .ccnt = 0
};

static void sq_flush(struct bc_squawker* s) {
	if (s->last != WAS_STR) return;
	s->buff[s->ccnt] = '\0';
	fprintf(s->logs, s->strs[sq_str_chunk], (long)s->ccnt, s->buff);

	s->last = WASNT_STR; /* Mark string in buffer as printed. */
	s->ccnt = 0;
}

void sq_on_lst_enter(struct bc_listener* const * b)
{
	struct bc_squawker* s = (struct bc_squawker*)b;
	sq_flush(s); fprintf(s->logs, s->strs[sq_lst_enter]);
}
void sq_on_lst_leave(struct bc_listener* const * b)
{
	struct bc_squawker* s = (struct bc_squawker*)b;
	sq_flush(s); fprintf(s->logs, s->strs[sq_lst_leave]);

}
void sq_on_dct_enter(struct bc_listener* const * b)
{
	struct bc_squawker* s = (struct bc_squawker*)b;
	sq_flush(s); fprintf(s->logs, s->strs[sq_dct_enter]);
}

void sq_on_dct_leave(struct bc_listener* const * b)
{
	struct bc_squawker* s = (struct bc_squawker*)b;
	sq_flush(s); fprintf(s->logs, s->strs[sq_dct_leave]);
}

void sq_on_int_value(struct bc_listener* const * b, long val)
{
	struct bc_squawker* s = (struct bc_squawker*)b;
	sq_flush(s); fprintf(s->logs, s->strs[sq_int_value], val);
}

void
sq_on_str_chunk(struct bc_listener* const * b, const char* str, unsigned long l)
{
	struct bc_squawker* s = (struct bc_squawker*)b;
	s->last = WAS_STR;
	while (l--) s->buff[s->ccnt++] = *str++;
}

void nop_finished(struct bc_client* _) { /* NOP */ }
void nop_bad_data(struct bc_client* _, enum bc_err e, enum bc_tag t, char ch) {}

void sq_finished(struct bc_client* b)
{
	struct bc_squawker* s = (struct bc_squawker*)b;
	sq_flush(s); fprintf(s->logs, " |DONE| ");
}

void sq_bad_data(struct bc_client* b, enum bc_err e, enum bc_tag t, char ch)
{
	struct bc_squawker* s = (struct bc_squawker*)b;
	const char* err = bc_fmt_error(e, t, ch);
	sq_flush(s); fprintf(s->logs, " |ERROR: %s|", err);
}

void bc_test(char* str, unsigned long len)
{
	struct bc_squawker cl = bc_squawker_proto;
	struct bc_istream  is = BC_ISTREAM_INIT((struct bc_client*)&cl);

	/* Configure client. */
	cl.logs = stdout;
	cl.strs = sq_parrot_strs;

	/* Addnl. methods. */
#if 1
	cl.base.finished = sq_finished;
	cl.base.bad_data = sq_bad_data;
#else
	cl.base.finished = nop_finished;
	cl.base.bad_data = nop_bad_data;
#endif

	bc_chunk(&is, str, len);
	sq_flush(&cl);
	fputc('\n', cl.logs);

	memset(str, '\0', len);
}

int main()
{
	char buf[128] = {'\0'};
	unsigned long len;
	len = bc_write(buf, bcStr("hi"));        bc_test(buf, len);
	len = bc_write(buf, bcInt(100));         bc_test(buf, len);
	len = bc_write(buf, bcInt(-10));         bc_test(buf, len);
	len = bc_write(buf, bcInt(20000));       bc_test(buf, len);
	len = bc_write(buf, bcStr("Hello!"));    bc_test(buf, len);
	len = bc_write(buf, bcLst(bcStr("hi"))); bc_test(buf, len);
	buf[0] = 'l'; buf[1] = 'e';              bc_test(buf, 2);
	buf[0] = 'd'; buf[1] = 'e';              bc_test(buf, 2);
	buf[0] = '0'; buf[1] = ':';              bc_test(buf, 2);

	len = bc_write(buf, bcLst(bcStr("Hello!"), bcInt(100),
	                          bcDct( {bcStr("hello"), bcLst(bcInt(10))},
	                                 {bcStr("hi"), bcInt(10)} ),
	                          bcInt(200)));
	bc_test(buf, len);
}
