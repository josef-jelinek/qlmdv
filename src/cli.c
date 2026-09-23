typedef enum {
    qlmdv_command_none,
    qlmdv_command_create,
    qlmdv_command_list,
    qlmdv_command_extract,
    qlmdv_command_add,
    qlmdv_command_remove,
    qlmdv_command_replace,
    qlmdv_command_reorder,
    qlmdv_command_inspect
} qlmdv_command;

typedef struct {
    char *directory;
    char *medium_name;
    char *metadata_path;
    uint32_t sectors;
    uint32_t random_id;
    bool force;
    bool verbose;
    bool sectors_set;
    bool random_id_set;
    bool help;
} qlmdv_options;

typedef struct {
    char filename[max_config_string];
    uint8_t header[qlmdv_qdos_header_size];
} qlmdv_metadata_entry;

typedef struct {
    qlmdv_metadata_entry entries[qlmdv_max_files];
    uint32_t count;
} qlmdv_metadata;

enum {
    qlmdv_diagnostic_path_length = 240
};

static char qlmdv_release_string[] = "qlmdv 0.1";

static void qlmdv_print_usage(FILE *stream) {
    fputs(
        "usage: qlmdv COMMAND [OPTIONS] IMAGE [OPERANDS...]\n"
        "\n"
        "commands:\n"
        "  c / create    create an image and optionally populate it\n"
        "  t / list      list files\n"
        "  x / extract   extract all or selected files\n"
        "  a / add       append files that do not already exist\n"
        "  d / remove    remove selected files\n"
        "  u / replace   replace selected existing files\n"
        "  m / reorder   put selected files first in the supplied order\n"
        "  i / inspect   validate and describe an image\n"
        "\n"
        "global options:\n"
        "  --help       show help\n"
        "  --version    show version\n"
        "\n"
        "common command option:\n"
        "  --verbose    show operation and arena statistics\n"
        "\n"
        "run 'qlmdv COMMAND --help' for command options\n",
        stream
    );
}

static void qlmdv_print_command_help(qlmdv_command command) {
    switch (command) {
    case qlmdv_command_create:
        puts(
            "usage: qlmdv create [--verbose] [--force] [--sectors N] [--medium-name NAME]\n"
            "                    [--random-id N] [--metadata FILE] IMAGE [HOST[=QDOS_NAME] ...]"
        );
        break;
    case qlmdv_command_list:
        puts("usage: qlmdv list [--verbose] IMAGE");
        break;
    case qlmdv_command_extract:
        puts(
            "usage: qlmdv extract [--verbose] [--directory DIR] [--force] [--metadata FILE]\n"
            "                     IMAGE [QDOS_NAME ...]"
        );
        break;
    case qlmdv_command_add:
        puts("usage: qlmdv add [--verbose] [--force] [--metadata FILE] IMAGE HOST[=QDOS_NAME] ...");
        break;
    case qlmdv_command_remove:
        puts("usage: qlmdv remove [--verbose] [--force] IMAGE QDOS_NAME ...");
        break;
    case qlmdv_command_replace:
        puts("usage: qlmdv replace [--verbose] [--force] [--metadata FILE] IMAGE HOST[=QDOS_NAME] ...");
        break;
    case qlmdv_command_reorder:
        puts("usage: qlmdv reorder [--verbose] [--force] IMAGE QDOS_NAME ...");
        break;
    case qlmdv_command_inspect:
        puts("usage: qlmdv inspect [--verbose] IMAGE");
        break;
    default:
        qlmdv_print_usage(stdout);
        break;
    }
}

static qlmdv_command qlmdv_parse_command(char *text) {
    if (ascii_equal_ignore_case(text, "create") || ascii_equal_ignore_case(text, "c")) {
        return qlmdv_command_create;
    }
    if (ascii_equal_ignore_case(text, "list") || ascii_equal_ignore_case(text, "t")) {
        return qlmdv_command_list;
    }
    if (ascii_equal_ignore_case(text, "extract") || ascii_equal_ignore_case(text, "x")) {
        return qlmdv_command_extract;
    }
    if (ascii_equal_ignore_case(text, "add") || ascii_equal_ignore_case(text, "a")) {
        return qlmdv_command_add;
    }
    if (ascii_equal_ignore_case(text, "remove") || ascii_equal_ignore_case(text, "d")) {
        return qlmdv_command_remove;
    }
    if (ascii_equal_ignore_case(text, "replace") || ascii_equal_ignore_case(text, "u")) {
        return qlmdv_command_replace;
    }
    if (ascii_equal_ignore_case(text, "reorder") || ascii_equal_ignore_case(text, "m")) {
        return qlmdv_command_reorder;
    }
    if (ascii_equal_ignore_case(text, "inspect") || ascii_equal_ignore_case(text, "i")) {
        return qlmdv_command_inspect;
    }
    return qlmdv_command_none;
}

static bool qlmdv_parse_u32(char *text, uint32_t max_value, uint32_t *value) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > max_value) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static char *qlmdv_option_value(
    int argc,
    char **argv,
    int *index,
    char *argument,
    char *name,
    bool *matched
) {
    uint32_t name_len = (uint32_t)strlen(name);
    *matched = false;
    if (strcmp(argument, name) == 0) {
        *matched = true;
        if (*index + 1 >= argc) {
            return NULL;
        }
        *index += 1;
        return argv[*index];
    }
    if (strncmp(argument, name, name_len) == 0 && argument[name_len] == '=') {
        *matched = true;
        if (argument[name_len + 1] == '\0') {
            return NULL;
        }
        return argument + name_len + 1;
    }
    return NULL;
}

static bool qlmdv_parse_options(
    int argc,
    char **argv,
    int *image_index,
    qlmdv_options *options,
    char *error,
    uint32_t error_size
) {
    *options = (qlmdv_options){ .directory = ".", .sectors = 255 };
    for (int index = 2; index < argc; index += 1) {
        char *argument = argv[index];
        if (strcmp(argument, "--") == 0) {
            index += 1;
            if (index >= argc) {
                (void)snprintf(error, error_size, "missing image path after --");
                return false;
            }
            *image_index = index;
            return true;
        }
        if (argument[0] != '-' || argument[1] == '\0') {
            *image_index = index;
            return true;
        }
        if (strcmp(argument, "--help") == 0) {
            options->help = true;
            continue;
        }
        if (strcmp(argument, "--force") == 0) {
            options->force = true;
            continue;
        }
        if (strcmp(argument, "--verbose") == 0) {
            options->verbose = true;
            continue;
        }

        bool matched = false;
        char *value = qlmdv_option_value(argc, argv, &index, argument, "--directory", &matched);
        if (matched) {
            if (value == NULL) {
                (void)snprintf(error, error_size, "--directory requires a value");
                return false;
            }
            options->directory = value;
            continue;
        }
        value = qlmdv_option_value(argc, argv, &index, argument, "--metadata", &matched);
        if (matched) {
            if (value == NULL) {
                (void)snprintf(error, error_size, "--metadata requires a value");
                return false;
            }
            options->metadata_path = value;
            continue;
        }
        value = qlmdv_option_value(argc, argv, &index, argument, "--medium-name", &matched);
        if (matched) {
            if (value == NULL) {
                (void)snprintf(error, error_size, "--medium-name requires a value");
                return false;
            }
            options->medium_name = value;
            continue;
        }
        value = qlmdv_option_value(argc, argv, &index, argument, "--sectors", &matched);
        if (matched) {
            if (value == NULL || !qlmdv_parse_u32(value, qlay_max_sectors, &options->sectors)) {
                (void)snprintf(error, error_size, "--sectors requires an integer from 200 to 255");
                return false;
            }
            options->sectors_set = true;
            continue;
        }
        value = qlmdv_option_value(argc, argv, &index, argument, "--random-id", &matched);
        if (matched) {
            if (value == NULL || !qlmdv_parse_u32(value, UINT16_MAX, &options->random_id)) {
                (void)snprintf(error, error_size, "--random-id requires an integer from 0 to 65535");
                return false;
            }
            options->random_id_set = true;
            continue;
        }

        (void)snprintf(error, error_size, "unknown option: %s", argument);
        return false;
    }
    *image_index = argc;
    return true;
}

static bool qlmdv_options_valid_for_command(
    qlmdv_command command,
    qlmdv_options *options,
    char *error,
    uint32_t error_size
) {
    if (
        options->force &&
        command != qlmdv_command_create &&
        command != qlmdv_command_extract &&
        command != qlmdv_command_add &&
        command != qlmdv_command_remove &&
        command != qlmdv_command_replace &&
        command != qlmdv_command_reorder
    ) {
        (void)snprintf(error, error_size, "--force is not valid for this command");
        return false;
    }
    if (options->directory != NULL && strcmp(options->directory, ".") != 0 && command != qlmdv_command_extract) {
        (void)snprintf(error, error_size, "--directory is only valid for extract");
        return false;
    }
    if (
        options->metadata_path != NULL &&
        command != qlmdv_command_create &&
        command != qlmdv_command_extract &&
        command != qlmdv_command_add &&
        command != qlmdv_command_replace
    ) {
        (void)snprintf(error, error_size, "--metadata is only valid for create, add, replace, and extract");
        return false;
    }
    if (
        (options->medium_name != NULL || options->sectors_set || options->random_id_set) &&
        command != qlmdv_command_create
    ) {
        (void)snprintf(error, error_size, "medium and geometry options are only valid for create");
        return false;
    }
    if (command == qlmdv_command_create && (options->sectors < 200 || options->sectors > 255)) {
        (void)snprintf(error, error_size, "--sectors requires an integer from 200 to 255");
        return false;
    }
    if (command == qlmdv_command_create && options->medium_name != NULL) {
        uint32_t length = (uint32_t)strlen(options->medium_name);
        if (length == 0 || length > 10) {
            (void)snprintf(error, error_size, "--medium-name must contain 1-10 bytes");
            return false;
        }
    }
    return true;
}

static int32_t qlmdv_hex_value(uint8_t ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = ascii_lower(ch);
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static bool qlmdv_manifest_unescape(
    char *encoded,
    char *decoded,
    uint32_t decoded_size,
    char *error,
    uint32_t error_size
) {
    if (decoded_size == 0) {
        (void)snprintf(error, error_size, "metadata filename output buffer has no space");
        return false;
    }
    uint32_t output = 0;
    for (uint32_t input = 0; encoded[input] != '\0'; input += 1) {
        uint8_t value = (uint8_t)encoded[input];
        if (value == '%') {
            if (encoded[input + 1] == '\0' || encoded[input + 2] == '\0') {
                (void)snprintf(error, error_size, "invalid percent escape in metadata filename");
                return false;
            }
            int32_t high = qlmdv_hex_value((uint8_t)encoded[input + 1]);
            int32_t low = qlmdv_hex_value((uint8_t)encoded[input + 2]);
            if (high < 0 || low < 0) {
                (void)snprintf(error, error_size, "invalid percent escape in metadata filename");
                return false;
            }
            value = (uint8_t)((uint32_t)high * 16 + (uint32_t)low);
            input += 2;
            if (value == 0) {
                (void)snprintf(error, error_size, "metadata filename contains a NUL byte");
                return false;
            }
        }
        if (output >= decoded_size - 1) {
            (void)snprintf(error, error_size, "metadata filename is too long");
            return false;
        }
        decoded[output] = (char)value;
        output += 1;
    }
    decoded[output] = '\0';
    return true;
}

static bool qlmdv_manifest_safe_byte(uint8_t ch) {
    return (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.';
}

static bool qlmdv_manifest_escape(char *input, char *encoded, uint32_t encoded_size) {
    static char hex[] = "0123456789ABCDEF";
    uint32_t output = 0;
    for (uint32_t i = 0; input[i] != '\0'; i += 1) {
        uint8_t ch = (uint8_t)input[i];
        uint32_t needed = 3;
        if (qlmdv_manifest_safe_byte(ch)) {
            needed = 1;
        }
        if (output + needed >= encoded_size) {
            return false;
        }
        if (needed == 1) {
            encoded[output] = (char)ch;
            output += 1;
            continue;
        }
        encoded[output] = '%';
        encoded[output + 1] = hex[ch >> 4];
        encoded[output + 2] = hex[ch & 0x0F];
        output += 3;
    }
    encoded[output] = '\0';
    return true;
}

static bool qlmdv_metadata_load(
    char *path,
    qlmdv_metadata *metadata,
    char *error,
    uint32_t error_size
) {
    memset(metadata, 0, sizeof *metadata);
    if (path == NULL) {
        return true;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        (void)snprintf(error, error_size, "could not open metadata %s: %s", path, strerror(errno_or_eio(errno)));
        return false;
    }

    enum {
        qlmdv_metadata_line_size = max_config_string * 3 + 256
    };
    char line[qlmdv_metadata_line_size];
    if (fgets(line, sizeof line, file) == NULL || strcmp(line, "QLMDV-METADATA 1\n") != 0) {
        (void)snprintf(error, error_size, "%s does not start with QLMDV-METADATA 1", path);
        (void)fclose(file);
        return false;
    }

    uint32_t line_number = 1;
    for (;;) {
        errno = 0;
        if (fgets(line, sizeof line, file) == NULL) {
            if (errno != 0) {
                (void)snprintf(error, error_size, "could not read metadata %s: %s", path, strerror(errno));
                (void)fclose(file);
                return false;
            }
            break;
        }
        line_number += 1;
        uint32_t line_len = (uint32_t)strlen(line);
        if (line_len == 0 || line[line_len - 1] != '\n') {
            (void)snprintf(
                error,
                error_size,
                "%s:%" PRIu32 ": metadata line is too long or incomplete",
                path,
                line_number
            );
            (void)fclose(file);
            return false;
        }
        line[line_len - 1] = '\0';
        if (line_len >= 2 && line[line_len - 2] == '\r') {
            line[line_len - 2] = '\0';
        }
        char *tab = strchr(line, '\t');
        if (tab == NULL || strchr(tab + 1, '\t') != NULL || strlen(tab + 1) != qlmdv_qdos_header_size * 2) {
            (void)snprintf(error, error_size, "%s:%" PRIu32 ": invalid metadata record", path, line_number);
            (void)fclose(file);
            return false;
        }
        *tab = '\0';
        if (metadata->count >= qlmdv_max_files) {
            (void)snprintf(error, error_size, "%s contains more than %d metadata records", path, qlmdv_max_files);
            (void)fclose(file);
            return false;
        }

        qlmdv_metadata_entry *entry = &metadata->entries[metadata->count];
        if (!qlmdv_manifest_unescape(line, entry->filename, sizeof entry->filename, error, error_size)) {
            (void)fclose(file);
            return false;
        }
        for (uint32_t i = 0; i < qlmdv_qdos_header_size; i += 1) {
            int32_t high = qlmdv_hex_value((uint8_t)tab[1 + i * 2]);
            int32_t low = qlmdv_hex_value((uint8_t)tab[2 + i * 2]);
            if (high < 0 || low < 0) {
                (void)snprintf(error, error_size, "%s:%" PRIu32 ": invalid header hexadecimal", path, line_number);
                (void)fclose(file);
                return false;
            }
            entry->header[i] = (uint8_t)((uint32_t)high * 16 + (uint32_t)low);
        }
        for (uint32_t i = 0; i < metadata->count; i += 1) {
            if (strcmp(metadata->entries[i].filename, entry->filename) == 0) {
                (void)snprintf(error, error_size, "%s:%" PRIu32 ": duplicate metadata filename", path, line_number);
                (void)fclose(file);
                return false;
            }
        }
        metadata->count += 1;
    }
    if (fclose(file) != 0) {
        (void)snprintf(error, error_size, "could not close metadata %s: %s", path, strerror(errno_or_eio(errno)));
        return false;
    }
    return true;
}

static qlmdv_metadata_entry *qlmdv_metadata_find(qlmdv_metadata *metadata, char *filename) {
    for (uint32_t i = 0; i < metadata->count; i += 1) {
        if (strcmp(metadata->entries[i].filename, filename) == 0) {
            return &metadata->entries[i];
        }
    }
    return NULL;
}

static char *qlmdv_basename(char *path) {
    char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return path;
    }
    return slash + 1;
}

static bool qlmdv_read_host_file(
    char *path,
    qlmdv_file *imported,
    struct stat *st,
    char *error,
    uint32_t error_size
) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        (void)snprintf(
            error,
            error_size,
            "could not open %.*s: %s",
            qlmdv_diagnostic_path_length,
            path,
            strerror(errno_or_eio(errno))
        );
        return false;
    }
    if (fstat(fd, st) != 0) {
        (void)snprintf(
            error,
            error_size,
            "could not stat %.*s: %s",
            qlmdv_diagnostic_path_length,
            path,
            strerror(errno_or_eio(errno))
        );
        (void)close(fd);
        return false;
    }
    uint64_t maximum_file_bytes = (uint64_t)qlay_max_sectors * qlay_data_size;
    if (!S_ISREG(st->st_mode) || st->st_size < 0 || (uint64_t)st->st_size > maximum_file_bytes) {
        (void)snprintf(
            error,
            error_size,
            "%.*s is not a supported regular file",
            qlmdv_diagnostic_path_length,
            path
        );
        (void)close(fd);
        return false;
    }
    imported->data_len = (uint32_t)st->st_size;
    if (imported->data_len != 0) {
        imported->data = arena_alloc(imported->data_len);
        if (imported->data == NULL) {
            (void)snprintf(
                error,
                error_size,
                "out of memory while importing %.*s",
                qlmdv_diagnostic_path_length,
                path
            );
            (void)close(fd);
            return false;
        }
        uint32_t done = 0;
        while (done < imported->data_len) {
            ssize_t count = read_fd_retry(fd, imported->data + done, imported->data_len - done);
            if (count <= 0) {
                (void)snprintf(
                    error,
                    error_size,
                    "could not read %.*s: %s",
                    qlmdv_diagnostic_path_length,
                    path,
                    strerror(errno_or_eio(errno))
                );
                qlmdv_file_clear(imported);
                (void)close(fd);
                return false;
            }
            done += (uint32_t)count;
        }
    }
    if (close(fd) != 0) {
        (void)snprintf(
            error,
            error_size,
            "could not close %.*s: %s",
            qlmdv_diagnostic_path_length,
            path,
            strerror(errno_or_eio(errno))
        );
        qlmdv_file_clear(imported);
        return false;
    }
    return true;
}

static bool qlmdv_import_operand(
    char *operand,
    qlmdv_metadata *metadata,
    qlmdv_file *imported,
    char *error,
    uint32_t error_size
) {
    *imported = (qlmdv_file){ 0 };
    uint32_t operand_len = (uint32_t)strlen(operand);
    if (operand_len >= max_config_string) {
        (void)snprintf(error, error_size, "import operand is too long");
        return false;
    }

    char host_path[max_config_string];
    char *mapped_name = NULL;
    char *equals = strchr(operand, '=');
    if (equals == operand) {
        (void)snprintf(error, error_size, "HOST=QDOS_NAME mapping has an empty host path");
        return false;
    }
    if (equals != NULL) {
        uint32_t host_len = (uint32_t)(equals - operand);
        memcpy(host_path, operand, host_len);
        host_path[host_len] = '\0';
        mapped_name = equals + 1;
        if (mapped_name[0] == '\0') {
            (void)snprintf(error, error_size, "HOST=QDOS_NAME mapping has an empty QDOS name");
            return false;
        }
    } else {
        memcpy(host_path, operand, operand_len + 1);
    }

    char *basename = qlmdv_basename(host_path);
    if (basename[0] == '\0') {
        (void)snprintf(
            error,
            error_size,
            "host path has an empty basename: %.*s",
            qlmdv_diagnostic_path_length,
            host_path
        );
        return false;
    }
    qlmdv_metadata_entry *metadata_entry = qlmdv_metadata_find(metadata, basename);
    struct stat st;
    if (!qlmdv_read_host_file(host_path, imported, &st, error, error_size)) {
        return false;
    }

    if (metadata_entry != NULL) {
        memcpy(imported->header, metadata_entry->header, sizeof imported->header);
    } else {
        memset(imported->header, 0, sizeof imported->header);
        imported->header[qlmdv_qdos_type_offset] = 0;
        qlmdv_write_be32(
            imported->header + qlmdv_qdos_update_offset,
            (uint32_t)unix_to_qdos_time(st.st_mtime)
        );
    }

    const uint8_t *qdos_name = (uint8_t *)mapped_name;
    uint32_t qdos_name_len = 0;
    if (mapped_name != NULL) {
        qdos_name_len = (uint32_t)strlen(mapped_name);
    }
    if (mapped_name == NULL && metadata_entry != NULL) {
        qdos_name_len = qlmdv_read_be16(metadata_entry->header + qlmdv_qdos_name_length_offset);
        if (qdos_name_len > 0 && qdos_name_len <= qlmdv_qdos_name_size) {
            qdos_name = metadata_entry->header + qlmdv_qdos_name_offset;
        } else {
            qdos_name_len = 0;
        }
    }
    if (qdos_name_len == 0) {
        qdos_name = (uint8_t *)basename;
        qdos_name_len = (uint32_t)strlen(basename);
    }
    if (!qlmdv_set_file_name(imported, qdos_name, qdos_name_len, error, error_size)) {
        qlmdv_file_clear(imported);
        return false;
    }
    return true;
}

static void qlmdv_fprint_name(FILE *stream, const uint8_t *name, uint32_t name_len) {
    static char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < name_len; i += 1) {
        uint8_t ch = name[i];
        if (ch >= 0x20 && ch <= 0x7E && ch != '\\') {
            fputc(ch, stream);
            continue;
        }
        if (ch == '\\') {
            fputs("\\\\", stream);
            continue;
        }
        fputs("\\x", stream);
        fputc(hex[ch >> 4], stream);
        fputc(hex[ch & 0x0F], stream);
    }
}

static void qlmdv_print_name(const uint8_t *name, uint32_t name_len) {
    qlmdv_fprint_name(stdout, name, name_len);
}

static bool qlmdv_host_leaf(
    const uint8_t *name,
    uint32_t name_len,
    char *leaf,
    uint32_t leaf_size
) {
    static char hex[] = "0123456789ABCDEF";
    uint32_t output = 0;
    for (uint32_t i = 0; i < name_len; i += 1) {
        uint8_t ch = name[i];
        bool safe = ch >= 0x20 && ch <= 0x7E && ch != '/' && ch != '\\' && ch != '%';
        uint32_t needed = 3;
        if (safe) {
            needed = 1;
        }
        if (output + needed >= leaf_size) {
            return false;
        }
        if (safe) {
            leaf[output] = (char)ch;
            output += 1;
        } else {
            leaf[output] = '%';
            leaf[output + 1] = hex[ch >> 4];
            leaf[output + 2] = hex[ch & 0x0F];
            output += 3;
        }
    }
    leaf[output] = '\0';
    if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0 || leaf[0] == '\0') {
        if (output + 2 >= leaf_size) {
            return false;
        }
        memmove(leaf + 3, leaf + 1, output);
        leaf[0] = '%';
        leaf[1] = '2';
        leaf[2] = 'E';
    }
    return true;
}

static bool qlmdv_load_clean_image(
    char *path,
    qlmdv_image *image,
    char *error,
    uint32_t error_size
) {
    if (!qlmdv_image_load(image, path, error, error_size)) {
        return false;
    }
    if (image->issue_count != 0) {
        (void)snprintf(
            error,
            error_size,
            "%s has %" PRIu32 " validation problem(s); run 'qlmdv inspect %s'",
            path,
            image->issue_count,
            path
        );
        return false;
    }
    return true;
}

static bool qlmdv_load_mutable_image(
    char *path,
    bool force,
    qlmdv_image *image,
    char *error,
    uint32_t error_size
) {
    if (!qlmdv_image_load(image, path, error, error_size)) {
        return false;
    }
    return qlmdv_image_can_modify(image, force, error, error_size);
}

static qlmdv_image *qlmdv_command_image_alloc(void) {
    qlmdv_image *image = arena_alloc_zero(1, sizeof *image);
    if (image == NULL) {
        fputs("qlmdv: out of memory while allocating image state\n", stderr);
    }
    return image;
}

static void qlmdv_command_image_free(qlmdv_image *image) {
    qlmdv_image_clear(image);
    arena_release(image);
}

static qlmdv_metadata *qlmdv_command_metadata_alloc(void) {
    qlmdv_metadata *metadata = arena_alloc_zero(1, sizeof *metadata);
    if (metadata == NULL) {
        fputs("qlmdv: out of memory while allocating metadata state\n", stderr);
    }
    return metadata;
}

static void qlmdv_print_file_list_entry(qlmdv_file *file, bool verbose) {
    printf(
        "%4" PRIu8 " %10" PRIu32,
        file->header[qlmdv_qdos_type_offset],
        file->data_len
    );
    if (verbose) {
        printf(
            " %10" PRIu32 " %08" PRIX32 " %7" PRIu16 " %08" PRIX32,
            qlmdv_read_be32(file->header + qlmdv_qdos_data_space_offset),
            qlmdv_read_be32(file->header + qlmdv_qdos_update_offset),
            qlmdv_read_be16(file->header + qlmdv_qdos_version_offset),
            qlmdv_read_be32(file->header + qlmdv_qdos_backup_offset)
        );
    }
    fputs("  ", stdout);
    qlmdv_print_name(file->name, file->name_len);
    fputc('\n', stdout);
}

static int qlmdv_command_list_run(char *path, bool verbose) {
    char error[512];
    qlmdv_image *image = qlmdv_command_image_alloc();
    if (image == NULL) {
        return 1;
    }
    if (!qlmdv_load_clean_image(path, image, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        qlmdv_command_image_free(image);
        return 1;
    }
    if (verbose) {
        puts("TYPE     LENGTH DATA_SPACE   UPDATE VERSION   BACKUP  NAME");
    } else {
        puts("TYPE     LENGTH  NAME");
    }
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        qlmdv_print_file_list_entry(&image->files[i], verbose);
    }
    qlmdv_command_image_free(image);
    return 0;
}

static void qlmdv_print_medium_name(const uint8_t name[10]) {
    uint32_t length = 10;
    while (length > 0 && name[length - 1] == ' ') {
        length -= 1;
    }
    qlmdv_print_name(name, length);
}

static char *qlmdv_qdos_type_description(uint8_t type) {
    switch (type) {
    case 0:
        return "default";
    case 1:
        return "executable";
    case 2:
        return "SROFF relocatable object";
    case 255:
        return "directory";
    default:
        return "unknown";
    }
}

static void qlmdv_print_hex_words(const uint8_t *bytes, uint32_t byte_count) {
    static char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < byte_count; i += 1) {
        if (i > 0 && i % 2 == 0) {
            fputc(' ', stdout);
        }
        fputc(hex[bytes[i] >> 4], stdout);
        fputc(hex[bytes[i] & 0x0F], stdout);
    }
}

static void qlmdv_print_inspect_files(qlmdv_image *image) {
    printf("files (%" PRIu32 "):\n", image->file_count);
    if (image->file_count == 0) {
        puts("  (none)");
        return;
    }
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        qlmdv_file *file = &image->files[i];
        printf("  file ID %" PRIu8 ":\n", file->original_file_id);

        printf("    %02" PRIX32 ": [", qlmdv_qdos_length_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_length_offset, 4);
        printf("] total length: %" PRIu32, qlmdv_read_be32(file->header + qlmdv_qdos_length_offset));
        printf(", data length: %" PRIu32 "\n", file->data_len);

        printf("    %02" PRIX32 ": [", qlmdv_qdos_access_key_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_access_key_offset, 1);
        printf("] access key\n");

        printf("    %02" PRIX32 ": [", qlmdv_qdos_type_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_type_offset, 1);
        uint8_t type = file->header[qlmdv_qdos_type_offset];
        printf("] type: %" PRIu8 " %s\n", type, qlmdv_qdos_type_description(type));

        printf("    %02" PRIX32 ": [", qlmdv_qdos_data_space_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_data_space_offset, 4);
        printf("] data space: %" PRIu32 "\n", qlmdv_read_be32(file->header + qlmdv_qdos_data_space_offset));

        printf("    %02" PRIX32 ": [", qlmdv_qdos_type_info_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_type_info_offset, 4);
        printf("] type info: %" PRIu32 "\n", qlmdv_read_be32(file->header + qlmdv_qdos_type_info_offset));

        printf("    %02" PRIX32 ": [", qlmdv_qdos_name_length_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_name_length_offset, 2);
        printf("] name length: %" PRIu8 "\n", file->name_len);

        printf("    %02" PRIX32 ": [", qlmdv_qdos_name_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_name_offset, qlmdv_qdos_name_size);
        printf("] name: \"");
        qlmdv_print_name(file->name, file->name_len);
        printf("\"\n");

        printf("    %02" PRIX32 ": [", qlmdv_qdos_update_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_update_offset, 4);
        printf("] update date (QDOS time)\n");

        printf("    %02" PRIX32 ": [", qlmdv_qdos_version_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_version_offset, 2);
        printf("] version: %" PRIu16 "\n", qlmdv_read_be16(file->header + qlmdv_qdos_version_offset));

        printf("    %02" PRIX32 ": [", qlmdv_qdos_reserved_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_reserved_offset, 2);
        printf("] reserved\n");

        printf("    %02" PRIX32 ": [", qlmdv_qdos_backup_offset);
        qlmdv_print_hex_words(file->header + qlmdv_qdos_backup_offset, 4);
        printf("] backup date (QDOS time)\n");
    }
}

static int qlmdv_command_inspect_run(char *path, bool verbose) {
    char error[512];
    qlmdv_image *image = qlmdv_command_image_alloc();
    if (image == NULL) {
        return 1;
    }
    if (!qlmdv_image_load(image, path, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        qlmdv_command_image_free(image);
        return 1;
    }

    fputs("medium name: ", stdout);
    qlmdv_print_medium_name(image->medium_name);
    fputc('\n', stdout);
    printf("random ID: 0x%04" PRIX16 "\n", image->random_id);
    printf("sectors: %" PRIu32 " physical, %" PRIu32 " usable, %" PRIu32 " bad\n",
        image->sector_count, image->good_sector_count, image->bad_sector_count);
    printf("allocation: %" PRIu32 " used, %" PRIu32 " free, %" PRIu32 " file(s)\n",
        image->used_sector_count, image->free_sector_count, image->file_count);
    printf(
        "checksums: %" PRIu32 " invalid, %" PRIu32 " valid\n",
        image->checksum_issue_count,
        image->formatted_sector_count * 3 - image->checksum_issue_count
    );
    qlmdv_print_inspect_files(image);

    if (verbose) {
        puts("sector detail:");
        for (uint32_t physical = 0; physical < image->sector_count; physical += 1) {
            uint8_t *sector = image->bytes + physical * qlay_sector_size;
            if (sector[qlay_sector_header_offset] != qlay_sector_header_flag) {
                printf("  physical %3" PRIu32 ": bad/unformatted record\n", physical);
                continue;
            }
            uint32_t logical = sector[qlay_sector_header_offset + 1];
            uint8_t map_file = 0;
            uint8_t map_block = 0;
            if (logical < image->sector_count) {
                map_file = image->map[logical * 2];
                map_block = image->map[logical * 2 + 1];
            }
            bool header_ok = qlay_read_checksum(sector + qlay_sector_header_checksum_offset) ==
                qlay_checksum(sector + qlay_sector_header_offset, qlay_sector_header_size);
            bool block_ok = qlay_read_checksum(sector + qlay_block_header_checksum_offset) ==
                qlay_checksum(sector + qlay_block_header_offset, qlay_block_header_size);
            bool data_ok = qlay_read_checksum(sector + qlay_data_checksum_offset) ==
                qlay_checksum(sector + qlay_data_offset, qlay_data_size);
            char header_status = '-';
            char block_status = '-';
            char data_status = '-';
            if (header_ok) {
                header_status = 'H';
            }
            if (block_ok) {
                block_status = 'B';
            }
            if (data_ok) {
                data_status = 'D';
            }
            printf(
                "  sector %3" PRIu32 " physical %3" PRIu32 ": map=%02" PRIX8 "/%02" PRIX8
                    " header=%02" PRIX8 "/%02" PRIX8 " checksums=%c%c%c\n",
                logical,
                physical,
                map_file,
                map_block,
                sector[qlay_block_header_offset],
                sector[qlay_block_header_offset + 1],
                header_status,
                block_status,
                data_status
            );
        }
    }

    if (image->issue_count == 0) {
        puts("status: valid");
        qlmdv_command_image_free(image);
        return 0;
    }
    printf("problems (%" PRIu32 "):\n", image->issue_count);
    for (uint32_t i = 0; i < image->issue_count; i += 1) {
        printf("  - %s\n", image->issues[i].text);
    }
    puts("status: corrupt");
    qlmdv_command_image_free(image);
    return 1;
}

static bool qlmdv_metadata_write(
    char *path,
    qlmdv_image *image,
    bool selected[qlmdv_max_files],
    char leaves[qlmdv_max_files][qlmdv_qdos_name_size * 3 + 4],
    bool force,
    char *error,
    uint32_t error_size
) {
    static char hex[] = "0123456789ABCDEF";
    char *header_line = "QLMDV-METADATA 1\n";
    enum {
        encoded_size = qlmdv_qdos_name_size * 9 + 16,
        line_size = encoded_size + qlmdv_qdos_header_size * 2 + 2
    };
    uint32_t header_len = (uint32_t)strlen(header_line);
    uint32_t capacity = header_len + image->file_count * line_size;
    uint8_t *manifest = arena_alloc(capacity);
    if (manifest == NULL) {
        (void)snprintf(error, error_size, "out of memory while creating metadata");
        return false;
    }
    memcpy(manifest, header_line, header_len);
    uint32_t manifest_len = header_len;
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        if (!selected[i]) {
            continue;
        }
        char encoded[encoded_size];
        if (!qlmdv_manifest_escape(leaves[i], encoded, sizeof encoded)) {
            (void)snprintf(error, error_size, "could not encode metadata filename");
            arena_release(manifest);
            return false;
        }
        char line[line_size];
        uint32_t offset = (uint32_t)snprintf(line, sizeof line, "%s\t", encoded);
        for (uint32_t byte = 0; byte < qlmdv_qdos_header_size; byte += 1) {
            uint8_t value = image->files[i].header[byte];
            line[offset] = hex[value >> 4];
            line[offset + 1] = hex[value & 0x0F];
            offset += 2;
        }
        line[offset] = '\n';
        offset += 1;
        memcpy(manifest + manifest_len, line, offset);
        manifest_len += offset;
    }
    bool ok = qlmdv_write_transaction(path, manifest, manifest_len, true, force, error, error_size);
    arena_release(manifest);
    return ok;
}

static bool qlmdv_extract_one(
    int directory_fd,
    qlmdv_file *file,
    char *leaf,
    bool force,
    char *error,
    uint32_t error_size
) {
    if (force) {
        struct stat existing;
        if (fstatat(directory_fd, leaf, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(existing.st_mode)) {
                (void)snprintf(error, error_size, "%s is not a regular file", leaf);
                return false;
            }
        } else if (errno != ENOENT) {
            (void)snprintf(error, error_size, "could not stat %s: %s", leaf, strerror(errno_or_eio(errno)));
            return false;
        }
    }
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (force) {
        flags |= O_TRUNC;
    } else {
        flags |= O_EXCL;
    }
    int fd = openat(directory_fd, leaf, flags, 0666);
    if (fd < 0) {
        (void)snprintf(error, error_size, "could not create %s: %s", leaf, strerror(errno_or_eio(errno)));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)snprintf(error, error_size, "%s is not a regular file", leaf);
        (void)close(fd);
        return false;
    }
    int write_error = 0;
    bool ok = qlmdv_write_all_fd(fd, file->data, file->data_len, &write_error);
    if (ok) {
        struct timespec times[2];
        times[0].tv_sec = time(NULL);
        times[0].tv_nsec = 0;
        times[1].tv_sec = qdos_to_unix_time(
            int32_from_uint32_bits(qlmdv_read_be32(file->header + qlmdv_qdos_update_offset))
        );
        times[1].tv_nsec = 0;
        if (futimens(fd, times) != 0) {
            write_error = errno_or_eio(errno);
            ok = false;
        }
    }
    if (close(fd) != 0 && ok) {
        write_error = errno_or_eio(errno);
        ok = false;
    }
    if (!ok) {
        (void)snprintf(error, error_size, "could not write %s: %s", leaf, strerror(write_error));
        return false;
    }
    return true;
}

static int qlmdv_command_extract_run(
    char *path,
    qlmdv_options *options,
    int operand_count,
    char **operands
) {
    char error[512];
    qlmdv_image *image = qlmdv_command_image_alloc();
    if (image == NULL) {
        return 1;
    }
    if (!qlmdv_load_clean_image(path, image, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        qlmdv_command_image_free(image);
        return 1;
    }

    bool selected[qlmdv_max_files] = { false };
    if (operand_count == 0) {
        for (uint32_t i = 0; i < image->file_count; i += 1) {
            selected[i] = true;
        }
    } else {
        for (int operand = 0; operand < operand_count; operand += 1) {
            int32_t index = qlmdv_find_file(image, operands[operand]);
            if (index < 0) {
                fprintf(stderr, "qlmdv: file not found: %s\n", operands[operand]);
                qlmdv_command_image_free(image);
                return 1;
            }
            selected[index] = true;
        }
    }
    uint32_t selected_count = 0;
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        if (selected[i]) {
            selected_count += 1;
        }
    }

    char leaves[qlmdv_max_files][qlmdv_qdos_name_size * 3 + 4] = { { 0 } };
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        if (
            selected[i] &&
            !qlmdv_host_leaf(image->files[i].name, image->files[i].name_len, leaves[i], sizeof leaves[i])
        ) {
            fprintf(stderr, "qlmdv: extracted filename is too long\n");
            qlmdv_command_image_free(image);
            return 1;
        }
    }

    int directory_fd = open(options->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        fprintf(stderr, "qlmdv: could not open directory %s: %s\n", options->directory, strerror(errno_or_eio(errno)));
        qlmdv_command_image_free(image);
        return 1;
    }
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        if (!selected[i]) {
            continue;
        }
        if (!qlmdv_extract_one(
            directory_fd,
            &image->files[i],
            leaves[i],
            options->force,
            error,
            sizeof error
        )) {
            fprintf(stderr, "qlmdv: %s\n", error);
            (void)close(directory_fd);
            qlmdv_command_image_free(image);
            return 1;
        }
    }
    if (close(directory_fd) != 0) {
        fprintf(stderr, "qlmdv: could not close directory %s: %s\n", options->directory, strerror(errno_or_eio(errno)));
        qlmdv_command_image_free(image);
        return 1;
    }
    if (
        options->metadata_path != NULL &&
        !qlmdv_metadata_write(
            options->metadata_path,
            image,
            selected,
            leaves,
            options->force,
            error,
            sizeof error
        )
    ) {
        fprintf(stderr, "qlmdv: %s\n", error);
        qlmdv_command_image_free(image);
        return 1;
    }
    if (options->verbose) {
        printf("extracted %" PRIu32 " file(s) from %s to %s\n", selected_count, path, options->directory);
    }
    qlmdv_command_image_free(image);
    return 0;
}

static bool qlmdv_import_all(
    int operand_count,
    char **operands,
    qlmdv_metadata *metadata,
    qlmdv_file imported[qlmdv_max_files],
    char *error,
    uint32_t error_size
) {
    for (int i = 0; i < operand_count; i += 1) {
        if (!qlmdv_import_operand(operands[i], metadata, &imported[i], error, error_size)) {
            for (int clear = 0; clear <= i; clear += 1) {
                qlmdv_file_clear(&imported[clear]);
            }
            return false;
        }
        for (int previous = 0; previous < i; previous += 1) {
            if (qlmdv_bytes_equal_ignore_case(
                imported[i].name,
                imported[i].name_len,
                imported[previous].name,
                imported[previous].name_len
            )) {
                (void)snprintf(error, error_size, "import operands contain duplicate QDOS names");
                for (int clear = 0; clear <= i; clear += 1) {
                    qlmdv_file_clear(&imported[clear]);
                }
                return false;
            }
        }
    }
    return true;
}

static int qlmdv_command_create_run(
    char *path,
    qlmdv_options *options,
    int operand_count,
    char **operands
) {
    char error[512];
    uint8_t medium_name[10];
    qlmdv_default_medium_name(path, medium_name);
    if (options->medium_name != NULL) {
        uint32_t length = (uint32_t)strlen(options->medium_name);
        if (length == 0 || length > 10) {
            fprintf(stderr, "qlmdv: --medium-name must contain 1-10 bytes\n");
            return 1;
        }
        memset(medium_name, ' ', sizeof medium_name);
        memcpy(medium_name, options->medium_name, length);
    }
    uint16_t random_id = qlmdv_random_id();
    if (options->random_id_set) {
        random_id = (uint16_t)options->random_id;
    }

    uint8_t *initial = NULL;
    uint32_t initial_len = 0;
    if (!qlmdv_create_image_bytes(
        options->sectors,
        medium_name,
        random_id,
        &initial,
        &initial_len,
        error,
        sizeof error
    )) {
        fprintf(stderr, "qlmdv: %s\n", error);
        return 1;
    }
    qlmdv_image *image = qlmdv_command_image_alloc();
    if (image == NULL) {
        arena_release(initial);
        return 1;
    }
    if (!qlmdv_image_decode(image, initial, initial_len, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        arena_release(initial);
        qlmdv_command_image_free(image);
        return 1;
    }
    arena_release(initial);
    if (image->issue_count != 0) {
        fprintf(stderr, "qlmdv: internal error: newly created image did not validate\n");
        qlmdv_command_image_free(image);
        return 1;
    }

    qlmdv_metadata *metadata = qlmdv_command_metadata_alloc();
    if (metadata == NULL) {
        qlmdv_command_image_free(image);
        return 1;
    }
    if (!qlmdv_metadata_load(options->metadata_path, metadata, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        arena_release(metadata);
        qlmdv_command_image_free(image);
        return 1;
    }
    if (operand_count > qlmdv_max_files) {
        fprintf(stderr, "qlmdv: a cartridge can contain at most %d files\n", qlmdv_max_files);
        arena_release(metadata);
        qlmdv_command_image_free(image);
        return 1;
    }
    qlmdv_file imported[qlmdv_max_files] = { 0 };
    if (!qlmdv_import_all(operand_count, operands, metadata, imported, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        arena_release(metadata);
        qlmdv_command_image_free(image);
        return 1;
    }
    arena_release(metadata);
    for (int i = 0; i < operand_count; i += 1) {
        image->files[image->file_count] = imported[i];
        image->file_count += 1;
        imported[i] = (qlmdv_file){ 0 };
    }
    bool ok = qlmdv_rebuild_and_write(image, path, true, options->force, error, sizeof error);
    qlmdv_command_image_free(image);
    if (!ok) {
        fprintf(stderr, "qlmdv: %s\n", error);
        return 1;
    }
    if (options->verbose) {
        printf("created %s: %d file(s), %" PRIu32 " sectors\n", path, operand_count, options->sectors);
    }
    return 0;
}

static int qlmdv_command_add_or_replace_run(
    qlmdv_command command,
    char *path,
    qlmdv_options *options,
    int operand_count,
    char **operands
) {
    char error[512];
    qlmdv_image *image = qlmdv_command_image_alloc();
    if (image == NULL) {
        return 1;
    }
    if (!qlmdv_load_mutable_image(path, options->force, image, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        qlmdv_command_image_free(image);
        return 1;
    }
    qlmdv_metadata *metadata = qlmdv_command_metadata_alloc();
    if (metadata == NULL) {
        qlmdv_command_image_free(image);
        return 1;
    }
    if (!qlmdv_metadata_load(options->metadata_path, metadata, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        arena_release(metadata);
        qlmdv_command_image_free(image);
        return 1;
    }
    if (
        (uint32_t)operand_count > qlmdv_max_files - image->file_count &&
        command == qlmdv_command_add
    ) {
        fprintf(stderr, "qlmdv: a cartridge can contain at most %d files\n", qlmdv_max_files);
        arena_release(metadata);
        qlmdv_command_image_free(image);
        return 1;
    }

    qlmdv_file imported[qlmdv_max_files] = { 0 };
    if (!qlmdv_import_all(operand_count, operands, metadata, imported, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        arena_release(metadata);
        qlmdv_command_image_free(image);
        return 1;
    }
    arena_release(metadata);
    int32_t targets[qlmdv_max_files];
    for (int i = 0; i < operand_count; i += 1) {
        targets[i] = qlmdv_find_file_bytes(
            image,
            imported[i].name,
            imported[i].name_len
        );
        if (command == qlmdv_command_add && targets[i] >= 0) {
            fprintf(stderr, "qlmdv: QDOS name already exists: ");
            qlmdv_fprint_name(stderr, imported[i].name, imported[i].name_len);
            fputc('\n', stderr);
            for (int clear = 0; clear < operand_count; clear += 1) {
                qlmdv_file_clear(&imported[clear]);
            }
            qlmdv_command_image_free(image);
            return 1;
        }
        if (command == qlmdv_command_replace && targets[i] < 0) {
            fprintf(stderr, "qlmdv: QDOS name does not exist: ");
            qlmdv_fprint_name(stderr, imported[i].name, imported[i].name_len);
            fputc('\n', stderr);
            for (int clear = 0; clear < operand_count; clear += 1) {
                qlmdv_file_clear(&imported[clear]);
            }
            qlmdv_command_image_free(image);
            return 1;
        }
    }

    for (int i = 0; i < operand_count; i += 1) {
        if (command == qlmdv_command_add) {
            image->files[image->file_count] = imported[i];
            image->file_count += 1;
        } else {
            qlmdv_file_clear(&image->files[targets[i]]);
            image->files[targets[i]] = imported[i];
        }
        imported[i] = (qlmdv_file){ 0 };
    }
    bool ok = qlmdv_rebuild_and_write(image, path, false, options->force, error, sizeof error);
    qlmdv_command_image_free(image);
    if (!ok) {
        fprintf(stderr, "qlmdv: %s\n", error);
        return 1;
    }
    if (options->verbose) {
        char *action = "replaced";
        if (command == qlmdv_command_add) {
            action = "added";
        }
        printf("%s %d file(s) in %s\n", action, operand_count, path);
    }
    return 0;
}

static int qlmdv_command_remove_run(
    char *path,
    qlmdv_options *options,
    int operand_count,
    char **operands
) {
    char error[512];
    qlmdv_image *image = qlmdv_command_image_alloc();
    if (image == NULL) {
        return 1;
    }
    if (!qlmdv_load_mutable_image(path, options->force, image, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        qlmdv_command_image_free(image);
        return 1;
    }
    bool remove_file[qlmdv_max_files] = { false };
    for (int i = 0; i < operand_count; i += 1) {
        int32_t index = qlmdv_find_file(image, operands[i]);
        if (index < 0) {
            fprintf(stderr, "qlmdv: file not found: %s\n", operands[i]);
            qlmdv_command_image_free(image);
            return 1;
        }
        if (remove_file[index]) {
            fprintf(stderr, "qlmdv: file selected more than once: %s\n", operands[i]);
            qlmdv_command_image_free(image);
            return 1;
        }
        remove_file[index] = true;
    }

    uint32_t output = 0;
    for (uint32_t input = 0; input < image->file_count; input += 1) {
        if (remove_file[input]) {
            qlmdv_file_clear(&image->files[input]);
            continue;
        }
        if (output != input) {
            image->files[output] = image->files[input];
            image->files[input] = (qlmdv_file){ 0 };
        }
        output += 1;
    }
    image->file_count = output;
    bool ok = qlmdv_rebuild_and_write(image, path, false, options->force, error, sizeof error);
    qlmdv_command_image_free(image);
    if (!ok) {
        fprintf(stderr, "qlmdv: %s\n", error);
        return 1;
    }
    if (options->verbose) {
        printf("removed %d file(s) from %s\n", operand_count, path);
    }
    return 0;
}

static int qlmdv_command_reorder_run(
    char *path,
    qlmdv_options *options,
    int operand_count,
    char **operands
) {
    char error[512];
    qlmdv_image *image = qlmdv_command_image_alloc();
    if (image == NULL) {
        return 1;
    }
    if (!qlmdv_load_mutable_image(path, options->force, image, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        qlmdv_command_image_free(image);
        return 1;
    }
    bool selected[qlmdv_max_files] = { false };
    uint32_t order[qlmdv_max_files];
    uint32_t order_count = 0;
    for (int i = 0; i < operand_count; i += 1) {
        int32_t index = qlmdv_find_file(image, operands[i]);
        if (index < 0) {
            fprintf(stderr, "qlmdv: file not found: %s\n", operands[i]);
            qlmdv_command_image_free(image);
            return 1;
        }
        if (selected[index]) {
            fprintf(stderr, "qlmdv: file selected more than once: %s\n", operands[i]);
            qlmdv_command_image_free(image);
            return 1;
        }
        selected[index] = true;
        order[order_count] = (uint32_t)index;
        order_count += 1;
    }
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        if (!selected[i]) {
            order[order_count] = i;
            order_count += 1;
        }
    }
    qlmdv_file reordered[qlmdv_max_files] = { 0 };
    for (uint32_t i = 0; i < order_count; i += 1) {
        reordered[i] = image->files[order[i]];
    }
    memcpy(image->files, reordered, sizeof image->files);

    bool ok = qlmdv_rebuild_and_write(image, path, false, options->force, error, sizeof error);
    qlmdv_command_image_free(image);
    if (!ok) {
        fprintf(stderr, "qlmdv: %s\n", error);
        return 1;
    }
    if (options->verbose) {
        printf("reordered %d file(s) in %s\n", operand_count, path);
    }
    return 0;
}

static int cli_main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        qlmdv_print_usage(stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts(qlmdv_release_string);
        return 0;
    }
    if (argc < 2) {
        qlmdv_print_usage(stderr);
        return 2;
    }
    qlmdv_command command = qlmdv_parse_command(argv[1]);
    if (command == qlmdv_command_none) {
        fprintf(stderr, "qlmdv: unknown command: %s\n", argv[1]);
        qlmdv_print_usage(stderr);
        return 2;
    }

    char error[512];
    qlmdv_options options;
    int image_index = argc;
    if (!qlmdv_parse_options(argc, argv, &image_index, &options, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        return 2;
    }
    if (options.help) {
        qlmdv_print_command_help(command);
        return 0;
    }
    if (!qlmdv_options_valid_for_command(command, &options, error, sizeof error)) {
        fprintf(stderr, "qlmdv: %s\n", error);
        return 2;
    }
    if (image_index >= argc) {
        fprintf(stderr, "qlmdv: missing image path\n");
        qlmdv_print_command_help(command);
        return 2;
    }

    char *path = argv[image_index];
    int operand_count = argc - image_index - 1;
    char **operands = argv + image_index + 1;
    if (
        (command == qlmdv_command_list || command == qlmdv_command_inspect) &&
        operand_count != 0
    ) {
        fprintf(stderr, "qlmdv: this command does not accept operands\n");
        return 2;
    }
    if (
        (command == qlmdv_command_add ||
            command == qlmdv_command_remove ||
            command == qlmdv_command_replace ||
            command == qlmdv_command_reorder) &&
        operand_count == 0
    ) {
        fprintf(stderr, "qlmdv: this command requires at least one operand\n");
        return 2;
    }
    if (operand_count > qlmdv_max_files) {
        fprintf(stderr, "qlmdv: too many operands\n");
        return 2;
    }

    int status = 2;
    switch (command) {
    case qlmdv_command_create:
        status = qlmdv_command_create_run(path, &options, operand_count, operands);
        break;
    case qlmdv_command_list:
        status = qlmdv_command_list_run(path, options.verbose);
        break;
    case qlmdv_command_extract:
        status = qlmdv_command_extract_run(path, &options, operand_count, operands);
        break;
    case qlmdv_command_add:
    case qlmdv_command_replace:
        status = qlmdv_command_add_or_replace_run(command, path, &options, operand_count, operands);
        break;
    case qlmdv_command_remove:
        status = qlmdv_command_remove_run(path, &options, operand_count, operands);
        break;
    case qlmdv_command_reorder:
        status = qlmdv_command_reorder_run(path, &options, operand_count, operands);
        break;
    case qlmdv_command_inspect:
        status = qlmdv_command_inspect_run(path, options.verbose);
        break;
    default:
        break;
    }
    if (options.verbose) {
        arena_report();
    }
    return status;
}
