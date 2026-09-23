// Linux runtime support and monotonic arena allocation.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ARENA_RESERVE_SIZE (64ULL * 1024ULL * 1024ULL)
#define ARENA_ALIGNMENT 16ULL
#define TIMEZONE_FILE_MAX (1024ULL * 1024ULL)
#define TIMEZONE_PATH_SIZE 4096

#ifndef INT64_C
#define INT64_C(value) value##LL
#endif

#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif

#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

enum {
    max_config_string = 4096,
    qdos_unix_epoch_delta_seconds = 283996800
};

typedef struct arena {
    uint8_t *base;
    uint64_t reserved;
    uint64_t used;
    uint64_t committed;
    uint64_t page_size;
} arena;

static arena arena_state;
static uint64_t arena_reserve_size = ARENA_RESERVE_SIZE;

static bool align_up(uint64_t value, uint64_t alignment, uint64_t *result) {
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) {
        return false;
    }
    *result = (value + mask) & ~mask;
    return true;
}

static bool arena_initialize(void) {
    if (arena_state.base != NULL) {
        return true;
    }
    int page_size = getpagesize();
    if (page_size <= 0) {
        return false;
    }
    uint64_t reserved = 0;
    if (!align_up(arena_reserve_size, (uint64_t)page_size, &reserved)) {
        return false;
    }
    void *base = mmap(
        NULL,
        (size_t)reserved,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (base == MAP_FAILED) {
        return false;
    }
    arena_state.base = base;
    arena_state.reserved = reserved;
    arena_state.page_size = (uint64_t)page_size;
    return true;
}

static bool arena_commit(uint64_t needed) {
    uint64_t committed = 0;
    if (!align_up(needed, arena_state.page_size, &committed)) {
        return false;
    }
    if (committed > arena_state.reserved) {
        return false;
    }
    if (committed <= arena_state.committed) {
        return true;
    }
    uint64_t additional = committed - arena_state.committed;
    int changed = mprotect(
        arena_state.base + arena_state.committed,
        (size_t)additional,
        PROT_READ | PROT_WRITE
    );
    if (changed != 0) {
        return false;
    }
    arena_state.committed = committed;
    return true;
}

static void *arena_alloc(uint64_t size) {
    if (!arena_initialize()) {
        return NULL;
    }
    if (size == 0) {
        size = 1;
    }
    uint64_t offset = 0;
    if (!align_up(arena_state.used, ARENA_ALIGNMENT, &offset)
        || offset > UINT64_MAX - size) {
        return NULL;
    }
    uint64_t new_used = offset + size;
    if (!arena_commit(new_used)) {
        return NULL;
    }
    arena_state.used = new_used;
    return arena_state.base + offset;
}

static void *arena_alloc_zero(uint64_t count, uint64_t size) {
    if (size != 0 && count > UINT64_MAX / size) {
        return NULL;
    }
    uint64_t total = count * size;
    void *memory = arena_alloc(total);
    if (memory != NULL) {
        memset(memory, 0, (size_t)total);
    }
    return memory;
}

static void arena_release(void *memory) {
    (void)memory;
}

static uint64_t arena_committed_pages(void) {
    if (arena_state.page_size == 0) {
        return 0;
    }
    return arena_state.committed / arena_state.page_size;
}

static void arena_report(void) {
    fprintf(
        stderr,
        "qlmdv: arena used %lu bytes, committed %lu pages (%lu bytes)\n",
        (unsigned long)arena_state.used,
        (unsigned long)arena_committed_pages(),
        (unsigned long)arena_state.committed
    );
}

#ifdef QLMDV_TEST
static bool temporary_random_override;
static uint64_t temporary_random_value;
#endif

static uint64_t temporary_random(void) {
#ifdef QLMDV_TEST
    if (temporary_random_override) {
        return temporary_random_value;
    }
#endif
    uint64_t value = 0;
    uint32_t offset = 0;
    while (offset < sizeof(value)) {
        ssize_t count = getrandom((uint8_t *)&value + offset, sizeof(value) - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            value = ((uint64_t)(uint32_t)getpid() << 32) ^ (uintptr_t)&value;
            break;
        }
        offset += (uint32_t)count;
    }
    return value;
}

static int create_temporary(char *path) {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint32_t length = (uint32_t)strlen(path);
    if (length < 6 || strcmp(path + length - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    for (uint32_t attempt = 0; attempt < 128; attempt += 1) {
        uint64_t value = temporary_random() ^ attempt;
        for (uint32_t index = 0; index < 6; index += 1) {
            path[length - 6 + index] = alphabet[value % (sizeof(alphabet) - 1)];
            value /= sizeof(alphabet) - 1;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

typedef enum timezone_rule_kind {
    TIMEZONE_RULE_JULIAN,
    TIMEZONE_RULE_ORDINAL,
    TIMEZONE_RULE_MONTH
} timezone_rule_kind;

typedef struct timezone_rule {
    timezone_rule_kind kind;
    int32_t day;
    int32_t month;
    int32_t week;
    int32_t weekday;
    int32_t seconds;
} timezone_rule;

typedef struct posix_timezone {
    int32_t standard_offset;
    int32_t daylight_offset;
    timezone_rule daylight_start;
    timezone_rule daylight_end;
    bool has_daylight;
} posix_timezone;

typedef struct timezone_parser {
    const char *current;
    const char *end;
} timezone_parser;

typedef struct tzif_counts {
    uint32_t utc_count;
    uint32_t standard_count;
    uint32_t leap_count;
    uint32_t transition_count;
    uint32_t type_count;
    uint32_t abbreviation_count;
} tzif_counts;

static uint32_t read_be32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24
        | (uint32_t)bytes[1] << 16
        | (uint32_t)bytes[2] << 8
        | bytes[3];
}

static int64_t read_be64(const uint8_t *bytes) {
    uint64_t value = (uint64_t)read_be32(bytes) << 32 | read_be32(bytes + 4);
    return (int64_t)value;
}

static int64_t floor_divide(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    if (value < 0 && value % divisor != 0) {
        quotient -= 1;
    }
    return quotient;
}

static bool leap_year(int64_t year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int64_t days_from_civil(int64_t year, int32_t month, int32_t day) {
    year -= month <= 2;
    int64_t era = floor_divide(year, 400);
    uint32_t year_of_era = (uint32_t)(year - era * 400);
    int32_t adjusted_month = month;
    if (month > 2) {
        adjusted_month -= 3;
    } else {
        adjusted_month += 9;
    }
    uint32_t day_of_year = (153u * (uint32_t)adjusted_month + 2) / 5 + (uint32_t)day - 1;
    uint32_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static int64_t civil_year_from_days(int64_t days) {
    days += 719468;
    int64_t era = floor_divide(days, 146097);
    uint32_t day_of_era = (uint32_t)(days - era * 146097);
    uint32_t year_of_era = (
        day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096
    ) / 365;
    int64_t year = (int64_t)year_of_era + era * 400;
    uint32_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    uint32_t month_piece = (5 * day_of_year + 2) / 153;
    if (month_piece >= 10) {
        year += 1;
    }
    return year;
}

static int32_t weekday_from_days(int64_t days) {
    int64_t weekday = (days + 4) % 7;
    if (weekday < 0) {
        weekday += 7;
    }
    return (int32_t)weekday;
}

static bool parse_digits(
    timezone_parser *parser,
    int32_t minimum,
    int32_t maximum,
    int32_t *value
) {
    if (parser->current >= parser->end || *parser->current < '0' || *parser->current > '9') {
        return false;
    }
    int32_t parsed = 0;
    while (parser->current < parser->end
        && *parser->current >= '0' && *parser->current <= '9') {
        int32_t digit = *parser->current - '0';
        if (parsed > (maximum - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
        parser->current += 1;
    }
    if (parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool parse_timezone_name(timezone_parser *parser) {
    const char *start = parser->current;
    if (parser->current < parser->end && *parser->current == '<') {
        parser->current += 1;
        start = parser->current;
        while (parser->current < parser->end && *parser->current != '>') {
            parser->current += 1;
        }
        if (parser->current >= parser->end || parser->current - start < 3) {
            return false;
        }
        parser->current += 1;
        return true;
    }
    while (parser->current < parser->end
        && ((*parser->current >= 'A' && *parser->current <= 'Z')
            || (*parser->current >= 'a' && *parser->current <= 'z'))) {
        parser->current += 1;
    }
    return parser->current - start >= 3;
}

static bool parse_timezone_clock(
    timezone_parser *parser,
    int32_t maximum_hour,
    int32_t *seconds
) {
    int32_t sign = 1;
    if (parser->current < parser->end && (*parser->current == '+' || *parser->current == '-')) {
        if (*parser->current == '-') {
            sign = -1;
        }
        parser->current += 1;
    }
    int32_t hour = 0;
    if (!parse_digits(parser, 0, maximum_hour, &hour)) {
        return false;
    }
    int32_t minute = 0;
    int32_t second = 0;
    if (parser->current < parser->end && *parser->current == ':') {
        parser->current += 1;
        if (!parse_digits(parser, 0, 59, &minute)) {
            return false;
        }
        if (parser->current < parser->end && *parser->current == ':') {
            parser->current += 1;
            if (!parse_digits(parser, 0, 59, &second)) {
                return false;
            }
        }
    }
    *seconds = sign * (hour * 3600 + minute * 60 + second);
    return true;
}

static bool parse_timezone_rule(
    timezone_parser *parser,
    timezone_rule *rule
) {
    *rule = (timezone_rule){ .seconds = 2 * 3600 };
    if (parser->current < parser->end && *parser->current == 'M') {
        rule->kind = TIMEZONE_RULE_MONTH;
        parser->current += 1;
        if (!parse_digits(parser, 1, 12, &rule->month)
            || parser->current >= parser->end || *parser->current != '.') {
            return false;
        }
        parser->current += 1;
        if (!parse_digits(parser, 1, 5, &rule->week)
            || parser->current >= parser->end || *parser->current != '.') {
            return false;
        }
        parser->current += 1;
        if (!parse_digits(parser, 0, 6, &rule->weekday)) {
            return false;
        }
    } else {
        rule->kind = TIMEZONE_RULE_ORDINAL;
        if (parser->current < parser->end && *parser->current == 'J') {
            rule->kind = TIMEZONE_RULE_JULIAN;
            parser->current += 1;
            if (!parse_digits(parser, 1, 365, &rule->day)) {
                return false;
            }
        } else if (!parse_digits(parser, 0, 365, &rule->day)) {
            return false;
        }
    }
    if (parser->current < parser->end && *parser->current == '/') {
        parser->current += 1;
        if (!parse_timezone_clock(parser, 167, &rule->seconds)) {
            return false;
        }
    }
    return true;
}

static bool parse_posix_timezone(
    const char *text,
    uint32_t length,
    posix_timezone *timezone
) {
    timezone_parser parser = { .current = text, .end = text + length };
    *timezone = (posix_timezone){ 0 };
    if (!parse_timezone_name(&parser)) {
        return false;
    }
    int32_t standard_clock = 0;
    if (!parse_timezone_clock(&parser, 24, &standard_clock)) {
        return false;
    }
    timezone->standard_offset = -standard_clock;
    if (parser.current == parser.end) {
        return true;
    }
    if (!parse_timezone_name(&parser)) {
        return false;
    }
    timezone->has_daylight = true;
    timezone->daylight_offset = timezone->standard_offset + 3600;
    if (parser.current < parser.end && *parser.current != ',') {
        int32_t daylight_clock = 0;
        if (!parse_timezone_clock(&parser, 24, &daylight_clock)) {
            return false;
        }
        timezone->daylight_offset = -daylight_clock;
    }
    if (parser.current >= parser.end || *parser.current != ',') {
        return false;
    }
    parser.current += 1;
    if (!parse_timezone_rule(&parser, &timezone->daylight_start)
        || parser.current >= parser.end || *parser.current != ',') {
        return false;
    }
    parser.current += 1;
    return parse_timezone_rule(&parser, &timezone->daylight_end)
        && parser.current == parser.end;
}

static int32_t timezone_rule_day(
    int64_t year,
    const timezone_rule *rule
) {
    if (rule->kind == TIMEZONE_RULE_JULIAN) {
        int32_t day = rule->day - 1;
        if (leap_year(year) && rule->day >= 60) {
            day += 1;
        }
        return day;
    }
    if (rule->kind == TIMEZONE_RULE_ORDINAL) {
        return rule->day;
    }
    int64_t month_start = days_from_civil(year, rule->month, 1);
    int32_t first_weekday = weekday_from_days(month_start);
    int32_t day = (rule->weekday - first_weekday + 7) % 7 + (rule->week - 1) * 7;
    static const int32_t month_lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int32_t month_length = month_lengths[rule->month - 1];
    if (rule->month == 2 && leap_year(year)) {
        month_length += 1;
    }
    if (day >= month_length) {
        day -= 7;
    }
    return (int32_t)(month_start - days_from_civil(year, 1, 1)) + day;
}

static int64_t timezone_transition(
    int64_t year,
    const timezone_rule *rule,
    int32_t prior_offset
) {
    int64_t year_start = days_from_civil(year, 1, 1) * INT64_C(86400);
    return year_start + (int64_t)timezone_rule_day(year, rule) * 86400
        + rule->seconds - prior_offset;
}

static int32_t posix_timezone_offset(
    const posix_timezone *timezone,
    int64_t unix_time
) {
    if (!timezone->has_daylight) {
        return timezone->standard_offset;
    }
    int64_t local_days = floor_divide(unix_time + timezone->standard_offset, 86400);
    int64_t year = civil_year_from_days(local_days);
    int64_t start = timezone_transition(
        year,
        &timezone->daylight_start,
        timezone->standard_offset
    );
    int64_t end = timezone_transition(
        year,
        &timezone->daylight_end,
        timezone->daylight_offset
    );
    bool daylight = false;
    if (start < end) {
        daylight = unix_time >= start && unix_time < end;
    } else {
        daylight = unix_time >= start || unix_time < end;
    }
    if (daylight) {
        return timezone->daylight_offset;
    }
    return timezone->standard_offset;
}

static bool parse_tzif_counts(
    const uint8_t *header,
    uint32_t available,
    tzif_counts *counts
) {
    if (available < 44 || memcmp(header, "TZif", 4) != 0) {
        return false;
    }
    counts->utc_count = read_be32(header + 20);
    counts->standard_count = read_be32(header + 24);
    counts->leap_count = read_be32(header + 28);
    counts->transition_count = read_be32(header + 32);
    counts->type_count = read_be32(header + 36);
    counts->abbreviation_count = read_be32(header + 40);
    return counts->type_count > 0 && counts->type_count <= 256;
}

static bool tzif_block_size(
    const tzif_counts *counts,
    uint32_t time_size,
    uint64_t *size
) {
    uint64_t total = (uint64_t)counts->transition_count * time_size;
    total += counts->transition_count;
    total += (uint64_t)counts->type_count * 6;
    total += counts->abbreviation_count;
    total += (uint64_t)counts->leap_count * (time_size + 4);
    total += counts->standard_count;
    total += counts->utc_count;
    *size = total;
    return total <= TIMEZONE_FILE_MAX;
}

static bool timezone_offset_from_tzif(
    const uint8_t *bytes,
    uint32_t byte_count,
    int64_t unix_time,
    int32_t *offset
) {
    tzif_counts counts;
    if (!parse_tzif_counts(bytes, byte_count, &counts)) {
        return false;
    }
    uint32_t time_size = 4;
    uint64_t block_size = 0;
    if (!tzif_block_size(&counts, time_size, &block_size)
        || block_size > byte_count - 44) {
        return false;
    }
    uint64_t block_offset = 44;
    if (bytes[4] == '2' || bytes[4] == '3' || bytes[4] == '4') {
        uint64_t second_header = block_offset + block_size;
        if (second_header > byte_count || byte_count - second_header < 44
            || !parse_tzif_counts(bytes + second_header, byte_count - (uint32_t)second_header, &counts)) {
            return false;
        }
        time_size = 8;
        if (!tzif_block_size(&counts, time_size, &block_size)
            || block_size > byte_count - second_header - 44) {
            return false;
        }
        block_offset = second_header + 44;
    }
    const uint8_t *transitions = bytes + block_offset;
    const uint8_t *transition_types = transitions + (uint64_t)counts.transition_count * time_size;
    const uint8_t *types = transition_types + counts.transition_count;
    uint32_t type_index = 0;
    bool found_transition = false;
    for (uint32_t i = 0; i < counts.transition_count; i += 1) {
        int64_t transition = 0;
        if (time_size == 8) {
            transition = read_be64(transitions + (uint64_t)i * 8);
        } else {
            transition = (int32_t)read_be32(transitions + (uint64_t)i * 4);
        }
        if (transition > unix_time) {
            break;
        }
        if (transition_types[i] >= counts.type_count) {
            return false;
        }
        type_index = transition_types[i];
        found_transition = true;
    }
    if (!found_transition) {
        for (uint32_t i = 0; i < counts.type_count; i += 1) {
            if (types[i * 6 + 4] == 0) {
                type_index = i;
                break;
            }
        }
    }
    uint64_t block_end = block_offset + block_size;
    bool after_last = counts.transition_count == 0;
    if (counts.transition_count > 0) {
        const uint8_t *last = transitions + (uint64_t)(counts.transition_count - 1) * time_size;
        int64_t last_time = 0;
        if (time_size == 8) {
            last_time = read_be64(last);
        } else {
            last_time = (int32_t)read_be32(last);
        }
        after_last = unix_time > last_time;
    }
    if (after_last && block_end < byte_count && bytes[block_end] == '\n') {
        uint64_t footer_end = block_end + 1;
        while (footer_end < byte_count && bytes[footer_end] != '\n') {
            footer_end += 1;
        }
        if (footer_end > block_end + 1 && footer_end < byte_count) {
            posix_timezone timezone;
            if (parse_posix_timezone(
                    (const char *)bytes + block_end + 1,
                    (uint32_t)(footer_end - block_end - 1),
                    &timezone
                )) {
                *offset = posix_timezone_offset(&timezone, unix_time);
                return true;
            }
        }
    }
    *offset = (int32_t)read_be32(types + type_index * 6);
    return true;
}

static char cached_timezone_path[TIMEZONE_PATH_SIZE];
static uint8_t *cached_timezone_bytes;
static uint32_t cached_timezone_byte_count;

static uint8_t *read_timezone_file(const char *path, uint32_t *byte_count) {
    if (cached_timezone_bytes != NULL && strcmp(path, cached_timezone_path) == 0) {
        *byte_count = cached_timezone_byte_count;
        return cached_timezone_bytes;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    struct stat status;
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_size <= 0 || (uint64_t)status.st_size > TIMEZONE_FILE_MAX) {
        (void)close(fd);
        return NULL;
    }
    uint32_t size = (uint32_t)status.st_size;
    uint8_t *bytes = arena_alloc(size);
    if (bytes == NULL) {
        (void)close(fd);
        return NULL;
    }
    uint32_t done = 0;
    while (done < size) {
        ssize_t count = read(fd, bytes + done, size - done);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            (void)close(fd);
            return NULL;
        }
        done += (uint32_t)count;
    }
    if (close(fd) != 0) {
        return NULL;
    }
    uint32_t path_length = (uint32_t)strlen(path);
    if (path_length < sizeof cached_timezone_path) {
        memcpy(cached_timezone_path, path, path_length + 1);
        cached_timezone_bytes = bytes;
        cached_timezone_byte_count = size;
    }
    *byte_count = size;
    return bytes;
}

static int32_t local_utc_offset(int64_t unix_time) {
    const char *zone = getenv("TZ");
    if (zone != NULL && zone[0] == '\0') {
        return 0;
    }
    const char *path = "/etc/localtime";
    char zone_path[TIMEZONE_PATH_SIZE];
    if (zone != NULL) {
        if (zone[0] == ':') {
            zone += 1;
        }
        if (zone[0] == '/') {
            path = zone;
        } else {
            int length = snprintf(zone_path, sizeof(zone_path), "/usr/share/zoneinfo/%s", zone);
            if (length > 0 && (uint32_t)length < sizeof(zone_path)) {
                path = zone_path;
            }
        }
    }
    uint32_t byte_count = 0;
    uint8_t *bytes = read_timezone_file(path, &byte_count);
    int32_t offset = 0;
    if (bytes != NULL && timezone_offset_from_tzif(bytes, byte_count, unix_time, &offset)) {
        return offset;
    }
    if (zone != NULL) {
        posix_timezone timezone;
        uint32_t length = (uint32_t)strlen(zone);
        if (parse_posix_timezone(zone, length, &timezone)) {
            return posix_timezone_offset(&timezone, unix_time);
        }
    }
    return 0;
}

#ifdef QLMDV_TEST
static bool arena_test_reset(uint64_t reserve_size) {
    if (arena_state.base != NULL && munmap(arena_state.base, (size_t)arena_state.reserved) != 0) {
        return false;
    }
    arena_state = (arena){ 0 };
    arena_reserve_size = reserve_size;
    cached_timezone_path[0] = '\0';
    cached_timezone_bytes = NULL;
    cached_timezone_byte_count = 0;
    return true;
}
#endif

static int errno_or_eio(int error_number) {
    if (error_number != 0) {
        return error_number;
    }
    return EIO;
}

static ssize_t read_fd_retry(int fd, void *buffer, uint32_t length) {
    for (;;) {
        ssize_t result = read(fd, buffer, length);
        if (result >= 0 || errno != EINTR) {
            return result;
        }
    }
}

static inline uint8_t ascii_lower(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (uint8_t)(ch + 'a' - 'A');
    }
    return ch;
}

static inline bool ascii_equal_ignore_case(char *left, char *right) {
    for (;;) {
        uint8_t left_ch = ascii_lower((uint8_t)*left);
        uint8_t right_ch = ascii_lower((uint8_t)*right);
        if (left_ch != right_ch) {
            return false;
        }
        if (left_ch == '\0') {
            return true;
        }
        left += 1;
        right += 1;
    }
}

static inline int32_t int32_from_uint32_bits(uint32_t value) {
    if (value <= (uint32_t)INT32_MAX) {
        return (int32_t)value;
    }
    return INT32_MIN + (int32_t)(value - UINT32_C(0x80000000));
}

static int32_t unix_time_zone_offset(time_t unix_time) {
    return local_utc_offset((int64_t)unix_time);
}

static int32_t unix_to_qdos_time(time_t value) {
    int32_t time_zone_offset = unix_time_zone_offset(value);
    int64_t unix_time = (int64_t)value;
    int64_t min_unix_time = -(int64_t)qdos_unix_epoch_delta_seconds - time_zone_offset;
    int64_t max_unix_time = (int64_t)UINT32_MAX - qdos_unix_epoch_delta_seconds - time_zone_offset;
    if (unix_time <= min_unix_time) {
        return 0;
    }
    if (unix_time >= max_unix_time) {
        return int32_from_uint32_bits(UINT32_MAX);
    }
    int64_t qdos_time = unix_time + qdos_unix_epoch_delta_seconds + time_zone_offset;
    return int32_from_uint32_bits((uint32_t)qdos_time);
}

static time_t qdos_to_unix_time(int32_t value) {
    uint32_t qdos_time = (uint32_t)value;
    int64_t qdos_local_time = (int64_t)qdos_time - qdos_unix_epoch_delta_seconds;
    int64_t unix_time = qdos_local_time;
    for (int32_t i = 0; i < 3; i += 1) {
        int64_t adjusted_time = qdos_local_time - local_utc_offset(unix_time);
        if (adjusted_time == unix_time) {
            break;
        }
        unix_time = adjusted_time;
    }
    if (sizeof(time_t) == 4) {
        if (unix_time < INT32_MIN) {
            unix_time = INT32_MIN;
        }
        if (unix_time > INT32_MAX) {
            unix_time = INT32_MAX;
        }
    }
    return (time_t)unix_time;
}
