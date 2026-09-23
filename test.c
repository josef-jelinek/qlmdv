#define QLMDV_TEST
#include "src/runtime.c"
#include "src/qlay.c"
#include "src/image.c"
#include "src/cli.c"

static int32_t test_runtime_allocator(void) {
    int32_t failures = 0;
    if (!arena_test_reset(4096)) {
        puts("runtime: could not configure the test arena");
        return 1;
    }
    uint8_t *bytes = arena_alloc_zero(32, 1);
    if (bytes == NULL || ((uintptr_t)bytes & 15u) != 0) {
        puts("runtime: arena allocation failed or was not aligned");
        failures += 1;
    } else {
        for (uint32_t i = 0; i < 32; i += 1) {
            if (bytes[i] != 0) {
                puts("runtime: zeroed arena allocation was not clear");
                failures += 1;
                break;
            }
        }
    }
    if (arena_alloc(4096) != NULL) {
        puts("runtime: arena exhaustion was not reported");
        failures += 1;
    }
    if (!arena_test_reset(ARENA_RESERVE_SIZE)) {
        puts("runtime: could not restore the production arena size");
        failures += 1;
    }
    return failures;
}

static int32_t test_timezone_runtime(void) {
    char saved_timezone[max_config_string];
    char *current_timezone = getenv("TZ");
    bool had_timezone = current_timezone != NULL;
    if (had_timezone) {
        (void)snprintf(saved_timezone, sizeof saved_timezone, "%s", current_timezone);
    }

    int32_t failures = 0;
    if (setenv("TZ", "UTC0", 1) != 0
        || unix_to_qdos_time(0) != qdos_unix_epoch_delta_seconds
        || qdos_to_unix_time(qdos_unix_epoch_delta_seconds) != 0) {
        puts("runtime: UTC QDOS time conversion failed");
        failures += 1;
    }
    if (setenv("TZ", "EST5", 1) != 0
        || unix_to_qdos_time(0) != qdos_unix_epoch_delta_seconds - 5 * 3600
        || qdos_to_unix_time(qdos_unix_epoch_delta_seconds - 5 * 3600) != 0) {
        puts("runtime: offset QDOS time conversion failed");
        failures += 1;
    }

    int restore_status = 0;
    if (had_timezone) {
        restore_status = setenv("TZ", saved_timezone, 1);
    } else {
        restore_status = unsetenv("TZ");
    }
    if (restore_status != 0) {
        puts("runtime: could not restore TZ after the time conversion test");
        failures += 1;
    }
    return failures;
}

static int32_t test_temporary_file_runtime(void) {
    char first_path[] = "/tmp/qlmdv-runtime-temp.XXXXXX";
    char second_path[] = "/tmp/qlmdv-runtime-temp.XXXXXX";
    temporary_random_override = true;
    temporary_random_value = 0;
    int first = create_temporary(first_path);
    int second = create_temporary(second_path);
    temporary_random_override = false;

    int32_t failures = 0;
    if (first < 0 || second < 0 || strcmp(first_path, second_path) == 0) {
        puts("runtime: temporary file collision handling failed");
        failures += 1;
    }
    if (first >= 0 && close(first) != 0) {
        puts("runtime: could not close the first temporary file");
        failures += 1;
    }
    if (second >= 0 && close(second) != 0) {
        puts("runtime: could not close the second temporary file");
        failures += 1;
    }
    if (first >= 0) {
        (void)unlink(first_path);
    }
    if (second >= 0) {
        (void)unlink(second_path);
    }
    return failures;
}

static int32_t test_qlay_format_contract(void) {
    int32_t failures = 0;
    uint8_t checksum_bytes[] = { 1, 2, 3 };
    uint16_t checksum = qlay_checksum(checksum_bytes, sizeof checksum_bytes);
    uint8_t stored_checksum[2];
    qlay_write_checksum(stored_checksum, checksum);
    if (checksum != 0x0F15 || stored_checksum[0] != 0x15 || stored_checksum[1] != 0x0F) {
        puts("qlmdv: additive checksum or little-endian storage is incorrect");
        failures += 1;
    }

    uint8_t medium_name[10] = { 'T', 'E', 'S', 'T', ' ', ' ', ' ', ' ', ' ', ' ' };
    uint8_t *bytes = NULL;
    uint32_t byte_count = 0;
    char error[512];
    if (!qlmdv_create_image_bytes(200, medium_name, 0x1234, &bytes, &byte_count, error, sizeof error)) {
        printf("qlmdv: could not create test image: %s\n", error);
        return failures + 1;
    }
    if (byte_count != 200 * qlay_sector_size) {
        puts("qlmdv: created geometry has the wrong byte count");
        failures += 1;
    }
    if (
        bytes[qlay_sector_header_offset + 1] != 0 ||
        bytes[qlay_sector_size + qlay_sector_header_offset + 1] != 199
    ) {
        puts("qlmdv: created image does not use map-first descending physical sector order");
        failures += 1;
    }

    qlmdv_image image;
    if (!qlmdv_image_decode(&image, bytes, byte_count, error, sizeof error)) {
        printf("qlmdv: could not decode created image: %s\n", error);
        arena_release(bytes);
        return failures + 1;
    }
    if (
        image.issue_count != 0 ||
        image.sector_count != 200 ||
        image.good_sector_count != 200 ||
        image.bad_sector_count != 0 ||
        image.free_sector_count != 198 ||
        image.used_sector_count != 2 ||
        !image.directory_available
    ) {
        puts("qlmdv: created allocation map or directory did not decode cleanly");
        failures += 1;
    }
    qlmdv_image_clear(&image);

    uint8_t *checksum_damage = arena_alloc(byte_count);
    if (checksum_damage == NULL) {
        arena_release(bytes);
        return failures + 1;
    }
    memcpy(checksum_damage, bytes, byte_count);
    uint32_t logical_two_physical = 198;
    checksum_damage[logical_two_physical * qlay_sector_size + qlay_data_offset] ^= 0x80;
    if (!qlmdv_image_decode(&image, checksum_damage, byte_count, error, sizeof error)) {
        puts("qlmdv: checksum-damaged image could not be decoded");
        failures += 1;
    } else {
        if (
            image.checksum_issue_count != 1 ||
            image.structural_issue_count != 0 ||
            qlmdv_image_can_modify(&image, false, error, sizeof error) ||
            !qlmdv_image_can_modify(&image, true, error, sizeof error)
        ) {
            puts("qlmdv: checksum-only damage did not obey forced mutation rules");
            failures += 1;
        }
        qlmdv_image_clear(&image);
    }
    arena_release(checksum_damage);

    uint8_t *bad_sector = arena_alloc(byte_count);
    if (bad_sector == NULL) {
        arena_release(bytes);
        return failures + 1;
    }
    memcpy(bad_sector, bytes, byte_count);
    uint8_t *map_sector = bad_sector;
    map_sector[qlay_data_offset + 199 * 2] = qlay_bad_file_id;
    map_sector[qlay_data_offset + 199 * 2 + 1] = 0;
    qlay_write_checksum(
        map_sector + qlay_data_checksum_offset,
        qlay_checksum(map_sector + qlay_data_offset, qlay_data_size)
    );
    memset(bad_sector + qlay_sector_size, 0, qlay_sector_size);
    if (!qlmdv_image_decode(&image, bad_sector, byte_count, error, sizeof error)) {
        puts("qlmdv: bad-sector image could not be decoded");
        failures += 1;
    } else {
        if (image.issue_count != 0 || image.bad_sector_count != 1 || image.good_sector_count != 199) {
            puts("qlmdv: bad-sector map and unformatted record were not interpreted consistently");
            failures += 1;
        }
        qlmdv_image_clear(&image);
    }
    arena_release(bad_sector);

    uint8_t *duplicate_sector = arena_alloc(byte_count);
    if (duplicate_sector == NULL) {
        arena_release(bytes);
        return failures + 1;
    }
    memcpy(duplicate_sector, bytes, byte_count);
    uint8_t *physical_one = duplicate_sector + qlay_sector_size;
    physical_one[qlay_sector_header_offset + 1] = 198;
    qlay_write_checksum(
        physical_one + qlay_sector_header_checksum_offset,
        qlay_checksum(physical_one + qlay_sector_header_offset, qlay_sector_header_size)
    );
    if (!qlmdv_image_decode(&image, duplicate_sector, byte_count, error, sizeof error)) {
        puts("qlmdv: malformed duplicate-sector image could not be decoded for diagnostics");
        failures += 1;
    } else {
        if (image.structural_issue_count == 0 || qlmdv_image_can_modify(&image, true, error, sizeof error)) {
            puts("qlmdv: ambiguous duplicate sectors were not rejected for mutation");
            failures += 1;
        }
        qlmdv_image_clear(&image);
    }
    arena_release(duplicate_sector);

    uint8_t *maximum_block = arena_alloc(byte_count);
    if (maximum_block == NULL) {
        arena_release(bytes);
        return failures + 1;
    }
    memcpy(maximum_block, bytes, byte_count);
    map_sector = maximum_block;
    map_sector[qlay_data_offset + 2 * 2] = qlmdv_max_files;
    map_sector[qlay_data_offset + 2 * 2 + 1] = UINT8_MAX;
    qlay_write_checksum(
        map_sector + qlay_data_checksum_offset,
        qlay_checksum(map_sector + qlay_data_offset, qlay_data_size)
    );
    uint8_t *logical_two = maximum_block + logical_two_physical * qlay_sector_size;
    logical_two[qlay_block_header_offset] = qlmdv_max_files;
    logical_two[qlay_block_header_offset + 1] = UINT8_MAX;
    qlay_write_checksum(
        logical_two + qlay_block_header_checksum_offset,
        qlay_checksum(logical_two + qlay_block_header_offset, qlay_block_header_size)
    );
    if (!qlmdv_image_decode(&image, maximum_block, byte_count, error, sizeof error)) {
        puts("qlmdv: maximum block-number image could not be decoded for diagnostics");
        failures += 1;
    } else {
        bool found_block_issue = false;
        for (uint32_t i = 0; i < image.issue_count; i += 1) {
            if (strstr(image.issues[i].text, "out-of-range file block 255") != NULL) {
                found_block_issue = true;
                break;
            }
        }
        if (!found_block_issue || qlmdv_image_can_modify(&image, true, error, sizeof error)) {
            puts("qlmdv: block number 255 was not rejected before allocation tracking");
            failures += 1;
        }
        qlmdv_image_clear(&image);
    }
    arena_release(maximum_block);

    uint8_t *adjacent_block_alias = arena_alloc(byte_count);
    if (adjacent_block_alias == NULL) {
        arena_release(bytes);
        return failures + 1;
    }
    memcpy(adjacent_block_alias, bytes, byte_count);
    map_sector = adjacent_block_alias;
    map_sector[qlay_data_offset + 2 * 2] = qlmdv_max_files - 1;
    map_sector[qlay_data_offset + 2 * 2 + 1] = UINT8_MAX;
    map_sector[qlay_data_offset + 3 * 2] = qlmdv_max_files;
    map_sector[qlay_data_offset + 3 * 2 + 1] = 0;
    qlay_write_checksum(
        map_sector + qlay_data_checksum_offset,
        qlay_checksum(map_sector + qlay_data_offset, qlay_data_size)
    );
    logical_two = adjacent_block_alias + logical_two_physical * qlay_sector_size;
    logical_two[qlay_block_header_offset] = qlmdv_max_files - 1;
    logical_two[qlay_block_header_offset + 1] = UINT8_MAX;
    qlay_write_checksum(
        logical_two + qlay_block_header_checksum_offset,
        qlay_checksum(logical_two + qlay_block_header_offset, qlay_block_header_size)
    );
    uint32_t logical_three_physical = 197;
    uint8_t *logical_three = adjacent_block_alias + logical_three_physical * qlay_sector_size;
    logical_three[qlay_block_header_offset] = qlmdv_max_files;
    logical_three[qlay_block_header_offset + 1] = 0;
    qlay_write_checksum(
        logical_three + qlay_block_header_checksum_offset,
        qlay_checksum(logical_three + qlay_block_header_offset, qlay_block_header_size)
    );
    if (!qlmdv_image_decode(&image, adjacent_block_alias, byte_count, error, sizeof error)) {
        puts("qlmdv: adjacent block-row alias image could not be decoded for diagnostics");
        failures += 1;
    } else {
        bool found_out_of_range_block = false;
        bool found_false_duplicate = false;
        for (uint32_t i = 0; i < image.issue_count; i += 1) {
            if (strstr(image.issues[i].text, "out-of-range file block 255") != NULL) {
                found_out_of_range_block = true;
            }
            if (strstr(image.issues[i].text, "file 240 block 0 is allocated more than once") != NULL) {
                found_false_duplicate = true;
            }
        }
        if (!found_out_of_range_block || found_false_duplicate) {
            puts("qlmdv: invalid block 255 aliased the next file's block-zero allocation state");
            failures += 1;
        }
        qlmdv_image_clear(&image);
    }
    arena_release(adjacent_block_alias);

    qlmdv_image full_directory = { 0 };
    full_directory.bytes = arena_alloc_zero(1, qlay_sector_size);
    if (full_directory.bytes == NULL) {
        arena_release(bytes);
        return failures + 1;
    }
    full_directory.byte_count = qlay_sector_size;
    full_directory.sector_count = 1;
    full_directory.logical_to_physical[0] = 0;
    full_directory.map[0] = 0;
    full_directory.map[1] = 0;
    full_directory.file_count = qlmdv_max_files;
    uint8_t *directory = full_directory.bytes + qlay_data_offset;
    qlmdv_write_be32(directory, 2 * qlmdv_qdos_header_size);
    uint8_t *last_header = directory + qlmdv_qdos_header_size;
    qlmdv_write_be32(last_header, qlmdv_qdos_header_size);
    qlmdv_write_be16(last_header + qlmdv_qdos_name_length_offset, 1);
    last_header[qlmdv_qdos_name_offset] = 'A';
    uint16_t full_directory_block_counts[qlmdv_max_files + 1] = { 0 };
    full_directory_block_counts[0] = 1;
    uint8_t (*full_directory_block_locations)[qlmdv_max_files + 1][qlay_max_sectors] =
        arena_alloc_zero(1, sizeof *full_directory_block_locations);
    if (full_directory_block_locations == NULL) {
        qlmdv_image_clear(&full_directory);
        arena_release(bytes);
        return failures + 1;
    }
    (*full_directory_block_locations)[0][0] = 1;
    qlmdv_parse_directory(&full_directory, full_directory_block_counts, *full_directory_block_locations);
    arena_release(full_directory_block_locations);
    bool found_file_capacity_issue = false;
    for (uint32_t i = 0; i < full_directory.issue_count; i += 1) {
        if (strstr(full_directory.issues[i].text, "too many active file entries") != NULL) {
            found_file_capacity_issue = true;
            break;
        }
    }
    if (!found_file_capacity_issue || full_directory.file_count != qlmdv_max_files) {
        puts("qlmdv: full file table did not reject another active directory entry");
        failures += 1;
    }
    qlmdv_image_clear(&full_directory);

    if (qlmdv_image_decode(&image, bytes, byte_count - 1, error, sizeof error)) {
        puts("qlmdv: truncated image was accepted");
        qlmdv_image_clear(&image);
        failures += 1;
    }
    arena_release(bytes);
    return failures;
}

static int32_t test_qlmdv_random_id_uses_full_pid(void) {
    uint16_t low_pid = qlmdv_random_id_fallback(
        UINT64_C(0x12345678),
        987654321,
        UINT64_C(0x1234),
        (uintptr_t)UINT64_C(0xABCDEF00)
    );
    uint16_t high_pid = qlmdv_random_id_fallback(
        UINT64_C(0x12345678),
        987654321,
        UINT64_C(0x11235),
        (uintptr_t)UINT64_C(0xABCDEF00)
    );
    if (low_pid == high_pid) {
        puts("qlmdv: random ID fallback ignored significant high PID bits");
        return 1;
    }
    return 0;
}

static int32_t test_qlmdv_manifest_unescape_rejects_short_escape(void) {
    char decoded[16];
    char error[128];
    char bare_percent[] = { '%', '\0' };
    char one_digit[] = { '%', 'A', '\0' };
    if (
        qlmdv_manifest_unescape(bare_percent, decoded, sizeof decoded, error, sizeof error) ||
        qlmdv_manifest_unescape(one_digit, decoded, sizeof decoded, error, sizeof error)
    ) {
        puts("qlmdv: short metadata percent escape was accepted");
        return 1;
    }
    return 0;
}

static int32_t test_qlmdv_manifest_unescape_rejects_zero_capacity(void) {
    char encoded[] = { 'A', '\0' };
    char decoded[] = { 'X' };
    char error[128];
    if (
        qlmdv_manifest_unescape(encoded, decoded, 0, error, sizeof error) ||
        decoded[0] != 'X'
    ) {
        puts("qlmdv: metadata unescape wrote to a zero-capacity output buffer");
        return 1;
    }
    return 0;
}

static int32_t test_qlmdv_type_descriptions(void) {
    typedef struct {
        uint8_t type;
        char *description;
    } type_description_test;

    type_description_test tests[] = {
        { 0, "default" },
        { 1, "executable" },
        { 2, "SROFF relocatable object" },
        { 3, "unknown" },
        { 254, "unknown" },
        { 255, "directory" }
    };
    for (uint32_t i = 0; i < sizeof tests / sizeof *tests; i += 1) {
        if (strcmp(qlmdv_qdos_type_description(tests[i].type), tests[i].description) != 0) {
            printf("qlmdv: type %" PRIu8 " has the wrong description\n", tests[i].type);
            return 1;
        }
    }
    return 0;
}

static int32_t test_qlmdv_verbose_option(void) {
    char *argv[] = { "qlmdv", "create", "--verbose", "image.mdv" };
    qlmdv_options options;
    char error[512] = { 0 };
    int image_index = 4;
    if (
        !qlmdv_parse_options(4, argv, &image_index, &options, error, sizeof error) ||
        !options.verbose ||
        image_index != 3
    ) {
        puts("qlmdv: --verbose was not parsed as a common command option");
        return 1;
    }
    qlmdv_command commands[] = {
        qlmdv_command_create,
        qlmdv_command_list,
        qlmdv_command_extract,
        qlmdv_command_add,
        qlmdv_command_remove,
        qlmdv_command_replace,
        qlmdv_command_reorder,
        qlmdv_command_inspect
    };
    for (uint32_t i = 0; i < sizeof commands / sizeof commands[0]; i += 1) {
        memset(error, 0, sizeof error);
        if (!qlmdv_options_valid_for_command(commands[i], &options, error, sizeof error)) {
            puts("qlmdv: --verbose was rejected for a command");
            return 1;
        }
    }
    char *invalid_argv[] = { "qlmdv", "create", "--verbose=yes", "image.mdv" };
    memset(error, 0, sizeof error);
    image_index = 4;
    if (qlmdv_parse_options(4, invalid_argv, &image_index, &options, error, sizeof error)) {
        puts("qlmdv: --verbose accepted an unexpected value");
        return 1;
    }
    return 0;
}

static bool qlmdv_test_write_file(char *path, const uint8_t *bytes, uint32_t byte_count) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        return false;
    }
    int write_error = 0;
    bool ok = qlmdv_write_all_fd(fd, bytes, byte_count, &write_error);
    if (close(fd) != 0) {
        ok = false;
    }
    return ok;
}

static bool qlmdv_test_file_equals(char *path, const uint8_t *expected, uint32_t expected_len) {
    uint8_t *bytes = NULL;
    uint32_t byte_count = 0;
    char error[512];
    if (!qlmdv_read_all_image(path, &bytes, &byte_count, error, sizeof error)) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
            (void)close(fd);
            return false;
        }
        byte_count = (uint32_t)st.st_size;
        bytes = arena_alloc(byte_count);
        if (bytes == NULL && byte_count != 0) {
            (void)close(fd);
            return false;
        }
        uint32_t done = 0;
        while (done < byte_count) {
            ssize_t count = read_fd_retry(fd, bytes + done, byte_count - done);
            if (count <= 0) {
                arena_release(bytes);
                (void)close(fd);
                return false;
            }
            done += (uint32_t)count;
        }
        (void)close(fd);
    }
    bool equal = byte_count == expected_len && memcmp(bytes, expected, expected_len) == 0;
    arena_release(bytes);
    return equal;
}

static int qlmdv_test_cli_quiet(int argc, char **argv) {
    fflush(stdout);
    fflush(stderr);
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (saved_stdout < 0 || saved_stderr < 0 || null_fd < 0) {
        if (saved_stdout >= 0) {
            (void)close(saved_stdout);
        }
        if (saved_stderr >= 0) {
            (void)close(saved_stderr);
        }
        if (null_fd >= 0) {
            (void)close(null_fd);
        }
        return cli_main(argc, argv);
    }
    (void)dup2(null_fd, STDOUT_FILENO);
    (void)dup2(null_fd, STDERR_FILENO);
    int status = cli_main(argc, argv);
    fflush(stdout);
    fflush(stderr);
    (void)dup2(saved_stdout, STDOUT_FILENO);
    (void)dup2(saved_stderr, STDERR_FILENO);
    (void)close(null_fd);
    (void)close(saved_stdout);
    (void)close(saved_stderr);
    return status;
}

static int qlmdv_test_cli_capture_stdout(
    int argc,
    char **argv,
    char *output,
    uint32_t output_size
) {
    if (output_size == 0) {
        return 2;
    }
    output[0] = '\0';
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    FILE *capture = tmpfile();
    if (saved_stdout < 0 || capture == NULL) {
        if (saved_stdout >= 0) {
            (void)close(saved_stdout);
        }
        if (capture != NULL) {
            (void)fclose(capture);
        }
        return 2;
    }
    int capture_fd = fileno(capture);
    if (capture_fd < 0 || dup2(capture_fd, STDOUT_FILENO) < 0) {
        (void)close(saved_stdout);
        (void)fclose(capture);
        return 2;
    }
    int status = cli_main(argc, argv);
    fflush(stdout);
    (void)dup2(saved_stdout, STDOUT_FILENO);
    (void)close(saved_stdout);
    if (lseek(capture_fd, 0, SEEK_SET) >= 0) {
        ssize_t count = read_fd_retry(capture_fd, output, output_size - 1);
        if (count > 0) {
            output[count] = '\0';
        }
    }
    (void)fclose(capture);
    return status;
}

static bool qlmdv_test_join_path(
    char *output,
    uint32_t output_size,
    char *prefix,
    char *suffix
) {
    uint32_t prefix_len = (uint32_t)strlen(prefix);
    uint32_t suffix_len = (uint32_t)strlen(suffix);
    if (prefix_len >= output_size || suffix_len >= output_size - prefix_len) {
        return false;
    }
    memcpy(output, prefix, prefix_len);
    memcpy(output + prefix_len, suffix, suffix_len + 1);
    return true;
}

static int32_t test_qlmdv_workflow(void) {
    char temp_template[] = "/tmp/qlmdv-test-XXXXXX";
    char *temp_dir = mkdtemp(temp_template);
    if (temp_dir == NULL) {
        puts("qlmdv: could not create workflow test directory");
        return 1;
    }

    char alpha_path[max_config_string];
    char boot_path[max_config_string];
    char gamma_path[max_config_string];
    char replacement_path[max_config_string];
    char large_path[max_config_string];
    char image_path[max_config_string];
    char roundtrip_path[max_config_string];
    char capacity_path[max_config_string];
    char select_dir[max_config_string];
    char all_dir[max_config_string];
    char manifest_path[max_config_string];
    (void)snprintf(alpha_path, sizeof alpha_path, "%s/alpha-host", temp_dir);
    (void)snprintf(boot_path, sizeof boot_path, "%s/BOOT", temp_dir);
    (void)snprintf(gamma_path, sizeof gamma_path, "%s/Gamma", temp_dir);
    (void)snprintf(replacement_path, sizeof replacement_path, "%s/replacement", temp_dir);
    (void)snprintf(large_path, sizeof large_path, "%s/large", temp_dir);
    (void)snprintf(image_path, sizeof image_path, "%s/work.mdv", temp_dir);
    (void)snprintf(roundtrip_path, sizeof roundtrip_path, "%s/roundtrip.mdv", temp_dir);
    (void)snprintf(capacity_path, sizeof capacity_path, "%s/capacity.mdv", temp_dir);
    (void)snprintf(select_dir, sizeof select_dir, "%s/select", temp_dir);
    (void)snprintf(all_dir, sizeof all_dir, "%s/all", temp_dir);
    (void)snprintf(manifest_path, sizeof manifest_path, "%s/metadata.txt", temp_dir);

    uint8_t alpha_data[] = "alpha-one";
    uint8_t boot_data[] = "boot-data";
    uint8_t gamma_data[] = "gamma-data";
    uint8_t replacement_data[] = "alpha-two";
    int32_t failures = 0;
    if (
        !qlmdv_test_write_file(alpha_path, alpha_data, sizeof alpha_data - 1) ||
        !qlmdv_test_write_file(boot_path, boot_data, sizeof boot_data - 1) ||
        !qlmdv_test_write_file(gamma_path, gamma_data, sizeof gamma_data - 1) ||
        !qlmdv_test_write_file(replacement_path, replacement_data, sizeof replacement_data - 1) ||
        mkdir(select_dir, 0777) != 0 ||
        mkdir(all_dir, 0777) != 0
    ) {
        puts("qlmdv: could not prepare workflow test inputs");
        failures += 1;
        goto cleanup;
    }

    char alpha_operand[max_config_string];
    char replacement_operand[max_config_string];
    if (
        !qlmdv_test_join_path(alpha_operand, sizeof alpha_operand, alpha_path, "=Alpha") ||
        !qlmdv_test_join_path(replacement_operand, sizeof replacement_operand, replacement_path, "=Alpha")
    ) {
        puts("qlmdv: workflow import operand is too long");
        failures += 1;
        goto cleanup;
    }
    char *create_argv[] = {
        "qlmdv", "create", "--sectors", "200", "--random-id", "4660",
        image_path, alpha_operand, boot_path
    };
    if (qlmdv_test_cli_quiet(9, create_argv) != 0) {
        puts("qlmdv: create workflow failed");
        failures += 1;
        goto cleanup;
    }

    char *duplicate_argv[] = { "qlmdv", "add", image_path, alpha_operand };
    uint8_t *before_duplicate = NULL;
    uint32_t before_duplicate_len = 0;
    char error[512];
    bool duplicate_snapshot_ready =
        qlmdv_read_all_image(image_path, &before_duplicate, &before_duplicate_len, error, sizeof error);
    if (!duplicate_snapshot_ready || qlmdv_test_cli_quiet(4, duplicate_argv) != 1) {
        puts("qlmdv: duplicate add was not rejected");
        failures += 1;
    }
    if (
        duplicate_snapshot_ready &&
        !qlmdv_test_file_equals(image_path, before_duplicate, before_duplicate_len)
    ) {
        puts("qlmdv: rejected duplicate add changed the original image");
        failures += 1;
    }
    arena_release(before_duplicate);

    char *add_argv[] = { "qlmdv", "a", image_path, gamma_path };
    char *replace_argv[] = { "qlmdv", "u", image_path, replacement_operand };
    char *remove_argv[] = { "qlmdv", "d", image_path, "boot" };
    char *reorder_argv[] = { "qlmdv", "m", image_path, "gamma" };
    if (
        qlmdv_test_cli_quiet(4, add_argv) != 0 ||
        qlmdv_test_cli_quiet(4, replace_argv) != 0 ||
        qlmdv_test_cli_quiet(4, remove_argv) != 0 ||
        qlmdv_test_cli_quiet(4, reorder_argv) != 0
    ) {
        puts("qlmdv: add/replace/remove/reorder workflow failed");
        failures += 1;
        goto cleanup;
    }

    qlmdv_image image;
    if (!qlmdv_image_load(&image, image_path, error, sizeof error)) {
        puts("qlmdv: could not reload workflow image");
        failures += 1;
        goto cleanup;
    }
    if (
        image.issue_count != 0 ||
        image.file_count != 2 ||
        !qlmdv_bytes_equal_ignore_case(image.files[0].name, image.files[0].name_len, (uint8_t *)"Gamma", 5) ||
        !qlmdv_bytes_equal_ignore_case(image.files[1].name, image.files[1].name_len, (uint8_t *)"Alpha", 5) ||
        image.files[1].data_len != sizeof replacement_data - 1 ||
        memcmp(image.files[1].data, replacement_data, sizeof replacement_data - 1) != 0
    ) {
        puts("qlmdv: workflow image contents or ordering are incorrect");
        failures += 1;
    }
    image.files[1].header[qlmdv_qdos_access_key_offset] = 0xA5;
    image.files[1].header[qlmdv_qdos_type_offset] = 1;
    qlmdv_write_be32(image.files[1].header + qlmdv_qdos_data_space_offset, 0x12345678);
    qlmdv_write_be32(image.files[1].header + qlmdv_qdos_type_info_offset, 0xCAFEBABE);
    qlmdv_write_be32(image.files[1].header + qlmdv_qdos_update_offset, 0x89ABCDEF);
    qlmdv_write_be16(image.files[1].header + qlmdv_qdos_version_offset, 7);
    qlmdv_write_be16(image.files[1].header + qlmdv_qdos_reserved_offset, 0xBEEF);
    qlmdv_write_be32(image.files[1].header + qlmdv_qdos_backup_offset, 0x76543210);
    uint8_t *metadata_image = NULL;
    uint32_t metadata_image_len = 0;
    if (
        !qlmdv_image_rebuild(&image, &metadata_image, &metadata_image_len, error, sizeof error) ||
        !qlmdv_write_transaction(
            image_path,
            metadata_image,
            metadata_image_len,
            false,
            false,
            error,
            sizeof error
        )
    ) {
        puts("qlmdv: could not prepare metadata workflow image");
        failures += 1;
    }
    arena_release(metadata_image);
    qlmdv_image_clear(&image);

    char list_output[2048];
    char inspect_output[4096];
    char expected_alpha_list[128];
    char expected_gamma_list[128];
    (void)snprintf(
        expected_alpha_list,
        sizeof expected_alpha_list,
        "%4u %10u  Alpha",
        1,
        (uint32_t)(sizeof replacement_data - 1)
    );
    (void)snprintf(
        expected_gamma_list,
        sizeof expected_gamma_list,
        "%4u %10u  Gamma",
        0,
        (uint32_t)(sizeof gamma_data - 1)
    );
    char *list_argv[] = { "qlmdv", "list", image_path };
    if (
        qlmdv_test_cli_capture_stdout(3, list_argv, list_output, sizeof list_output) != 0 ||
        strstr(list_output, "TYPE     LENGTH  NAME\n") == NULL ||
        strstr(list_output, expected_alpha_list) == NULL ||
        strstr(list_output, expected_gamma_list) == NULL
    ) {
        puts("qlmdv: list output omitted file type or length");
        failures += 1;
    }

    char *inspect_argv[] = { "qlmdv", "inspect", image_path };
    int inspect_status = qlmdv_test_cli_capture_stdout(3, inspect_argv, inspect_output, sizeof inspect_output);
    char expected_gamma_inspect[] =
        "  file ID 1:\n"
        "    00: [0000 004A] total length: 74, data length: 10\n"
        "    04: [00] access key\n"
        "    05: [00] type: 0 default\n";
    char expected_alpha_inspect[] =
        "  file ID 2:\n"
        "    00: [0000 0049] total length: 73, data length: 9\n"
        "    04: [A5] access key\n"
        "    05: [01] type: 1 executable\n"
        "    06: [1234 5678] data space: 305419896\n"
        "    0A: [CAFE BABE] type info: 3405691582\n"
        "    0E: [0005] name length: 5\n"
        "    10: [416C 7068 6100 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000]"
        " name: \"Alpha\"\n"
        "    34: [89AB CDEF] update date (QDOS time)\n"
        "    38: [0007] version: 7\n"
        "    3A: [BEEF] reserved\n"
        "    3C: [7654 3210] backup date (QDOS time)\n";
    if (
        inspect_status != 0 ||
        strstr(inspect_output, "files (2):\n") == NULL ||
        strstr(inspect_output, expected_gamma_inspect) == NULL ||
        strstr(inspect_output, expected_alpha_inspect) == NULL
    ) {
        puts("qlmdv: compact inspect output has incorrect per-file header data");
        failures += 1;
    }

    char *select_argv[] = { "qlmdv", "x", "--directory", select_dir, image_path, "alpha" };
    char *extract_argv[] = {
        "qlmdv", "extract", "--directory", all_dir, "--metadata", manifest_path, image_path
    };
    if (qlmdv_test_cli_quiet(6, select_argv) != 0 || qlmdv_test_cli_quiet(7, extract_argv) != 0) {
        puts("qlmdv: selective or complete extraction failed");
        failures += 1;
        goto cleanup;
    }
    char select_alpha[max_config_string];
    char select_gamma[max_config_string];
    char all_alpha[max_config_string];
    char all_gamma[max_config_string];
    if (
        !qlmdv_test_join_path(select_alpha, sizeof select_alpha, select_dir, "/Alpha") ||
        !qlmdv_test_join_path(select_gamma, sizeof select_gamma, select_dir, "/Gamma") ||
        !qlmdv_test_join_path(all_alpha, sizeof all_alpha, all_dir, "/Alpha") ||
        !qlmdv_test_join_path(all_gamma, sizeof all_gamma, all_dir, "/Gamma")
    ) {
        puts("qlmdv: workflow extraction path is too long");
        failures += 1;
        goto cleanup;
    }
    if (
        !qlmdv_test_file_equals(select_alpha, replacement_data, sizeof replacement_data - 1) ||
        access(select_gamma, F_OK) == 0 ||
        !qlmdv_test_file_equals(all_alpha, replacement_data, sizeof replacement_data - 1) ||
        !qlmdv_test_file_equals(all_gamma, gamma_data, sizeof gamma_data - 1)
    ) {
        puts("qlmdv: extracted file selection or contents are incorrect");
        failures += 1;
    }

    uint8_t original_manifest[] = "ORIGINAL METADATA\n";
    char *force_metadata_argv[] = {
        "qlmdv", "x", "--directory", all_dir, "--metadata", manifest_path, "--force", image_path
    };
    bool original_manifest_ready = qlmdv_test_write_file(
        manifest_path,
        original_manifest,
        sizeof original_manifest - 1
    );
    qlmdv_test_transaction_write_limit = 8;
    qlmdv_test_transaction_write_was_limited = false;
    int forced_metadata_status = qlmdv_test_cli_quiet(8, force_metadata_argv);
    qlmdv_test_transaction_write_limit = -1;
    if (
        !original_manifest_ready ||
        !qlmdv_test_transaction_write_was_limited ||
        forced_metadata_status != 1 ||
        !qlmdv_test_file_equals(manifest_path, original_manifest, sizeof original_manifest - 1)
    ) {
        puts("qlmdv: failed forced metadata write did not preserve the original manifest");
        failures += 1;
    }
    if (qlmdv_test_cli_quiet(8, force_metadata_argv) != 0) {
        puts("qlmdv: successful forced metadata replacement failed");
        failures += 1;
    }

    char *collision_argv[] = { "qlmdv", "x", "--directory", all_dir, image_path };
    char *force_extract_argv[] = { "qlmdv", "x", "--directory", all_dir, "--force", image_path };
    uint8_t changed[] = "changed";
    if (
        qlmdv_test_cli_quiet(5, collision_argv) != 1 ||
        !qlmdv_test_write_file(all_alpha, changed, sizeof changed - 1) ||
        qlmdv_test_cli_quiet(6, force_extract_argv) != 0 ||
        !qlmdv_test_file_equals(all_alpha, replacement_data, sizeof replacement_data - 1)
    ) {
        puts("qlmdv: forced and non-forced extraction collision handling failed");
        failures += 1;
    }

    char *roundtrip_argv[] = {
        "qlmdv", "c", "--metadata", manifest_path, roundtrip_path, all_alpha, all_gamma
    };
    bool roundtrip_created = qlmdv_test_cli_quiet(7, roundtrip_argv) == 0;
    bool roundtrip_loaded = false;
    if (!roundtrip_created) {
        puts("qlmdv: metadata round-trip create failed");
        failures += 1;
    }
    if (roundtrip_created) {
        roundtrip_loaded = qlmdv_image_load(&image, roundtrip_path, error, sizeof error);
    }
    if (roundtrip_created && !roundtrip_loaded) {
        puts("qlmdv: metadata round-trip image could not be loaded");
        failures += 1;
    }
    if (roundtrip_loaded) {
        int32_t alpha_index = qlmdv_find_file(&image, "Alpha");
        if (
            image.issue_count != 0 ||
            alpha_index < 0 ||
            image.files[alpha_index].header[qlmdv_qdos_access_key_offset] != 0xA5 ||
            image.files[alpha_index].header[qlmdv_qdos_type_offset] != 1 ||
            qlmdv_read_be32(image.files[alpha_index].header + qlmdv_qdos_data_space_offset) != 0x12345678 ||
            qlmdv_read_be32(image.files[alpha_index].header + qlmdv_qdos_type_info_offset) != 0xCAFEBABE ||
            qlmdv_read_be16(image.files[alpha_index].header + qlmdv_qdos_version_offset) != 7 ||
            qlmdv_read_be16(image.files[alpha_index].header + qlmdv_qdos_reserved_offset) != 0xBEEF ||
            qlmdv_read_be32(image.files[alpha_index].header + qlmdv_qdos_backup_offset) != 0x76543210
        ) {
            puts("qlmdv: metadata manifest did not preserve QDOS header fields");
            failures += 1;
        }
        qlmdv_image_clear(&image);
    }

    uint8_t *corrupt_bytes = NULL;
    uint32_t corrupt_len = 0;
    if (!qlmdv_read_all_image(image_path, &corrupt_bytes, &corrupt_len, error, sizeof error)) {
        puts("qlmdv: could not read image for atomic validation test");
        failures += 1;
    } else {
        corrupt_bytes[2 * qlay_sector_size + qlay_data_offset] ^= 1;
        if (!qlmdv_write_transaction(
            image_path,
            corrupt_bytes,
            corrupt_len,
            false,
            false,
            error,
            sizeof error
        )) {
            puts("qlmdv: could not prepare checksum damage test");
            failures += 1;
        } else {
            char *reject_mutation_argv[] = { "qlmdv", "d", image_path, "Alpha" };
            char *force_mutation_argv[] = { "qlmdv", "d", "--force", image_path, "Alpha" };
            if (
                qlmdv_test_cli_quiet(4, reject_mutation_argv) != 1 ||
                !qlmdv_test_file_equals(image_path, corrupt_bytes, corrupt_len) ||
                qlmdv_test_cli_quiet(5, force_mutation_argv) != 0
            ) {
                puts("qlmdv: checksum validation did not keep rejected mutation atomic");
                failures += 1;
            }
        }
    }
    arena_release(corrupt_bytes);

    uint8_t large_block[qlay_data_size] = { 0xA5 };
    int large_fd = open(large_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    bool large_ok = large_fd >= 0;
    int large_error = 0;
    for (uint32_t block = 0; large_ok && block < 200; block += 1) {
        large_ok = qlmdv_write_all_fd(large_fd, large_block, sizeof large_block, &large_error);
    }
    if (large_fd >= 0 && close(large_fd) != 0) {
        large_ok = false;
    }
    char *capacity_argv[] = { "qlmdv", "c", "--sectors", "200", capacity_path, large_path };
    if (!large_ok || qlmdv_test_cli_quiet(6, capacity_argv) != 1 || access(capacity_path, F_OK) == 0) {
        puts("qlmdv: capacity exhaustion was not rejected before image creation");
        failures += 1;
    }

    uint8_t *roundtrip_before = NULL;
    uint32_t roundtrip_before_len = 0;
    char *create_collision_argv[] = { "qlmdv", "c", roundtrip_path };
    char *force_create_argv[] = { "qlmdv", "c", "--force", roundtrip_path };
    if (
        !qlmdv_read_all_image(roundtrip_path, &roundtrip_before, &roundtrip_before_len, error, sizeof error) ||
        qlmdv_test_cli_quiet(3, create_collision_argv) != 1 ||
        !qlmdv_test_file_equals(roundtrip_path, roundtrip_before, roundtrip_before_len) ||
        qlmdv_test_cli_quiet(4, force_create_argv) != 0
    ) {
        puts("qlmdv: create collision handling or forced replacement failed");
        failures += 1;
    }
    arena_release(roundtrip_before);

cleanup:
    (void)unlink(alpha_path);
    (void)unlink(boot_path);
    (void)unlink(gamma_path);
    (void)unlink(replacement_path);
    (void)unlink(large_path);
    (void)unlink(image_path);
    (void)unlink(roundtrip_path);
    (void)unlink(capacity_path);
    (void)unlink(manifest_path);
    char cleanup_path[max_config_string];
    (void)qlmdv_test_join_path(cleanup_path, sizeof cleanup_path, select_dir, "/Alpha");
    (void)unlink(cleanup_path);
    (void)qlmdv_test_join_path(cleanup_path, sizeof cleanup_path, select_dir, "/Gamma");
    (void)unlink(cleanup_path);
    (void)qlmdv_test_join_path(cleanup_path, sizeof cleanup_path, all_dir, "/Alpha");
    (void)unlink(cleanup_path);
    (void)qlmdv_test_join_path(cleanup_path, sizeof cleanup_path, all_dir, "/Gamma");
    (void)unlink(cleanup_path);
    (void)rmdir(select_dir);
    (void)rmdir(all_dir);
    (void)rmdir(temp_dir);
    return failures;
}

int main(void) {
    int32_t failures = 0;

    failures += test_runtime_allocator();
    failures += test_timezone_runtime();
    failures += test_temporary_file_runtime();
    failures += test_qlay_format_contract();
    failures += test_qlmdv_random_id_uses_full_pid();
    failures += test_qlmdv_manifest_unescape_rejects_short_escape();
    failures += test_qlmdv_manifest_unescape_rejects_zero_capacity();
    failures += test_qlmdv_type_descriptions();
    failures += test_qlmdv_verbose_option();
    failures += test_qlmdv_workflow();

    uint32_t release_string_prefix_len = (uint32_t)(sizeof "qlmdv " - 1);
    if (strncmp(qlmdv_release_string, "qlmdv ", release_string_prefix_len) != 0) {
        printf("version: release string has unexpected prefix: %s\n", qlmdv_release_string);
        failures += 1;
    }
    if (qlmdv_release_string[release_string_prefix_len] == '\0') {
        puts("version: release string is missing a version suffix");
        failures += 1;
    }

    if (failures != 0) {
        printf("%d test failure(s)\n", failures);
        return 1;
    }

    puts("all tests passed");
    return 0;
}
