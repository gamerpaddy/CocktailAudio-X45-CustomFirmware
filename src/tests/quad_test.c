/* Unit test for the quadrature decoder in libx45/x45in.c.
 *
 * The knobs can only be exercised by hand, so the decode logic is verified
 * here with synthetic channel transitions instead. Includes the .c directly so
 * the test can reach quad_feed(), which is static.
 *
 * Build as an ARM binary and run it on the device (it touches no hardware):
 *   make test   &&   x45push out/quad_test /home/root/x45new/bin/quad_test
 */
#include "../libx45/x45in.c"

#include <stdio.h>

static int failures;

static void check(const char *what, int got, int want)
{
	int ok = (got == want);
	if (!ok)
		failures++;
	printf("  %-52s got %+3d want %+3d  %s\n", what, got, want,
	       ok ? "ok" : "FAIL");
}

/* Feed one state transition, working out which channel moved.
 * state is (A<<1)|B. Returns the detents reported. */
static int step(struct x45_quad *q, int from, int to, int per_detent)
{
	int a_from = (from >> 1) & 1, b_from = from & 1;
	int a_to = (to >> 1) & 1, b_to = to & 1;

	if (a_from != a_to && b_from != b_to) {
		/* both at once: a missed edge. Deliver as a single channel event
		 * so we exercise the illegal-transition path. */
		return quad_feed(q, 0, a_to, per_detent);
	}
	if (a_from != a_to)
		return quad_feed(q, 0, a_to, per_detent);
	if (b_from != b_to)
		return quad_feed(q, 1, b_to, per_detent);
	return 0; /* no change */
}

/* Clockwise Gray sequence: 00 -> 01 -> 11 -> 10 -> 00 */
static const int cw[] = { 0, 1, 3, 2, 0 };
/* Counter-clockwise: 00 -> 10 -> 11 -> 01 -> 00 */
static const int ccw[] = { 0, 2, 3, 1, 0 };

static int rotate(struct x45_quad *q, const int *seq, int turns, int per_detent)
{
	int total = 0, t, i;
	for (t = 0; t < turns; t++)
		for (i = 0; i < 4; i++)
			total += step(q, seq[i], seq[i + 1], per_detent);
	return total;
}

/* Primes the decoder: the very first channel change only establishes where the
 * shaft is resting, so it reports nothing. */
static void prime(struct x45_quad *q, int per_detent)
{
	quad_feed(q, 1, 1, per_detent); /* B -> 1, state becomes 01 */
	q->state = 0;                   /* then pretend we are at 00 */
	q->accum = 0;
}

int main(void)
{
	struct x45_quad q;

	puts("quadrature decoder tests\n");

	puts("1. full rotations, 4 transitions per detent");
	memset(&q, 0, sizeof q); prime(&q, 4);
	check("one clockwise revolution (4 edges) = +1 detent",
	      rotate(&q, cw, 1, 4), +1);
	memset(&q, 0, sizeof q); prime(&q, 4);
	check("one counter-clockwise revolution = -1 detent",
	      rotate(&q, ccw, 1, 4), -1);
	memset(&q, 0, sizeof q); prime(&q, 4);
	check("ten clockwise revolutions = +10",
	      rotate(&q, cw, 10, 4), +10);
	memset(&q, 0, sizeof q); prime(&q, 4);
	check("ten counter-clockwise = -10",
	      rotate(&q, ccw, 10, 4), -10);

	puts("\n2. every edge reported (ticks_per_detent = 1)");
	memset(&q, 0, sizeof q); prime(&q, 1);
	check("one clockwise revolution = +4 ticks",
	      rotate(&q, cw, 1, 1), +4);
	memset(&q, 0, sizeof q); prime(&q, 1);
	check("one counter-clockwise revolution = -4 ticks",
	      rotate(&q, ccw, 1, 1), -4);

	puts("\n3. direction reversal nets out");
	memset(&q, 0, sizeof q); prime(&q, 4);
	check("5 clockwise then 5 counter-clockwise = 0",
	      rotate(&q, cw, 5, 4) + rotate(&q, ccw, 5, 4), 0);
	memset(&q, 0, sizeof q); prime(&q, 4);
	check("3 clockwise then 1 counter-clockwise = +2",
	      rotate(&q, cw, 3, 4) + rotate(&q, ccw, 1, 4), +2);

	puts("\n4. dither at a detent boundary must not emit (the real-world case:");
	puts("   the shaft resting between detents, one channel stuck 'pressed')");
	memset(&q, 0, sizeof q); prime(&q, 4);
	{
		int total = 0, i;
		for (i = 0; i < 20; i++) {
			total += step(&q, 0, 1, 4); /* 00 -> 01 */
			total += step(&q, 1, 0, 4); /* 01 -> 00  back again */
		}
		check("20 back-and-forth wobbles = 0 detents", total, 0);
	}

	puts("\n5. a stuck channel then continued rotation still tracks");
	memset(&q, 0, sizeof q); prime(&q, 4);
	{
		int total = 0;
		/* advance a quarter, sit there (repeat the same value, no change),
		 * then complete the revolution */
		total += step(&q, 0, 1, 4);
		total += quad_feed(&q, 1, 1, 4); /* B already 1: no state change */
		total += quad_feed(&q, 1, 1, 4);
		total += step(&q, 1, 3, 4);
		total += step(&q, 3, 2, 4);
		total += step(&q, 2, 0, 4);
		check("quarter turn, idle repeats, then finish = +1", total, +1);
	}

	puts("\n6. illegal diagonal transition (a missed edge) emits nothing spurious");
	memset(&q, 0, sizeof q); prime(&q, 4);
	{
		/* 00 -> 11 is impossible in Gray code; the table yields 0 */
		int d = quad_tbl[0][3];
		check("quad_tbl[00][11] (diagonal) = 0", d, 0);
		d = quad_tbl[3][0];
		check("quad_tbl[11][00] (diagonal) = 0", d, 0);
		d = quad_tbl[1][2];
		check("quad_tbl[01][10] (diagonal) = 0", d, 0);
	}

	puts("\n7. the table is antisymmetric (reversing a move reverses the sign)");
	{
		int i, j, bad = 0;
		for (i = 0; i < 4; i++)
			for (j = 0; j < 4; j++)
				if (quad_tbl[i][j] != -quad_tbl[j][i])
					bad++;
		check("all 16 entries satisfy t[i][j] == -t[j][i]", bad, 0);
	}

	puts("\n8. both knobs are independent");
	{
		struct x45_indev in;
		struct x45_event ev;
		int i, vol = 0, scr = 0;

		memset(&in, 0, sizeof in);
		in.fd_fast = -1;   /* no sampler thread in these tests */
		in.decode = X45_DECODE_QUADRATURE; /* this test is about that model */
		in.ticks_per_detent = 4;
		in.fd_panel = in.fd_ir = -1;
		/* prime both */
		handle_code(&in, &ev, X45_ENC_VOL_B, 1, 0);
		in.knob[X45_KNOB_VOLUME].state = 0;
		handle_code(&in, &ev, X45_ENC_SCROLL_B, 1, 0);
		in.knob[X45_KNOB_SCROLL].state = 0;

		/* turn volume clockwise one revolution, interleaved with scroll
		 * counter-clockwise one revolution */
		for (i = 0; i < 4; i++) {
			int vfrom = cw[i], vto = cw[i + 1];
			int sfrom = ccw[i], sto = ccw[i + 1];
			int code, val;

			code = ((vfrom >> 1) != (vto >> 1)) ? X45_ENC_VOL_A : X45_ENC_VOL_B;
			val = (code == X45_ENC_VOL_A) ? ((vto >> 1) & 1) : (vto & 1);
			if (handle_code(&in, &ev, code, val, 0) &&
			    ev.kind == X45_EV_ENCODER && ev.code == X45_KNOB_VOLUME)
				vol += ev.ticks;

			code = ((sfrom >> 1) != (sto >> 1)) ? X45_ENC_SCROLL_A : X45_ENC_SCROLL_B;
			val = (code == X45_ENC_SCROLL_A) ? ((sto >> 1) & 1) : (sto & 1);
			if (handle_code(&in, &ev, code, val, 0) &&
			    ev.kind == X45_EV_ENCODER && ev.code == X45_KNOB_SCROLL)
				scr += ev.ticks;
		}
		check("interleaved: volume knob = +1", vol, +1);
		check("interleaved: scroll wheel = -1", scr, -1);
	}

	puts("\n9. buttons are not mistaken for encoder channels");
	{
		struct x45_indev in;
		struct x45_event ev;
		int r;
		memset(&in, 0, sizeof in);
		in.fd_fast = -1;   /* no sampler thread in these tests */
		in.ticks_per_detent = 4;
		in.fd_panel = in.fd_ir = -1;

		r = handle_code(&in, &ev, X45_KEY_POWER, 1, 0);
		check("power press reported", r && ev.kind == X45_EV_KEY, 1);
		check("power code preserved", ev.code, X45_KEY_POWER);
		r = handle_code(&in, &ev, X45_KEY_VOL_PUSH, 1, 0);
		check("volume-push is a KEY, not an encoder",
		      r && ev.kind == X45_EV_KEY, 1);
		r = handle_code(&in, &ev, X45_KEY_OK, 0, 0);
		check("ok release reported as release", r && ev.pressed == 0, 1);
		r = handle_code(&in, &ev, X45_KEY_MENU, 2, 0);
		check("autorepeat (value 2) suppressed", r, 0);
	}

	puts("");
	puts("10. DIRECTION decode (NOT the default: the lines are quadrature phases, see x45in.h)");
	{
		struct x45_indev in;
		struct x45_event ev;
		int i, vol = 0, scr = 0, r;

		memset(&in, 0, sizeof in);
		in.fd_fast = -1;   /* no sampler thread in these tests */
		in.decode = X45_DECODE_DIRECTION;
		in.fd_panel = in.fd_ir = -1;

		for (i = 0; i < 10; i++) {
			if (handle_code(&in, &ev, X45_ENC_VOL_UP, 1, 0) &&
			    ev.kind == X45_EV_ENCODER)
				vol += ev.ticks;
			/* the matching release must contribute nothing */
			if (handle_code(&in, &ev, X45_ENC_VOL_UP, 0, 0) &&
			    ev.kind == X45_EV_ENCODER)
				vol += ev.ticks;
		}
		check("10 presses on volume-up = +10", vol, +10);

		for (i = 0; i < 4; i++)
			if (handle_code(&in, &ev, X45_ENC_VOL_DOWN, 1, 0) &&
			    ev.kind == X45_EV_ENCODER)
				vol += ev.ticks;
		check("then 4 on volume-down = +6", vol, +6);

		for (i = 0; i < 3; i++)
			if (handle_code(&in, &ev, X45_ENC_SCROLL_DN, 1, 0) &&
			    ev.kind == X45_EV_ENCODER)
				scr += ev.ticks;
		check("scroll-down x3 = -3", scr, -3);
		check("volume unaffected by scroll", vol, +6);

		r = handle_code(&in, &ev, X45_ENC_VOL_UP, 0, 0);
		check("a bare release emits nothing", r, 0);

		vol = 0;
		handle_code(&in, &ev, X45_ENC_VOL_UP, 1, 0); vol += ev.ticks;
		handle_code(&in, &ev, X45_ENC_VOL_UP, 0, 0);
		handle_code(&in, &ev, X45_ENC_VOL_UP, 0, 0);
		handle_code(&in, &ev, X45_ENC_VOL_UP, 0, 0);
		check("press then three releases = +1", vol, +1);
	}

	/* ---- the model the hardware actually follows: quadrature -------------------
	 * `states` is the (A,B) sequence recorded from the real volume knob on the
	 * unit (A = KEY 474, B = KEY 475; 2-bit value A<<1|B). The first twenty
	 * entries are four clean clockwise cycles; the rest is a person wiggling the
	 * knob. Replaying it through the decoder with the shipped defaults is the
	 * regression test for the bug where this knob was dead. */
	{
		static const int states[] = {
			2, 3, 1, 0, 2, 3, 1, 0, 2, 3, 1, 0, 2, 3, 1, 0, 2, 3, 1, 3,
			2, 0, 1, 3, 2, 3, 1, 3, 2, 3, 1, 3, 1, 3, 2, 3, 2, 0
		};
		const int n = (int)(sizeof states / sizeof states[0]);
		struct x45_indev fwd, raw;
		struct x45_event ev;
		int i, prev, clean_ticks = 0, total_inv = 0, total_raw = 0, single_bit = 1;

		memset(&fwd, 0, sizeof fwd);
		fwd.fd_fast = -1;   /* no sampler thread in these tests */
		fwd.decode = X45_DECODE_QUADRATURE;   /* the shipped defaults */
		fwd.ticks_per_detent = 4;
		fwd.invert = 1;
		raw = fwd;
		raw.invert = 0;

		prev = 0;                              /* the decoder starts at 00 */
		for (i = 0; i < n; i++) {
			int diff = prev ^ states[i];
			int code;

			if (diff != 1 && diff != 2) {
				single_bit = 0;                /* a 2-bit jump is not an edge */
				break;
			}
			code = (diff == 2) ? X45_ENC_VOL_UP : X45_ENC_VOL_DOWN;
			if (handle_code(&fwd, &ev, code, (states[i] >> (diff == 2 ? 1 : 0)) & 1, 0) &&
			    ev.kind == X45_EV_ENCODER) {
				total_inv += ev.ticks;
				if (i < 20)
					clean_ticks += ev.ticks;
			}
			if (handle_code(&raw, &ev, code, (states[i] >> (diff == 2 ? 1 : 0)) & 1, 0) &&
			    ev.kind == X45_EV_ENCODER)
				total_raw += ev.ticks;
			prev = states[i];
		}
		check("recorded sequence is made of single-edge steps", single_bit, 1);
		/* 20 recorded edges: the first only establishes the state, 19 count,
		 * 19 / 4 = 4 whole detents */
		check("4 clean clockwise cycles = +4 detents (shipped polarity)", clean_ticks, +4);
		check("inverting the polarity negates every tick", total_raw, -total_inv);
		check("the whole recording nets a positive turn, not zero", total_inv > 0, 1);
	}

	/* a counter-clockwise turn (B leads) is the exact reverse and must go negative */
	{
		struct x45_indev in;
		struct x45_event ev;
		int i, ticks = 0;
		/* B rises, A rises, B falls, A falls: states 01 11 10 00 */
		static const struct { int code, val; } seq[] = {
			{ X45_ENC_VOL_DOWN, 1 }, { X45_ENC_VOL_UP, 1 },
			{ X45_ENC_VOL_DOWN, 0 }, { X45_ENC_VOL_UP, 0 },
		};

		memset(&in, 0, sizeof in);
		in.fd_fast = -1;   /* no sampler thread in these tests */
		in.decode = X45_DECODE_QUADRATURE;
		in.ticks_per_detent = 4;
		in.invert = 1;
		for (i = 0; i < 4 * 6; i++)            /* six cycles */
			if (handle_code(&in, &ev, seq[i % 4].code, seq[i % 4].val, 0) &&
			    ev.kind == X45_EV_ENCODER)
				ticks += ev.ticks;
		check("counter-clockwise turn goes negative", ticks < 0, 1);
	}

	/* the two knobs are independent decoders */
	{
		struct x45_indev in;
		struct x45_event ev;
		int i, vol = 0, scr = 0;
		static const int cw_code[4] = { 0, 1, 0, 1 };   /* A, B, A, B */
		static const int cw_val[4]  = { 1, 1, 0, 0 };

		memset(&in, 0, sizeof in);
		in.fd_fast = -1;   /* no sampler thread in these tests */
		in.decode = X45_DECODE_QUADRATURE;
		in.ticks_per_detent = 4;
		in.invert = 1;
		for (i = 0; i < 24; i++) {
			int b = cw_code[i % 4];
			if (handle_code(&in, &ev, b ? X45_ENC_VOL_DOWN : X45_ENC_VOL_UP, cw_val[i % 4], 0) &&
			    ev.kind == X45_EV_ENCODER && ev.code == X45_KNOB_VOLUME)
				vol += ev.ticks;
		}
		for (i = 0; i < 12; i++)
			if (handle_code(&in, &ev, cw_code[i % 4] ? X45_ENC_SCROLL_DN : X45_ENC_SCROLL_UP,
					cw_val[i % 4], 0) &&
			    ev.kind == X45_EV_ENCODER && ev.code == X45_KNOB_SCROLL)
				scr += ev.ticks;
		check("volume knob: 6 cycles = +5 detents (first edge only primes)", vol, +5);
		check("scroll wheel: 3 cycles = +2 detents, independent of volume", scr, +2);
	}

	/* ---- the real read path, with the kernel's real record format ---------------
	 * This is the regression test for the bug that made the whole panel dead:
	 * x45_input_next() must accept the kernel's 16-byte records. A pipe stands in
	 * for /dev/input/event1; everything after read() is the production code. */
	{
		struct x45_indev in;
		struct x45_event ev;
		int fds[2], r, ticks = 0, got_key = 0, i;
		/* sec, usec, type, code, value as the 3.14 kernel writes them */
		struct x45_input_event rec[] = {
			{ 1, 0, 1, 68, 1 }, { 1, 0, 0, 0, 0 },   /* MENU down, SYN */
			{ 1, 1, 1, 68, 0 }, { 1, 1, 0, 0, 0 },   /* MENU up,   SYN */
		};

		check("a kernel input record is 16 bytes", (int)sizeof(struct x45_input_event), 16);
		if (pipe(fds) < 0)
			return 2;
		memset(&in, 0, sizeof in);
		in.fd_fast = -1;   /* no sampler thread in these tests */
		in.fd_panel = fds[0];
		in.fd_ir = -1;
		in.decode = X45_DECODE_QUADRATURE;
		in.ticks_per_detent = 4;
		in.invert = 1;

		if (write(fds[1], rec, sizeof rec) != (ssize_t)sizeof rec)
			return 2;
		for (i = 0; i < 4; i++) {
			r = x45_input_next(&in, &ev, 100);
			if (r == 1 && ev.kind == X45_EV_KEY && ev.code == 68 && ev.pressed)
				got_key = 1;
		}
		check("a MENU press written as raw kernel records arrives", got_key, 1);

		/* one clockwise cycle on the volume lines (A rises, B rises, A falls,
		 * B falls), repeated: the primed first edge plus 4 cycles = +3 detents */
		{
			static const struct { uint16_t code; int32_t val; } cyc[4] = {
				{ X45_ENC_VOL_UP, 1 }, { X45_ENC_VOL_DOWN, 1 },
				{ X45_ENC_VOL_UP, 0 }, { X45_ENC_VOL_DOWN, 0 },
			};
			for (i = 0; i < 16; i++) {
				struct x45_input_event k = { 2, 0, 1, cyc[i % 4].code, cyc[i % 4].val };
				struct x45_input_event s = { 2, 0, 0, 0, 0 };

				write(fds[1], &k, sizeof k);
				write(fds[1], &s, sizeof s);
			}
			for (i = 0; i < 40; i++) {
				r = x45_input_next(&in, &ev, 20);
				if (r == 1 && ev.kind == X45_EV_ENCODER && ev.code == X45_KNOB_VOLUME)
					ticks += ev.ticks;
			}
			check("4 raw knob cycles reach the caller as +3 detents", ticks, +3);
		}
		close(fds[0]);
		close(fds[1]);
	}

	/* ---- the remote: a held button is a key event once, then bare NEC repeat frames (MSC_SCAN and SYN, no key) ---- */
	{
		struct x45_indev in;
		struct x45_event ev;
		int fds[2], r, i, n = 0, seq[8];
		/* sec, usec, type, code, value: EV_MSC (4) MSC_SCAN (4), EV_KEY (1), EV_SYN (0) */
		struct x45_input_event rec[] = {
			{ 10, 0, 4, 4, 0x65751 }, { 10, 0, 1, 62, 1 }, { 10, 0, 0, 0, 0 },          /* Vol+ down */
			{ 10, 108000, 4, 4, 0x65751 }, { 10, 108000, 0, 0, 0 },                      /* held: repeat frame */
			{ 10, 216000, 4, 4, 0x65751 }, { 10, 216000, 0, 0, 0 },                      /* held: repeat frame */
			{ 10, 300000, 4, 4, 0x65733 }, { 10, 300000, 0, 0, 0 },                      /* another, unknown button: nothing */
			{ 10, 470000, 1, 62, 0 }, { 10, 470000, 0, 0, 0 },                           /* released */
			{ 10, 520000, 4, 4, 0x65751 }, { 10, 520000, 0, 0, 0 },                      /* a stray frame after the release: nothing */
			{ 11, 0, 4, 4, 0x6570a }, { 11, 0, 1, 103, 1 }, { 11, 0, 0, 0, 0 },          /* Up down */
			{ 11, 900000, 4, 4, 0x6570a }, { 11, 900000, 0, 0, 0 },                      /* 0.9 s later: not a repeat of that press */
		};

		if (pipe(fds) < 0)
			return 2;
		memset(&in, 0, sizeof in);
		in.fd_panel = -1;
		in.fd_ir = fds[0];
		in.fd_fast = -1;
		if (write(fds[1], rec, sizeof rec) != (ssize_t)sizeof rec)
			return 2;
		for (i = 0; i < 40; i++) {
			r = x45_input_next(&in, &ev, 20);
			if (r == 1 && ev.kind == X45_EV_IR && n < 8)
				seq[n++] = ev.code * 10 + ev.pressed;
		}
		check("the remote: Vol+ down, 2 repeats, up, then Up down = 5 events", n, 5);
		check("  Vol+ pressed", n > 0 ? seq[0] : -1, 621);
		check("  Vol+ held (first repeat)", n > 1 ? seq[1] : -1, 622);
		check("  Vol+ held (second repeat)", n > 2 ? seq[2] : -1, 622);
		check("  Vol+ released; an unknown button and a stray frame add nothing", n > 3 ? seq[3] : -1, 620);
		check("  Up pressed, and its late frame is no repeat", n > 4 ? seq[4] : -1, 1031);
		close(fds[0]);
		close(fds[1]);
	}

	printf("\n%s  (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
