#ifndef BENCODE_H
#define BENCODE_H
/**
 * A preemptible bencode parser and a simple bencode serializer.
 * Uses a static amount of memory. Event-based for parsing. Preemptible.
 * Requires static expression depths for items serialized.
 * Single header!
 * Requires C99.
 *
 * No dependencies, except the functions the user must himself define, below.
 */

/*******************************
 ** CLIENT PROVIDED FUNCTIONS **
 *******************************/

#ifndef BC_PROVIDED_ITOA
#error "no itoa provided"
#else
#define ITOA BC_PROVIDED_ITOA
#endif /* BC_PROVIDED_ITOA */

#ifndef BC_PROVIDED_MEMCPY
#error "no MEMCPY provided"
#else
#define MEMCPY BC_PROVIDED_MEMCPY
#endif /* BC_PROVIDED_MEMCPY */

#ifndef BC_PROVIDED_STRCAT
#error "no strcat provided"
#else
#define STRCAT BC_PROVIDED_STRCAT
#endif /* BC_PROVIDED_STRCAT */

struct bc_listener;
struct bc_client;

typedef void (*bc_lst_enter)(struct bc_listener* const *);
typedef void (*bc_lst_leave)(struct bc_listener* const *);

typedef void (*bc_dct_enter)(struct bc_listener* const *);
typedef void (*bc_dct_leave)(struct bc_listener* const *);

typedef void (*bc_int_value)(struct bc_listener* const *, long);
typedef void (*bc_str_chunk)(struct bc_listener* const *, const char*, unsigned long);

struct bc_listener {
	bc_lst_enter lst_enter;
	bc_lst_leave lst_leave;
	bc_dct_enter dct_enter;
	bc_dct_leave dct_leave;
	bc_int_value int_value;
	bc_str_chunk str_chunk;
};

enum bc_tag { BCT_INT, BCT_STR, BCT_DCT, BCT_LST, BCT_NONE /* <- Internal. */ };
enum bc_err { BCE_UNEXPECTED, BCE_OVERFLOW, BCE_EXTRAVAL };
typedef void (*bc_finished)(struct bc_client*);
typedef void (*bc_bad_data)(struct bc_client*, enum bc_err, enum bc_tag, char);
struct bc_client {
	struct bc_listener* listener;
	       bc_finished  finished;
	       bc_bad_data  bad_data;
};

#define BC_ISTREAM_INIT(client) { (client), {BCT_NONE}, 1, 0, 1, 0 }
struct bc_istream {
	struct bc_client* client;
	unsigned char  stack[60];
	unsigned short depth;        /* Nested types seen.                   */
	unsigned short state;        /* Mealy mach. state. See ``bc_1step.'' */
	unsigned long long neg : 01; /* 0 for positive, 1 for negative.      */
	unsigned long long acc : 63;
};


/* Mealy machine states: Not part of public interface. Undef'd later. */
#define _in  (1 << 8) /* In a scalar value.          */
#define _col (1 << 9) /* Passed the ':' in a string. */
#define _out (0)      /* Outside of a scalar value.  */

/*
 * Advance the machine by one character.
 *
 * This function is quite strict, only allowing a few possible invalid inputs.
 * It is also slow, and (if possible) shouldn't be called once each character.
 * Because of this, ``bc_chunk'' exists, and offers block-operations
 *  on some larger parts of strings.
 *
 * The function operates as something of a simple mealy machine.
 * The mealy machine proper has two states, ``in,'' and ``out.''
 * ``out'' is the de facto state, and implies the machine isn't in the middle
 *  of parsing an atomic scalar (string or integer), AND expects a value.
 * ``in''  is the state reached when the machine must begin parsing an
 *  integer or string, and in this mode characters are interpreted as
 *  part of that value.
 *
 * The automaton has a stack, which keeps track of which types nested have
 *  been encountered. It also has an ``accumulator'' and sign bit,
 *  which hold the running total of integer values encountered so far.
 * Finally, it has a single bit, ``colon,'' which defines whether the
 *  netstring colon ':' has been seen inside a string literal yet.
 *
 * The mealy machine is reasonably simple to ensure the correctness of,
 *  and it is entirely preemptible, caring only about a single character.
 * However, it is complicated and in practice requires more information
 *  than is provided by just the two states.
 *
 * Its defects are few; it admits only a few invalid inputs.
 * It allows multiple '+' and '-' characters within an integer literal.
 * Additionally, an integer literal without numerals is interpeted as 0.
 * These things are, of course, subject to change, and none should rely on them.
 * Consider them unspecified.
 */
static void bc_1step(struct bc_istream* is, char ch) {
#define SIGNAL(M) is->client->listener->M(&is->client->listener)
#define SIGNAL_VA(M, ...)\
	is->client->listener->M(&is->client->listener, __VA_ARGS__)

	enum bc_err     err = BCE_UNEXPECTED; /* Default error. */
	const enum bc_tag T = is->stack[is->depth-1];
	int real = 1; /* Is the character reported to str_chunk real? */

	if      (is->depth ==  0)                { err = BCE_EXTRAVAL; goto error; }
	else if (is->depth == sizeof(is->stack)) { err = BCE_OVERFLOW; goto error; }

	switch (ch | is->state) {
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		is->stack[is->depth++] = BCT_STR; is->state = _in;
		/* fallthru */
	case '0'|_in: case '1'|_in: case '2'|_in: case '3'|_in:
	case '4'|_in: case '5'|_in: case '6'|_in: case '7'|_in:
	case '8'|_in: case '9'|_in: is->acc *= 10, is->acc += ch - '0';     break;
	case 'i':
		is->stack[is->depth++]=BCT_INT;
		is->state = _in, is->neg = 0;                                   break;
	case 'd': is->stack[is->depth++]=BCT_DCT; SIGNAL(dct_enter);        break;
	case 'l': is->stack[is->depth++]=BCT_LST; SIGNAL(lst_enter);        break;
	case ':'|_in: is->state = _col;  real = 0;             goto str;    break;
	case '-'|_in: if (T==BCT_INT) is->neg = /* T */1; else goto error;  break;
	case '+'|_in: if (T==BCT_INT) is->neg = /* F */0; else goto error;  break;
	case 'e'|_in:
		if (T == BCT_INT) {
			SIGNAL_VA(int_value, (is->neg) ? -(long long)is->acc : is->acc);
			is->acc = 0;
		} else goto error;
		goto pop;
	case 'e':
		if      (T == BCT_LST) SIGNAL(lst_leave);
		else if (T == BCT_DCT) SIGNAL(dct_leave); else goto error;
		goto pop;
	default: str:
		if ((T == BCT_STR) & (is->state == _col)) {
			SIGNAL_VA(str_chunk, &ch, real);
			if (!(is->acc -= real)) goto pop;
		} else goto error;
	}
	return;
pop:
	--is->depth, is->state = _out; /* pop */
	if (is->depth == 1) --is->depth, is->client->finished(is->client);
	return;
error: is->client->bad_data(is->client, err, T, ch);
#undef SIGNAL
#undef SIGNAL_VA
}

static void bc_chunk(struct bc_istream* is, const char* s, unsigned long len) {
	while (len--) bc_1step(is, *s++); /* TODO: Optimize */
}

/**
 * Print a human-readable description of error information.
 *
 * Currently modifies and returns static memory, which is rather horrid.
 * In a multithreaded program, one would want to protect calls to this
 *  function with a lock. It's mostly for bebugging and printing terminal
 *  output - and one wouldn't EVER have more than a single thread doing THAT,
 *  now would he?
 */
static const char* bc_fmt_error(enum bc_err err, enum bc_tag typ, char chr) {
	struct err_msg_entry {
		char msg[256];
		int  char_idx;
		int  type_idx;
	}; static struct err_msg_entry msgs[] = {
		{"Unexpected '$' in ", 12, 18},  /* <- BCE_UNEXPECTED */
		{"Stack overflow @ '$', when parsing ", 18, 35},  /* <- BCE_OVERFLOW */
		{"Unexpected (nonnested) char; '$', ",  30, 34}}; /* <- BCE_EXTRAVAL */

	const char* reps[] = { /* BCT_INT: */ "int.", /* BCT_STR: */ "str.",
	                       /* BCT_DCT: */ "dct.", /* BCT_LST: */ "lst."};

	char* str = &msgs[err].msg[0];

	        str[msgs[err].char_idx] = chr;
	        str[msgs[err].type_idx] ='\0';
	STRCAT(&str[msgs[err].type_idx], reps[typ]);
	return str;
}

/****************
 ** SERIALIZER **
 ****************/

#define _bcLen(T, ...) ( sizeof(_bcArr(T, __VA_ARGS__)) / sizeof(T) )
#define _bcArr(T, ...) (T[]) { __VA_ARGS__ }
#define _bcVec(T, ...) { _bcLen(T, __VA_ARGS__), &_bcArr(T, __VA_ARGS__)[0] }
#define _bcObj(T, Sel, obj) (struct bc_object) { .Tag = (T), .as. Sel = obj }

#define _bcDct(T, ...) ((struct bc_dict_b) _bcVec(T, __VA_ARGS__))
#define _bcLst(T, ...) ((struct bc_list_b) _bcVec(T, __VA_ARGS__))

#define bcDct(...) _bcObj(BCT_DCT, Dct, (_bcDct(struct bc_kvpair, __VA_ARGS__)))
#define bcLst(...) _bcObj(BCT_LST, Lst, (_bcLst(struct bc_object, __VA_ARGS__)))
#define bcInt(num) _bcObj(BCT_INT, Int, num)
#define bcStr(str) _bcObj(BCT_STR, Str, ((struct bc_string){STRLEN(str), str}))

struct bc_string { unsigned Len; const char*       Buf; };
struct bc_dict_b { unsigned Len; struct bc_kvpair* Arr; };
struct bc_list_b { unsigned Len; struct bc_object* Arr; };
struct bc_object {
	enum bc_tag Tag;
	union {
		long             Int;
		struct bc_string Str;
		struct bc_list_b Lst;
		struct bc_dict_b Dct;
	} as;
};
struct bc_kvpair { struct bc_object Pair[2]; /* 0 key, 1 val */ };

/**
 * Serialize a bencode_object array out to a character buffer.
 *
 * Note: Currently clobbers the bencode object passed in.
 */
static unsigned long bc_write(char* s, struct bc_object obj) {
	unsigned long     l,f = 0; /* f is a flag, either 0 or 1.     */
	const char* const fst = s; /* Initial position of the string. */
	struct bc_object *o;
	struct bc_list_b first = {1, &obj};
	struct { unsigned N; struct bc_list_b* B[32]; } S = {1, {&first}};

	/*
	 * This function maintains a small, fixed-sized stack, above.
	 * The stack references small arrays, and iteratively
	 *  the loop below pulls an element from the array, and if appropriate,
	 *  pops any newly-empty arrays from the stack, pushing on new ones
	 *  as they are encountered. Namely, dict arrays and list arrays.
	 *
	 * The algorithm for doing this is not very simple, but the basic
	 *  premise is pretty easy to understand.
	 *
	 * TODO: Having ``parent pointers'' settable in the bencode objects
	 *  proper may lead to a more straightforward approach.
	 */

	void* v; /* Pull an item off the array currently at the top of the stack. */
	while ((S.N-=f) && ((l=--S.B[S.N-1]->Len), (f=!l), (v=S.B[S.N-1]->Arr++))) {
		switch (o=(struct bc_object*)v, o->Tag) {
		case BCT_STR:
		    s+=ITOA(s, l= o->as.Str.Len); *s++=':';
		     MEMCPY(s, o->as.Str.Buf, l);  s += l;               break;
		case BCT_INT: *s++='i'; s+=ITOA(s, o->as.Int); *s++='e'; break;
		case BCT_LST: *s++='l'; S.B[S.N++-f]=&o->as.Lst;         break;
		case BCT_DCT: *s++='d'; S.B[S.N++-f]=&o->as.Lst;
		                             o->as.Lst.Len *= 2;         break;
	} if (f & (v != &obj)) *s++ = 'e'; }
	return s - fst;
}

#undef   ITOA
#undef STRCAT /* Not part of interface. */
#undef _col
#undef _in
#undef _out
#endif /* BENCODE_H */
