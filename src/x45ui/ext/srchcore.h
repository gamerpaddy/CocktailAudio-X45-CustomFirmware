/* srchcore - the matching behind the station search (search.c), with no screen in it so it can be tested on its own.
 *
 * Names and what is typed are compared in one plain form: ASCII, lower case, every run of anything that is not a letter
 * or a digit one space ("Sunshine live - 90s" is "sunshine live 90s"). A query is a list of words; a name matches when
 * each word is somewhere in it. */
#ifndef X45UI_SRCHCORE_H
#define X45UI_SRCHCORE_H

#include <stddef.h>

#define SRCH_NAME 96   /* longest plain form kept of a name */
#define SRCH_MAXQ 40   /* longest query typed */

/* the plain form of `utf8` (cut to cap - 1, no space at either end) */
void srch_norm(char *out, size_t cap, const char *utf8);

/* 0: `name` (plain form) lacks a word of `query` (plain form); 1: the name begins with the query; 2: every word begins a
 * word of the name; 3: the words are somewhere inside words. An empty query matches everything with 1. */
int srch_rank(const char *name, const char *query);

/* Adds to cnt[c] for each character c that could be typed next after `typed` (as typed: letters, digits and single spaces,
 * possibly one at the end) so that `name` still matches: the one after the last word's occurrences, ' ' where a word
 * ends, the first letter of every word when the last word is empty. */
void srch_next(const char *name, const char *typed, int cnt[128]);

/* the characters a number key of the remote cycles through: "abc2" for 2 (the digit itself comes last; 0 is a space) */
const char *srch_tap_chars(int digit);

#endif
