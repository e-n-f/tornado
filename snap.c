#include <stdio.h>
#include <stdlib.h>
#include <expat.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

unsigned mindate = 0;

#define LON_BITS ((7 * 8 - 4) / 2)
#define LON_MULT (((1 << (LON_BITS - 1)) - 1) / 180.0)
#define FOOT .00000274

#define BASE_YEAR 2004

struct __attribute__ ((__packed__)) pack {
	int lat : (LON_BITS - 1);
	int lon : LON_BITS;
	unsigned int when : 4;
	unsigned int uid : 8;
};

struct node {
	unsigned id; // now 76% of the way to 2^32!
};

struct node2 {
	int lat;
	int lon;
	unsigned date;
};

int nodecmp(const void *v1, const void *v2) {
	const struct node *n1 = v1;
	const struct node *n2 = v2;

	if (n1->id < n2->id) {
		return -1;
	}
	if (n1->id > n2->id) {
		return 1;
	}

	return 0;
}

// http://www.tbray.org/ongoing/When/200x/2003/03/22/Binary
void *search(const void *key, const void *base, size_t nel, size_t width,
		int (*cmp)(const void *, const void *)) {

	long long high = nel, low = -1, probe;
	while (high - low > 1) {
		probe = (low + high) >> 1;
		int c = cmp(((char *) base) + probe * width, key);
		if (c > 0) {
			high = probe;
		} else {
			low = probe;
		}
	}

	if (low < 0) {
		low = 0;
	}

	return ((char *) base) + low * width;
}

// boilerplate from
// http://marcomaggi.github.io/docs/expat.html#overview-intro
// Copyright 1999, Clark Cooper

#define BUFFSIZE        8192

char tmpfname[L_tmpnam];
int tmpfd;
long long tmplen;
struct pack *tmpmap;

unsigned theway = 0;
struct pack *thenodes[100000];
unsigned thenodecount = 0;
long long seq = 0;
long long maxnode = 0;

char tags[50000] = "";
char thetimestamp[50000] = "";
int bogus = 0;

unsigned parsedate(const char *date) {
	struct tm tm;

	if (sscanf(date, "%d-%d-%dT%d:%d:%d",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		&tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {

		int day = (tm.tm_mday - 1) + (31 * (tm.tm_mon - 1)) + (372 * (tm.tm_year - BASE_YEAR));
		return (day * 86400) +
			3600 * tm.tm_hour + 60 * tm.tm_min + tm.tm_sec;
	} else {
		fprintf(stderr, "can't parse date %s\n", date);
		return 0;
	}
}

void printdate(unsigned date) {
	int tod = date % 86400;
	date /= 86400;

	int day = date % 31 + 1;
	date /= 31;

	int month = date % 12 + 1;
	date /= 12;

	int year = date + BASE_YEAR;

	printf("%04d-%02d-%02d %02d:%02d:%02d ",
		year, month, day, tod / 3600, (tod / 60) % 60, tod % 60);
}

void appendq(char *s, const char *suffix) {
	while (*s != '\0') {
		s++;
	}

	while (*suffix != '\0') {
		if (*suffix == '"' || *suffix == '\\') {
			*s++ = '\\';
		}
		*s++ = *suffix++;
	}

	*s = '\0';
}

static void XMLCALL start(void *data, const char *element, const char **attribute) {
	if (strcmp(element, "node") == 0) {
		long long id = -1;
		int lat = INT_MIN;
		int lon = INT_MIN;
		unsigned date = 0;
		unsigned uid = 0;
		const char *timestamp = "none";

		int i;
		for (i = 0; attribute[i] != NULL; i += 2) {
			if (strcmp(attribute[i], "id") == 0) {
				id = atoll(attribute[i + 1]);
			} else if (strcmp(attribute[i], "lat") == 0) {
				lat = atof(attribute[i + 1]) * LON_MULT;
			} else if (strcmp(attribute[i], "lon") == 0) {
				lon = atof(attribute[i + 1]) * LON_MULT;
			} else if (strcmp(attribute[i], "uid") == 0) {
				uid = atoi(attribute[i + 1]) % 256;
			} else if (strcmp(attribute[i], "timestamp") == 0) {
				timestamp = attribute[i + 1];
				date = parsedate(attribute[i + 1]);
			}
		}

		if (id < 0) {
			fprintf(stderr, "node with no id\n");
		} else if (lat == INT_MIN || lon == INT_MIN) {
			fprintf(stderr, "node %lld has no location\n", id);
		} else {
			if ((id + 1) * sizeof(struct pack) >= tmplen) {
				if (munmap(tmpmap, tmplen) != 0) {
					perror("munmap");
					exit(EXIT_FAILURE);
				}

				tmplen = id * sizeof(struct pack) + 64 * 1024 * 1024;

				if (ftruncate(tmpfd, tmplen) != 0) {
					perror("ftruncate");
					exit(EXIT_FAILURE);
				}
				tmpmap = mmap(NULL, tmplen, PROT_READ | PROT_WRITE, MAP_SHARED, tmpfd, 0);
				if (tmpmap == MAP_FAILED) {
					perror("mmap");
					exit(EXIT_FAILURE);
				}
			}

			if (id > maxnode) {
				maxnode = id;
			}

			struct pack p;
			p.lat = lat;
			p.lon = lon;
			p.uid = uid;
#if 0
			if (date > mindate) {
				p.when = 1;
			} else {
				p.when = 0;
			}
#endif
			p.when = date / (371 * 86400);
			//printf("date %s %d\n", timestamp, p.when);

			tmpmap[id] = p;
		}

		if (seq++ % 100000 == 0) {
			fprintf(stderr, "node %lld  \r", id);
		}
	} else if (strcmp(element, "way") == 0) {
		int i;
		thenodecount = 0;
		strcpy(tags, "");
		strcpy(thetimestamp, "");
		bogus = 0;

		for (i = 0; attribute[i] != NULL; i += 2) {
			if (strcmp(attribute[i], "id") == 0) {
				theway = atoi(attribute[i + 1]);
			} else if (strcmp(attribute[i], "timestamp") == 0) {
				strcpy(thetimestamp, attribute[i + 1]);
			}
		}

		if (seq++ % 100000 == 0) {
			fprintf(stderr, "way %lld  \r", (long long) theway);
		}
	} else if (strcmp(element, "nd") == 0) {
		long long id = 0;

		int i;
		for (i = 0; attribute[i] != NULL; i += 2) {
			if (strcmp(attribute[i], "ref") == 0) {
				id = atoll(attribute[i + 1]);
			}
		}

		if (id >= 0 && id <= maxnode ) {
			struct pack *find = &tmpmap[id];

			if (find->lat == 0 && find->lon == 0) {
				fprintf(stderr, "probably bogus node for %lld\n", id);
				bogus = 1;
			} else {
				thenodes[thenodecount++] = find;
			}
		} else {
			fprintf(stderr, "bad node %lld\n", id);
			bogus = 1;
		}
	} else if (strcmp(element, "tag") == 0) {
		if (theway != 0) {
			const char *key = "";
			const char *value = "";

			int i;
			for (i = 0; attribute[i] != NULL; i += 2) {
				if (strcmp(attribute[i], "k") == 0) {
					key = attribute[i + 1];
				}
				if (strcmp(attribute[i], "v") == 0) {
					value = attribute[i + 1];
				}
			}

			int n = strlen(tags);
			if (n + 2 * strlen(key) + 2 * strlen(value) + 9 < sizeof(tags)) {
				sprintf(tags + strlen(tags), ", \"");
				appendq(tags, key);
				sprintf(tags + strlen(tags), "\": \"");
				appendq(tags, value);
				sprintf(tags + strlen(tags), "\"");
			}
		}
	}
}

static void XMLCALL end(void *data, const char *el) {
	if (strcmp(el, "way") == 0) {
		int i;
		int year = 0;
		for (i = 0; i < thenodecount; i++) {
			if (thenodes[i]->when > year) {
				year = thenodes[i]->when;
				break;
			}
		}

		if (thenodecount > 0 /* include */) {
			int polygon = 0;
			if (thenodes[0] == thenodes[thenodecount - 1]) {
				polygon = 1;
			}

			int start = 0;
			int end;
			int split = 0;
			for (end = 0; end <= thenodecount; end++) {
				if (end == thenodecount || 
					(!polygon && end > 0 && 
						(thenodes[end - 1]->when != thenodes[end]->when || 
						 thenodes[end - 1]->uid != thenodes[end]->uid))) {
					int within = 0;
					if (end != thenodecount) {
						split = 1;
					}

					printf("{ \"type\": \"Feature\"");

					if (polygon) {
						printf(", \"geometry\": { \"type\": \"Polygon\", \"coordinates\": [ [");
					} else {
						printf(", \"geometry\": { \"type\": \"LineString\", \"coordinates\": [");
					}

					if (start > 0) {
						printf("[ %lf,%lf ]", (thenodes[start - 1]->lon + thenodes[start]->lon) / LON_MULT / 2,
								      (thenodes[start - 1]->lat + thenodes[start]->lat) / LON_MULT / 2);
						within = 1;
					}

					int i;
					for (i = start; i < end; i++) {
						if (within) {
							printf(", ");
						}

						printf("[ %lf,%lf ]", thenodes[i]->lon / LON_MULT,
								      thenodes[i]->lat / LON_MULT);
						within = 1;
					}

					if (end < thenodecount) {
						if (within) {
							printf(", ");
						}

						printf("[ %lf,%lf ]", (thenodes[end - 1]->lon + thenodes[end]->lon) / LON_MULT / 2,
								      (thenodes[end - 1]->lat + thenodes[end]->lat) / LON_MULT / 2);
					}

					if (polygon) {
						printf("] ] }, \"properties\": { ");
					} else {
						printf("] }, \"properties\": { ");
					}

					printf("\"id\": %u", theway);
					printf(", \"year\": %u", thenodes[start]->when + BASE_YEAR);
					printf(", \"uid\": %u", thenodes[start]->uid);
					printf(", \"timestamp\": \"%s\"", thetimestamp);

					if (bogus) {
						printf(", \"bogus\": \"true\"");
					}
					if (split) {
						printf(", \"split\": \"true\"");
					}

					printf("%s", tags);
					printf(" } }\n");

					start = end;
				}
			}
		}

		theway = 0;
	}
}

int main(int argc, char *argv[]) {
	if (sizeof(struct pack) != 8) {
		fprintf(stderr, "pack size is somehow %lld\n\n", (long long) (sizeof(struct pack)));
		exit(EXIT_FAILURE);
	}

	fprintf(stderr, "lat/lon multiplier is %f for resolution of %f degrees, %f feet\n", LON_MULT, 1.0 / LON_MULT, 1.0 / LON_MULT / FOOT);

	int i;
	extern int optind;
	extern char *optarg;

	mindate = parsedate("2014-01-01T00:00:00Z");

	while ((i = getopt(argc, argv, "s:")) != -1) {
		switch (i) {
		default:
			fprintf(stderr, "Usage: %s [-s num]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	
	if (tmpnam(tmpfname) == NULL) {
		perror(tmpfname);
		exit(EXIT_FAILURE);
	}

	tmpfd = open(tmpfname, O_RDWR | O_CREAT, 0666);
	if (tmpfd < 0) {
		perror(tmpfname);
		exit(EXIT_FAILURE);
	}

	unlink(tmpfname);

	tmplen = 1000;
	if (ftruncate(tmpfd, tmplen) != 0) {
		perror("ftruncate");
		exit(EXIT_FAILURE);
	}
	tmpmap = mmap(NULL, tmplen, PROT_READ | PROT_WRITE, MAP_SHARED, tmpfd, 0);
	if (tmpmap == MAP_FAILED) {
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	XML_Parser p = XML_ParserCreate(NULL);
	if (p == NULL) {
		fprintf(stderr, "Couldn't allocate memory for parser\n");
		exit(EXIT_FAILURE);
	}

	XML_SetElementHandler(p, start, end);

	int done = 0;
	while (!done) {
		int len;
		char Buff[BUFFSIZE];

		len = fread(Buff, 1, BUFFSIZE, stdin);
		if (ferror(stdin)) {
       			fprintf(stderr, "Read error\n");
			exit(EXIT_FAILURE);
		}
		done = feof(stdin);

		if (XML_Parse(p, Buff, len, done) == XML_STATUS_ERROR) {
			fprintf(stderr, "Parse error at line %lld:\n%s\n", (long long) XML_GetCurrentLineNumber(p), XML_ErrorString(XML_GetErrorCode(p)));
			exit(EXIT_FAILURE);
		}
	}

	XML_ParserFree(p);
	return 0;
}
