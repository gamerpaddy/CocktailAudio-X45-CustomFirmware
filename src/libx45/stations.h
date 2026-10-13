/* stations - the internet-radio preset list shown on the RADIO screen.
 *
 * The file is meant to be edited by hand on the device, so the parser is
 * forgiving about layout and strict about content:
 *
 *     # comment
 *     Name | stream URL
 *
 * - blank lines, comment lines and CRLF line endings are fine
 * - a row without a '|' , with an empty URL, or with a URL that is not
 *   http:// or https:// is skipped (a file:// or "..\n" entry must never reach
 *   an MPD command line)
 * - control characters anywhere in a row skip the row
 * - names and URLs are clipped to the struct sizes; a row whose URL would be
 *   clipped is skipped instead, because a truncated URL plays the wrong thing
 */
#ifndef X45_STATIONS_H
#define X45_STATIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#define X45_STATION_NAME 64
#define X45_STATION_URL  256

struct x45_station {
	char name[X45_STATION_NAME];
	char url[X45_STATION_URL];
};

/* Parse `text` (NUL-terminated). Fills up to `max` entries and returns how many.
 * If `skipped` is non-NULL it receives the number of rows that were rejected. */
int x45_stations_parse(const char *text, struct x45_station *out, int max,
		       int *skipped);

/* Read and parse `path`. Returns the entry count, or -errno if it cannot be
 * read (a missing file is -ENOENT, not an empty list). */
int x45_stations_load(const char *path, struct x45_station *out, int max,
		      int *skipped);

#ifdef __cplusplus
}
#endif
#endif /* X45_STATIONS_H */
