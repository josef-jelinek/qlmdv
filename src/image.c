enum {
    qlmdv_qdos_header_size = 64,
    qlmdv_qdos_length_offset = 0,
    qlmdv_qdos_access_key_offset = 4,
    qlmdv_qdos_type_offset = 5,
    qlmdv_qdos_data_space_offset = 6,
    qlmdv_qdos_type_info_offset = 10,
    qlmdv_qdos_name_length_offset = 14,
    qlmdv_qdos_name_offset = 16,
    qlmdv_qdos_name_size = 36,
    qlmdv_qdos_update_offset = 52,
    qlmdv_qdos_version_offset = 56,
    qlmdv_qdos_reserved_offset = 58,
    qlmdv_qdos_backup_offset = 60,
    qlmdv_max_files = 240,
    qlmdv_max_issues = 2048,
    qlmdv_issue_text_size = 192
};

_Static_assert(
    qlmdv_qdos_backup_offset + 4 == qlmdv_qdos_header_size,
    "QDOS file header fields must fill 64 bytes"
);

typedef struct {
    uint8_t header[qlmdv_qdos_header_size];
    uint8_t name[qlmdv_qdos_name_size];
    uint8_t *data;
    uint32_t data_len;
    uint8_t name_len;
    uint8_t original_file_id;
} qlmdv_file;

typedef struct {
    char text[qlmdv_issue_text_size];
    bool checksum;
} qlmdv_issue;

typedef struct {
    uint8_t *bytes;
    uint32_t byte_count;
    uint32_t sector_count;
    int16_t logical_to_physical[qlay_max_sectors];
    uint8_t map[qlay_data_size];
    uint8_t medium_name[10];
    uint8_t directory_header[qlmdv_qdos_header_size];
    qlmdv_file files[qlmdv_max_files];
    qlmdv_issue issues[qlmdv_max_issues];
    uint32_t file_count;
    uint32_t issue_count;
    uint32_t checksum_issue_count;
    uint32_t structural_issue_count;
    uint32_t good_sector_count;
    uint32_t formatted_sector_count;
    uint32_t bad_sector_count;
    uint32_t free_sector_count;
    uint32_t used_sector_count;
    uint16_t random_id;
    uint8_t map_file_id;
    bool directory_available;
} qlmdv_image;

static uint16_t qlmdv_read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint32_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t qlmdv_read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) |
        ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) |
        bytes[3];
}

static void qlmdv_write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void qlmdv_write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static bool qlmdv_file_id_is_map(uint8_t file_id) {
    return file_id == qlay_map_file_id || file_id == qlay_alternate_map_file_id;
}

static bool qlmdv_file_id_is_bad(uint8_t file_id) {
    return file_id == qlay_bad_file_id || file_id == qlay_bad_file_id_alternate;
}

static bool qlmdv_bytes_equal_ignore_case(
    const uint8_t *left,
    uint32_t left_len,
    const uint8_t *right,
    uint32_t right_len
) {
    if (left_len != right_len) {
        return false;
    }
    for (uint32_t i = 0; i < left_len; i += 1) {
        if (ascii_lower(left[i]) != ascii_lower(right[i])) {
            return false;
        }
    }
    return true;
}

static void qlmdv_add_issue(qlmdv_image *image, bool checksum, char *fmt, ...) {
    if (image->issue_count >= qlmdv_max_issues) {
        return;
    }

    qlmdv_issue *issue = &image->issues[image->issue_count];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(issue->text, sizeof issue->text, fmt, ap);
    va_end(ap);
    issue->checksum = checksum;
    image->issue_count += 1;
    if (checksum) {
        image->checksum_issue_count += 1;
    } else {
        image->structural_issue_count += 1;
    }
}

static void qlmdv_file_clear(qlmdv_file *file) {
    arena_release(file->data);
    *file = (qlmdv_file){ 0 };
}

static void qlmdv_image_clear(qlmdv_image *image) {
    if (image == NULL) {
        return;
    }
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        qlmdv_file_clear(&image->files[i]);
    }
    arena_release(image->bytes);
    memset(image, 0, sizeof *image);
}

static uint8_t *qlmdv_sector_bytes(qlmdv_image *image, uint32_t logical_sector) {
    if (logical_sector >= image->sector_count) {
        return NULL;
    }
    int16_t physical = image->logical_to_physical[logical_sector];
    if (physical < 0) {
        return NULL;
    }
    return image->bytes + (uint32_t)physical * qlay_sector_size;
}

static bool qlmdv_bytes_are(const uint8_t *bytes, uint32_t count, uint8_t value) {
    for (uint32_t i = 0; i < count; i += 1) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}

static void qlmdv_validate_record_preambles(
    qlmdv_image *image,
    const uint8_t *sector,
    uint32_t physical
) {
    if (
        !qlmdv_bytes_are(sector, 10, 0) ||
        sector[10] != 0xFF ||
        sector[11] != 0xFF
    ) {
        qlmdv_add_issue(image, false, "physical sector %" PRIu32 " has an invalid sector preamble", physical);
    }
    if (
        !qlmdv_bytes_are(sector + qlay_block_preamble_offset, 10, 0) ||
        sector[38] != 0xFF ||
        sector[39] != 0xFF
    ) {
        qlmdv_add_issue(image, false, "physical sector %" PRIu32 " has an invalid block preamble", physical);
    }
    if (
        !qlmdv_bytes_are(sector + qlay_data_preamble_offset, 6, 0) ||
        sector[50] != 0xFF ||
        sector[51] != 0xFF
    ) {
        qlmdv_add_issue(image, false, "physical sector %" PRIu32 " has an invalid data preamble", physical);
    }
}

static void qlmdv_validate_checksums(
    qlmdv_image *image,
    const uint8_t *sector,
    uint32_t physical,
    uint32_t logical
) {
    uint16_t expected = qlay_checksum(sector + qlay_sector_header_offset, qlay_sector_header_size);
    uint16_t stored = qlay_read_checksum(sector + qlay_sector_header_checksum_offset);
    if (expected != stored) {
        qlmdv_add_issue(
            image,
            true,
            "sector %" PRIu32 " (physical %" PRIu32 ") header checksum is %04" PRIX16
                ", expected %04" PRIX16,
            logical,
            physical,
            stored,
            expected
        );
    }

    expected = qlay_checksum(sector + qlay_block_header_offset, qlay_block_header_size);
    stored = qlay_read_checksum(sector + qlay_block_header_checksum_offset);
    if (expected != stored) {
        qlmdv_add_issue(
            image,
            true,
            "sector %" PRIu32 " (physical %" PRIu32 ") block checksum is %04" PRIX16
                ", expected %04" PRIX16,
            logical,
            physical,
            stored,
            expected
        );
    }

    expected = qlay_checksum(sector + qlay_data_offset, qlay_data_size);
    stored = qlay_read_checksum(sector + qlay_data_checksum_offset);
    if (expected != stored) {
        qlmdv_add_issue(
            image,
            true,
            "sector %" PRIu32 " (physical %" PRIu32 ") data checksum is %04" PRIX16
                ", expected %04" PRIX16,
            logical,
            physical,
            stored,
            expected
        );
    }
}

static void qlmdv_scan_sector_headers(qlmdv_image *image) {
    for (uint32_t i = 0; i < qlay_max_sectors; i += 1) {
        image->logical_to_physical[i] = -1;
    }

    uint32_t invalid_headers = 0;
    for (uint32_t physical = 0; physical < image->sector_count; physical += 1) {
        uint8_t *sector = image->bytes + physical * qlay_sector_size;
        if (sector[qlay_sector_header_offset] != qlay_sector_header_flag) {
            invalid_headers += 1;
            continue;
        }

        uint32_t logical = sector[qlay_sector_header_offset + 1];
        image->formatted_sector_count += 1;
        qlmdv_validate_record_preambles(image, sector, physical);
        qlmdv_validate_checksums(image, sector, physical, logical);
        if (logical >= image->sector_count) {
            qlmdv_add_issue(
                image,
                false,
                "physical sector %" PRIu32 " identifies out-of-range sector %" PRIu32,
                physical,
                logical
            );
            continue;
        }
        if (image->logical_to_physical[logical] >= 0) {
            qlmdv_add_issue(image, false, "logical sector %" PRIu32 " has duplicate physical records", logical);
            continue;
        }
        image->logical_to_physical[logical] = (int16_t)physical;
    }

    uint8_t *map_sector = qlmdv_sector_bytes(image, 0);
    if (map_sector == NULL) {
        qlmdv_add_issue(image, false, "allocation-map sector 0 is missing");
        return;
    }

    memcpy(image->medium_name, map_sector + qlay_sector_header_offset + 2, sizeof image->medium_name);
    image->random_id = qlmdv_read_be16(map_sector + qlay_sector_header_offset + 12);
    image->map_file_id = map_sector[qlay_block_header_offset];
    memcpy(image->map, map_sector + qlay_data_offset, sizeof image->map);

    if (!qlmdv_file_id_is_map(image->map_file_id) || map_sector[qlay_block_header_offset + 1] != 0) {
        qlmdv_add_issue(image, false, "sector 0 block header does not identify allocation map block 0");
    }
    if (!qlmdv_file_id_is_map(image->map[0]) || image->map[1] != 0) {
        qlmdv_add_issue(image, false, "allocation-map entry 0 does not identify allocation map block 0");
    }

    uint32_t mapped_bad = 0;
    for (uint32_t logical = 0; logical < image->sector_count; logical += 1) {
        uint8_t map_file = image->map[logical * 2];
        if (qlmdv_file_id_is_bad(map_file)) {
            mapped_bad += 1;
        }
    }
    if (mapped_bad != invalid_headers) {
        qlmdv_add_issue(
            image,
            false,
            "allocation map records %" PRIu32 " bad sector(s), but %" PRIu32
                " physical record(s) lack a valid header",
            mapped_bad,
            invalid_headers
        );
    }
}

static void qlmdv_validate_medium_identity(qlmdv_image *image) {
    for (uint32_t logical = 0; logical < image->sector_count; logical += 1) {
        uint8_t *sector = qlmdv_sector_bytes(image, logical);
        if (sector == NULL) {
            continue;
        }
        uint8_t *name = sector + qlay_sector_header_offset + 2;
        uint16_t random_id = qlmdv_read_be16(sector + qlay_sector_header_offset + 12);
        if (memcmp(name, image->medium_name, sizeof image->medium_name) != 0) {
            qlmdv_add_issue(image, false, "sector %" PRIu32 " has a different medium name", logical);
        }
        if (random_id != image->random_id) {
            qlmdv_add_issue(image, false, "sector %" PRIu32 " has a different random ID", logical);
        }
    }
}

static void qlmdv_validate_map_and_blocks(
    qlmdv_image *image,
    uint16_t block_counts[qlmdv_max_files + 1],
    uint8_t block_locations[qlmdv_max_files + 1][qlay_max_sectors]
) {
    for (uint32_t logical = 0; logical < image->sector_count; logical += 1) {
        uint8_t map_file = image->map[logical * 2];
        uint8_t map_block = image->map[logical * 2 + 1];
        uint8_t *sector = qlmdv_sector_bytes(image, logical);

        if (qlmdv_file_id_is_bad(map_file)) {
            image->bad_sector_count += 1;
            continue;
        }
        if (sector == NULL) {
            qlmdv_add_issue(image, false, "usable sector %" PRIu32 " has no valid physical header", logical);
            continue;
        }

        image->good_sector_count += 1;
        uint8_t header_file = sector[qlay_block_header_offset];
        uint8_t header_block = sector[qlay_block_header_offset + 1];
        if (map_file == qlay_free_file_id) {
            image->free_sector_count += 1;
            if (header_file != qlay_free_file_id || header_block != 0) {
                qlmdv_add_issue(image, false, "free sector %" PRIu32 " has a non-free block header", logical);
            }
            continue;
        }
        if (qlmdv_file_id_is_map(map_file)) {
            image->used_sector_count += 1;
            if (logical != 0 || map_block != 0 || !qlmdv_file_id_is_map(header_file) || header_block != 0) {
                qlmdv_add_issue(image, false, "sector %" PRIu32 " has an invalid allocation-map entry", logical);
            }
            continue;
        }
        if (map_file > qlmdv_max_files) {
            qlmdv_add_issue(
                image,
                false,
                "sector %" PRIu32 " has unsupported allocation file ID %02" PRIX8,
                logical,
                map_file
            );
            continue;
        }

        image->used_sector_count += 1;
        if (header_file != map_file || header_block != map_block) {
            qlmdv_add_issue(
                image,
                false,
                "sector %" PRIu32 " map entry %02" PRIX8 "/%02" PRIX8
                    " disagrees with block header %02" PRIX8 "/%02" PRIX8,
                logical,
                map_file,
                map_block,
                header_file,
                header_block
            );
        }
        if (map_block >= qlay_max_sectors) {
            qlmdv_add_issue(
                image,
                false,
                "sector %" PRIu32 " has out-of-range file block %" PRIu8,
                logical,
                map_block
            );
            continue;
        }
        if (block_locations[map_file][map_block] != 0) {
            qlmdv_add_issue(
                image,
                false,
                "file %" PRIu8 " block %" PRIu8 " is allocated more than once",
                map_file,
                map_block
            );
            block_locations[map_file][map_block] = (uint8_t)(logical + 1);
            continue;
        }
        block_locations[map_file][map_block] = (uint8_t)(logical + 1);
        block_counts[map_file] += 1;
    }

    for (uint32_t file_id = 0; file_id <= qlmdv_max_files; file_id += 1) {
        for (uint32_t block = 0; block < block_counts[file_id]; block += 1) {
            if (block_locations[file_id][block] == 0) {
                qlmdv_add_issue(
                    image,
                    false,
                    "file %" PRIu32 " block chain is missing block %" PRIu32,
                    file_id,
                    block
                );
            }
        }
    }
}

static uint8_t *qlmdv_collect_file_blocks(
    qlmdv_image *image,
    uint32_t file_id,
    uint32_t block_count,
    uint8_t block_locations[qlmdv_max_files + 1][qlay_max_sectors]
) {
    if (block_count == 0) {
        return NULL;
    }
    if (block_count > UINT32_MAX / qlay_data_size) {
        return NULL;
    }

    uint8_t *bytes = arena_alloc_zero(block_count, qlay_data_size);
    if (bytes == NULL) {
        return NULL;
    }
    for (uint32_t block = 0; block < block_count; block += 1) {
        uint8_t logical_plus_one = block_locations[file_id][block];
        if (logical_plus_one == 0) {
            continue;
        }
        uint32_t logical = (uint32_t)logical_plus_one - 1;
        uint8_t *sector = qlmdv_sector_bytes(image, logical);
        if (sector == NULL) {
            continue;
        }
        memcpy(bytes + block * qlay_data_size, sector + qlay_data_offset, qlay_data_size);
    }
    return bytes;
}

static void qlmdv_parse_directory(
    qlmdv_image *image,
    uint16_t block_counts[qlmdv_max_files + 1],
    uint8_t block_locations[qlmdv_max_files + 1][qlay_max_sectors]
) {
    if (block_counts[0] == 0) {
        qlmdv_add_issue(image, false, "directory file 0 has no blocks");
        return;
    }

    uint32_t directory_capacity = block_counts[0] * qlay_data_size;
    uint8_t *directory = qlmdv_collect_file_blocks(image, 0, block_counts[0], block_locations);
    if (directory == NULL) {
        qlmdv_add_issue(image, false, "could not reconstruct directory blocks");
        return;
    }

    uint32_t directory_len = qlmdv_read_be32(directory + qlmdv_qdos_length_offset);
    if (
        directory_len < qlmdv_qdos_header_size ||
        directory_len > directory_capacity ||
        (directory_len % qlmdv_qdos_header_size) != 0
    ) {
        qlmdv_add_issue(
            image,
            false,
            "directory length %" PRIu32 " is invalid for %" PRIu32 " byte(s) of directory blocks",
            directory_len,
            directory_capacity
        );
        arena_release(directory);
        return;
    }

    uint32_t expected_directory_blocks =
        (directory_len + qlay_data_size - 1) / qlay_data_size;
    if (expected_directory_blocks != block_counts[0]) {
        qlmdv_add_issue(
            image,
            false,
            "directory uses %" PRIu16 " block(s), but its length requires %" PRIu32,
            block_counts[0],
            expected_directory_blocks
        );
    }

    uint32_t entry_count = directory_len / qlmdv_qdos_header_size;
    if (entry_count > qlmdv_max_files + 1) {
        qlmdv_add_issue(image, false, "directory contains more than %d file entries", qlmdv_max_files);
        arena_release(directory);
        return;
    }

    memcpy(image->directory_header, directory, qlmdv_qdos_header_size);
    image->directory_available = true;
    bool active_entry[qlmdv_max_files + 1] = { false };
    active_entry[0] = true;
    for (uint32_t entry = 1; entry < entry_count; entry += 1) {
        uint8_t *header = directory + entry * qlmdv_qdos_header_size;
        uint32_t total_len = qlmdv_read_be32(header + qlmdv_qdos_length_offset);
        if (total_len == 0) {
            if (block_counts[entry] != 0) {
                qlmdv_add_issue(image, false, "deleted directory entry %" PRIu32 " still owns blocks", entry);
            }
            continue;
        }
        active_entry[entry] = true;
        if (total_len < qlmdv_qdos_header_size) {
            qlmdv_add_issue(image, false, "file %" PRIu32 " has an invalid length %" PRIu32, entry, total_len);
            continue;
        }

        uint32_t name_len = qlmdv_read_be16(header + qlmdv_qdos_name_length_offset);
        if (name_len == 0 || name_len > qlmdv_qdos_name_size) {
            qlmdv_add_issue(image, false, "file %" PRIu32 " has invalid QDOS name length %" PRIu32, entry, name_len);
            continue;
        }
        if (image->file_count >= qlmdv_max_files) {
            qlmdv_add_issue(image, false, "directory contains too many active file entries");
            continue;
        }

        for (uint32_t previous = 0; previous < image->file_count; previous += 1) {
            qlmdv_file *other = &image->files[previous];
            if (qlmdv_bytes_equal_ignore_case(
                header + qlmdv_qdos_name_offset,
                name_len,
                other->name,
                other->name_len
            )) {
                qlmdv_add_issue(
                    image,
                    false,
                    "directory entries %" PRIu8 " and %" PRIu32 " have duplicate QDOS names",
                    other->original_file_id,
                    entry
                );
            }
        }

        uint32_t data_len = total_len - qlmdv_qdos_header_size;
        uint32_t minimum_blocks =
            (uint32_t)(((uint64_t)total_len + qlay_data_size - 1) / qlay_data_size);
        if (block_counts[entry] < minimum_blocks) {
            qlmdv_add_issue(
                image,
                false,
                "file %" PRIu32 " uses %" PRIu16 " block(s), but its data requires at least %" PRIu32,
                entry,
                block_counts[entry],
                minimum_blocks
            );
        }

        qlmdv_file *file = &image->files[image->file_count];
        memcpy(file->header, header, sizeof file->header);
        memcpy(file->name, header + qlmdv_qdos_name_offset, name_len);
        file->name_len = (uint8_t)name_len;
        file->original_file_id = (uint8_t)entry;
        file->data_len = data_len;
        uint8_t *blocks = qlmdv_collect_file_blocks(image, entry, block_counts[entry], block_locations);
        if (blocks == NULL) {
            qlmdv_add_issue(image, false, "could not reconstruct file %" PRIu32, entry);
            qlmdv_file_clear(file);
            continue;
        }
        if (memcmp(blocks, header, qlmdv_qdos_header_size) != 0) {
            qlmdv_add_issue(
                image,
                false,
                "file %" PRIu32 " block header copy disagrees with its directory entry",
                entry
            );
        }
        if (data_len != 0) {
            file->data = arena_alloc(data_len);
            if (file->data == NULL) {
                arena_release(blocks);
                qlmdv_add_issue(image, false, "could not reconstruct file %" PRIu32, entry);
                qlmdv_file_clear(file);
                continue;
            }
            uint32_t available = block_counts[entry] * qlay_data_size - qlmdv_qdos_header_size;
            uint32_t copy_len = data_len;
            if (copy_len > available) {
                copy_len = available;
            }
            memcpy(file->data, blocks + qlmdv_qdos_header_size, copy_len);
            if (copy_len < data_len) {
                memset(file->data + copy_len, 0, data_len - copy_len);
            }
        }
        arena_release(blocks);
        image->file_count += 1;
    }

    for (uint32_t file_id = 1; file_id <= qlmdv_max_files; file_id += 1) {
        if (block_counts[file_id] != 0 && (file_id >= entry_count || !active_entry[file_id])) {
            qlmdv_add_issue(image, false, "allocated file ID %" PRIu32 " has no active directory entry", file_id);
        }
    }
    arena_release(directory);
}

static bool qlmdv_image_decode(
    qlmdv_image *image,
    const uint8_t *bytes,
    uint32_t byte_count,
    char *error,
    uint32_t error_size
) {
    memset(image, 0, sizeof *image);
    if (
        byte_count == 0 ||
        (byte_count % qlay_sector_size) != 0 ||
        byte_count / qlay_sector_size > qlay_max_sectors
    ) {
        (void)snprintf(error, error_size, "not a raw QLAY image: size must be 1-255 records of 686 bytes");
        return false;
    }

    image->bytes = arena_alloc(byte_count);
    if (image->bytes == NULL) {
        (void)snprintf(error, error_size, "out of memory while reading image");
        return false;
    }
    memcpy(image->bytes, bytes, byte_count);
    image->byte_count = byte_count;
    image->sector_count = byte_count / qlay_sector_size;

    qlmdv_scan_sector_headers(image);
    if (image->logical_to_physical[0] < 0) {
        return true;
    }
    qlmdv_validate_medium_identity(image);

    uint16_t (*block_counts)[qlmdv_max_files + 1] = arena_alloc_zero(1, sizeof *block_counts);
    uint8_t (*block_locations)[qlmdv_max_files + 1][qlay_max_sectors] = arena_alloc_zero(1, sizeof *block_locations);
    if (block_counts == NULL || block_locations == NULL) {
        arena_release(block_counts);
        arena_release(block_locations);
        (void)snprintf(error, error_size, "out of memory while validating image");
        qlmdv_image_clear(image);
        return false;
    }
    qlmdv_validate_map_and_blocks(image, *block_counts, *block_locations);
    qlmdv_parse_directory(image, *block_counts, *block_locations);
    arena_release(block_counts);
    arena_release(block_locations);
    return true;
}

static bool qlmdv_read_all_image(
    char *path,
    uint8_t **bytes,
    uint32_t *byte_count,
    char *error,
    uint32_t error_size
) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        (void)snprintf(error, error_size, "could not open %s: %s", path, strerror(errno_or_eio(errno)));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        (void)snprintf(error, error_size, "could not stat %s: %s", path, strerror(errno_or_eio(errno)));
        (void)close(fd);
        return false;
    }
    uint64_t max_bytes = (uint64_t)qlay_max_sectors * qlay_sector_size;
    if (st.st_size <= 0 || (uint64_t)st.st_size > max_bytes) {
        (void)snprintf(error, error_size, "%s is not a 1-255 sector QLAY image", path);
        (void)close(fd);
        return false;
    }

    uint32_t length = (uint32_t)st.st_size;
    uint8_t *buffer = arena_alloc(length);
    if (buffer == NULL) {
        (void)snprintf(error, error_size, "out of memory while reading %s", path);
        (void)close(fd);
        return false;
    }
    uint32_t done = 0;
    while (done < length) {
        ssize_t count = read_fd_retry(fd, buffer + done, length - done);
        if (count < 0) {
            (void)snprintf(error, error_size, "could not read %s: %s", path, strerror(errno_or_eio(errno)));
            arena_release(buffer);
            (void)close(fd);
            return false;
        }
        if (count == 0) {
            (void)snprintf(error, error_size, "short read from %s", path);
            arena_release(buffer);
            (void)close(fd);
            return false;
        }
        done += (uint32_t)count;
    }
    if (close(fd) != 0) {
        (void)snprintf(error, error_size, "could not close %s: %s", path, strerror(errno_or_eio(errno)));
        arena_release(buffer);
        return false;
    }
    *bytes = buffer;
    *byte_count = length;
    return true;
}

static bool qlmdv_image_load(
    qlmdv_image *image,
    char *path,
    char *error,
    uint32_t error_size
) {
    memset(image, 0, sizeof *image);
    uint8_t *bytes = NULL;
    uint32_t byte_count = 0;
    if (!qlmdv_read_all_image(path, &bytes, &byte_count, error, error_size)) {
        return false;
    }
    bool ok = qlmdv_image_decode(image, bytes, byte_count, error, error_size);
    arena_release(bytes);
    return ok;
}

static bool qlmdv_image_can_modify(
    qlmdv_image *image,
    bool force,
    char *error,
    uint32_t error_size
) {
    if (image->structural_issue_count != 0) {
        (void)snprintf(
            error,
            error_size,
            "image has %" PRIu32 " structural problem(s) and cannot be modified",
            image->structural_issue_count
        );
        return false;
    }
    if (image->checksum_issue_count != 0 && !force) {
        (void)snprintf(
            error,
            error_size,
            "image has %" PRIu32 " checksum problem(s); inspect it or use --force to rebuild",
            image->checksum_issue_count
        );
        return false;
    }
    if (!image->directory_available) {
        (void)snprintf(error, error_size, "image directory is unavailable");
        return false;
    }
    return true;
}

static int32_t qlmdv_find_file_bytes(
    qlmdv_image *image,
    const uint8_t *name,
    uint32_t name_len
) {
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        qlmdv_file *file = &image->files[i];
        if (qlmdv_bytes_equal_ignore_case(file->name, file->name_len, name, name_len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t qlmdv_find_file(qlmdv_image *image, char *name) {
    uint32_t name_len = (uint32_t)strlen(name);
    return qlmdv_find_file_bytes(image, (uint8_t *)name, name_len);
}

static bool qlmdv_set_file_name(
    qlmdv_file *file,
    const uint8_t *name,
    uint32_t name_len,
    char *error,
    uint32_t error_size
) {
    bool contains_nul = false;
    for (uint32_t i = 0; i < name_len; i += 1) {
        if (name[i] == 0) {
            contains_nul = true;
            break;
        }
    }
    if (name_len == 0 || name_len > qlmdv_qdos_name_size || contains_nul) {
        (void)snprintf(error, error_size, "QDOS names must contain 1-%d non-NUL bytes", qlmdv_qdos_name_size);
        return false;
    }
    memset(file->name, 0, sizeof file->name);
    memcpy(file->name, name, name_len);
    file->name_len = (uint8_t)name_len;
    return true;
}

static void qlmdv_rewrite_file_header(qlmdv_file *file) {
    qlmdv_write_be32(file->header + qlmdv_qdos_length_offset, file->data_len + qlmdv_qdos_header_size);
    memset(file->header + qlmdv_qdos_name_length_offset, 0, 2 + qlmdv_qdos_name_size);
    qlmdv_write_be16(file->header + qlmdv_qdos_name_length_offset, file->name_len);
    memcpy(file->header + qlmdv_qdos_name_offset, file->name, file->name_len);
}

static void qlmdv_encode_sector(
    uint8_t *sector,
    uint8_t logical,
    const uint8_t medium_name[10],
    uint16_t random_id,
    uint8_t file_id,
    uint8_t block,
    const uint8_t *data
) {
    memset(sector, 0, qlay_sector_size);
    sector[10] = 0xFF;
    sector[11] = 0xFF;
    sector[qlay_sector_header_offset] = qlay_sector_header_flag;
    sector[qlay_sector_header_offset + 1] = logical;
    memcpy(sector + qlay_sector_header_offset + 2, medium_name, 10);
    qlmdv_write_be16(sector + qlay_sector_header_offset + 12, random_id);
    qlay_write_checksum(
        sector + qlay_sector_header_checksum_offset,
        qlay_checksum(sector + qlay_sector_header_offset, qlay_sector_header_size)
    );

    sector[38] = 0xFF;
    sector[39] = 0xFF;
    sector[qlay_block_header_offset] = file_id;
    sector[qlay_block_header_offset + 1] = block;
    qlay_write_checksum(
        sector + qlay_block_header_checksum_offset,
        qlay_checksum(sector + qlay_block_header_offset, qlay_block_header_size)
    );

    sector[50] = 0xFF;
    sector[51] = 0xFF;
    if (data != NULL) {
        memcpy(sector + qlay_data_offset, data, qlay_data_size);
    }
    qlay_write_checksum(
        sector + qlay_data_checksum_offset,
        qlay_checksum(sector + qlay_data_offset, qlay_data_size)
    );
    memset(sector + qlay_gap_offset, 0x5A, qlay_sector_size - qlay_gap_offset);
}

static bool qlmdv_image_rebuild(
    qlmdv_image *image,
    uint8_t **rebuilt_bytes,
    uint32_t *rebuilt_len,
    char *error,
    uint32_t error_size
) {
    uint32_t directory_len = (image->file_count + 1) * qlmdv_qdos_header_size;
    uint32_t directory_blocks = (directory_len + qlay_data_size - 1) / qlay_data_size;
    uint64_t needed_blocks = directory_blocks;
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        qlmdv_file *file = &image->files[i];
        if (file->data_len > UINT32_MAX - qlmdv_qdos_header_size) {
            (void)snprintf(error, error_size, "file is too large for a QDOS header");
            return false;
        }
        uint32_t total_len = file->data_len + qlmdv_qdos_header_size;
        needed_blocks += ((uint64_t)total_len + qlay_data_size - 1) / qlay_data_size;
    }

    uint8_t good_sectors[qlay_max_sectors];
    uint32_t good_count = 0;
    for (uint32_t logical = 1; logical < image->sector_count; logical += 1) {
        uint8_t map_file = image->map[logical * 2];
        if (!qlmdv_file_id_is_bad(map_file) && image->logical_to_physical[logical] >= 0) {
            good_sectors[good_count] = (uint8_t)logical;
            good_count += 1;
        }
    }
    if (needed_blocks > good_count) {
        (void)snprintf(
            error,
            error_size,
            "image capacity exceeded: %" PRIu64 " data block(s) needed, %" PRIu32 " available",
            (unsigned long)needed_blocks,
            good_count
        );
        return false;
    }

    uint8_t *directory = arena_alloc_zero(directory_blocks, qlay_data_size);
    uint8_t *bytes = arena_alloc(image->byte_count);
    if (directory == NULL || bytes == NULL) {
        arena_release(directory);
        arena_release(bytes);
        (void)snprintf(error, error_size, "out of memory while rebuilding image");
        return false;
    }
    memcpy(bytes, image->bytes, image->byte_count);
    memcpy(directory, image->directory_header, qlmdv_qdos_header_size);
    qlmdv_write_be32(directory + qlmdv_qdos_length_offset, directory_len);
    memset(directory + qlmdv_qdos_name_length_offset, 0, 2 + qlmdv_qdos_name_size);
    for (uint32_t i = 0; i < image->file_count; i += 1) {
        qlmdv_rewrite_file_header(&image->files[i]);
        memcpy(
            directory + (i + 1) * qlmdv_qdos_header_size,
            image->files[i].header,
            qlmdv_qdos_header_size
        );
    }

    uint8_t map[qlay_data_size];
    memset(map, 0, sizeof map);
    for (uint32_t logical = 0; logical < image->sector_count; logical += 1) {
        uint8_t old_file = image->map[logical * 2];
        if (qlmdv_file_id_is_bad(old_file)) {
            map[logical * 2] = old_file;
        } else {
            map[logical * 2] = qlay_free_file_id;
        }
    }
    uint8_t map_file_id = image->map_file_id;
    if (!qlmdv_file_id_is_map(map_file_id)) {
        map_file_id = qlay_map_file_id;
    }
    map[0] = map_file_id;
    map[1] = 0;

    uint32_t allocation = 0;
    for (uint32_t block = 0; block < directory_blocks; block += 1) {
        uint8_t logical = good_sectors[allocation];
        allocation += 1;
        map[(uint32_t)logical * 2] = 0;
        map[(uint32_t)logical * 2 + 1] = (uint8_t)block;
    }
    for (uint32_t file_index = 0; file_index < image->file_count; file_index += 1) {
        qlmdv_file *file = &image->files[file_index];
        uint32_t total_len = file->data_len + qlmdv_qdos_header_size;
        uint32_t blocks = (uint32_t)(((uint64_t)total_len + qlay_data_size - 1) / qlay_data_size);
        for (uint32_t block = 0; block < blocks; block += 1) {
            uint8_t logical = good_sectors[allocation];
            allocation += 1;
            map[(uint32_t)logical * 2] = (uint8_t)(file_index + 1);
            map[(uint32_t)logical * 2 + 1] = (uint8_t)block;
        }
    }
    if (allocation != 0) {
        map[qlay_data_size - 1] = good_sectors[allocation - 1];
    }

    for (uint32_t logical = 0; logical < image->sector_count; logical += 1) {
        int16_t physical = image->logical_to_physical[logical];
        if (physical < 0 || qlmdv_file_id_is_bad(map[logical * 2])) {
            continue;
        }
        uint8_t *sector = bytes + (uint32_t)physical * qlay_sector_size;
        uint8_t file_id = map[logical * 2];
        uint8_t block = map[logical * 2 + 1];
        uint8_t payload[qlay_data_size] = { 0 };
        if (logical == 0) {
            memcpy(payload, map, sizeof payload);
            file_id = map_file_id;
            block = 0;
        }
        if (logical != 0 && file_id == 0) {
            memcpy(payload, directory + (uint32_t)block * qlay_data_size, sizeof payload);
        }
        if (logical != 0 && file_id != 0 && file_id <= image->file_count) {
            qlmdv_file *file = &image->files[file_id - 1];
            uint32_t offset = (uint32_t)block * qlay_data_size;
            if (offset < qlmdv_qdos_header_size) {
                uint32_t header_count = qlmdv_qdos_header_size - offset;
                if (header_count > qlay_data_size) {
                    header_count = qlay_data_size;
                }
                memcpy(payload, file->header + offset, header_count);
            }
            uint32_t block_end = offset + qlay_data_size;
            if (block_end > qlmdv_qdos_header_size && offset < file->data_len + qlmdv_qdos_header_size) {
                uint32_t stream_start = offset;
                if (stream_start < qlmdv_qdos_header_size) {
                    stream_start = qlmdv_qdos_header_size;
                }
                uint32_t stream_end = block_end;
                uint32_t file_end = file->data_len + qlmdv_qdos_header_size;
                if (stream_end > file_end) {
                    stream_end = file_end;
                }
                uint32_t data_count = stream_end - stream_start;
                memcpy(
                    payload + stream_start - offset,
                    file->data + stream_start - qlmdv_qdos_header_size,
                    data_count
                );
            }
        }
        qlmdv_encode_sector(
            sector,
            (uint8_t)logical,
            image->medium_name,
            image->random_id,
            file_id,
            block,
            payload
        );
    }
    arena_release(directory);
    *rebuilt_bytes = bytes;
    *rebuilt_len = image->byte_count;
    return true;
}

static bool qlmdv_write_all_fd(int fd, const uint8_t *bytes, uint32_t byte_count, int *write_error) {
    uint32_t done = 0;
    while (done < byte_count) {
        ssize_t count = write(fd, bytes + done, byte_count - done);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            *write_error = errno_or_eio(errno);
            return false;
        }
        done += (uint32_t)count;
    }
    return true;
}

#ifdef QLMDV_TEST
static int64_t qlmdv_test_transaction_write_limit = -1;
static bool qlmdv_test_transaction_write_was_limited;
#endif

static bool qlmdv_write_transaction(
    char *path,
    const uint8_t *bytes,
    uint32_t byte_count,
    bool create,
    bool force,
    char *error,
    uint32_t error_size
) {
    struct stat existing;
    bool target_exists = lstat(path, &existing) == 0;
    if (!target_exists && errno != ENOENT) {
        (void)snprintf(error, error_size, "could not stat %s: %s", path, strerror(errno_or_eio(errno)));
        return false;
    }
    if (create && target_exists && !force) {
        (void)snprintf(error, error_size, "%s already exists; use --force to replace it", path);
        return false;
    }
    if (target_exists && !S_ISREG(existing.st_mode)) {
        (void)snprintf(error, error_size, "%s is not a regular file", path);
        return false;
    }
    if (!create && !target_exists) {
        (void)snprintf(error, error_size, "%s does not exist", path);
        return false;
    }

    uint32_t path_len = (uint32_t)strlen(path);
    if (path_len > max_config_string - sizeof ".tmp.XXXXXX") {
        (void)snprintf(error, error_size, "path is too long");
        return false;
    }
    char temp_path[max_config_string];
    (void)snprintf(temp_path, sizeof temp_path, "%s.tmp.XXXXXX", path);
    int fd = create_temporary(temp_path);
    if (fd < 0) {
        (void)snprintf(error, error_size, "could not create temporary sibling: %s", strerror(errno_or_eio(errno)));
        return false;
    }

    bool ok = true;
    int saved_error = 0;
    mode_t mode;
    if (target_exists) {
        mode = existing.st_mode & 07777;
    } else {
        mode_t mask = umask(0);
        (void)umask(mask);
        mode = 0666 & ~mask;
    }
    if (fchmod(fd, mode) != 0) {
        saved_error = errno_or_eio(errno);
        ok = false;
    }
    uint32_t write_len = byte_count;
#ifdef QLMDV_TEST
    if (
        qlmdv_test_transaction_write_limit >= 0 &&
        (uint64_t)write_len > (uint64_t)qlmdv_test_transaction_write_limit
    ) {
        write_len = (uint32_t)qlmdv_test_transaction_write_limit;
        qlmdv_test_transaction_write_was_limited = true;
    }
#endif
    if (ok && !qlmdv_write_all_fd(fd, bytes, write_len, &saved_error)) {
        ok = false;
    }
    if (ok && write_len != byte_count) {
        saved_error = EIO;
        ok = false;
    }
    if (ok && fsync(fd) != 0) {
        saved_error = errno_or_eio(errno);
        ok = false;
    }
    if (close(fd) != 0 && ok) {
        saved_error = errno_or_eio(errno);
        ok = false;
    }
    if (ok && rename(temp_path, path) != 0) {
        saved_error = errno_or_eio(errno);
        ok = false;
    }
    if (!ok) {
        (void)unlink(temp_path);
        (void)snprintf(error, error_size, "could not replace %s: %s", path, strerror(saved_error));
        return false;
    }
    return true;
}

static bool qlmdv_create_image_bytes(
    uint32_t sector_count,
    const uint8_t medium_name[10],
    uint16_t random_id,
    uint8_t **bytes_out,
    uint32_t *byte_count_out,
    char *error,
    uint32_t error_size
) {
    if (sector_count < 200 || sector_count > qlay_max_sectors) {
        (void)snprintf(error, error_size, "new images must contain 200-255 sectors");
        return false;
    }
    uint32_t byte_count = sector_count * qlay_sector_size;
    uint8_t *bytes = arena_alloc_zero(1, byte_count);
    if (bytes == NULL) {
        (void)snprintf(error, error_size, "out of memory while creating image");
        return false;
    }

    uint8_t map[qlay_data_size] = { 0 };
    for (uint32_t logical = 0; logical < sector_count; logical += 1) {
        map[logical * 2] = qlay_free_file_id;
    }
    map[0] = qlay_map_file_id;
    map[1] = 0;
    map[2] = 0;
    map[3] = 0;
    map[qlay_data_size - 1] = 1;

    uint8_t directory[qlay_data_size] = { 0 };
    qlmdv_write_be32(directory, qlmdv_qdos_header_size);
    for (uint32_t physical = 0; physical < sector_count; physical += 1) {
        uint32_t logical = 0;
        if (physical != 0) {
            logical = sector_count - physical;
        }
        uint8_t file_id = qlay_free_file_id;
        uint8_t block = 0;
        uint8_t payload[qlay_data_size] = { 0 };
        if (logical == 0) {
            file_id = qlay_map_file_id;
            memcpy(payload, map, sizeof payload);
        }
        if (logical == 1) {
            file_id = 0;
            memcpy(payload, directory, sizeof payload);
        }
        qlmdv_encode_sector(
            bytes + physical * qlay_sector_size,
            (uint8_t)logical,
            medium_name,
            random_id,
            file_id,
            block,
            payload
        );
    }
    *bytes_out = bytes;
    *byte_count_out = byte_count;
    return true;
}

static bool qlmdv_rebuild_and_write(
    qlmdv_image *image,
    char *path,
    bool create,
    bool force,
    char *error,
    uint32_t error_size
) {
    uint8_t *bytes = NULL;
    uint32_t byte_count = 0;
    if (!qlmdv_image_rebuild(image, &bytes, &byte_count, error, error_size)) {
        return false;
    }
    bool ok = qlmdv_write_transaction(path, bytes, byte_count, create, force, error, error_size);
    arena_release(bytes);
    return ok;
}

static uint64_t qlmdv_random_id_mix(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27;
    value *= UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}

static uint16_t qlmdv_random_id_fallback(
    uint64_t seconds,
    uint32_t nanoseconds,
    uint64_t process_id,
    uintptr_t address
) {
    uint64_t mixed = qlmdv_random_id_mix(seconds ^ UINT64_C(0x9E3779B97F4A7C15));
    mixed = qlmdv_random_id_mix(mixed ^ nanoseconds);
    mixed = qlmdv_random_id_mix(mixed ^ process_id);
    mixed = qlmdv_random_id_mix(mixed ^ (uint64_t)address);
    return (uint16_t)(mixed ^ (mixed >> 16) ^ (mixed >> 32) ^ (mixed >> 48));
}

static uint16_t qlmdv_random_id(void) {
    uint16_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t count = read_fd_retry(fd, &value, sizeof value);
        (void)close(fd);
        if (count == sizeof value) {
            return value;
        }
    }
    struct timespec now = { 0 };
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        now.tv_sec = time(NULL);
    }
    return qlmdv_random_id_fallback(
        (uint64_t)now.tv_sec,
        (uint32_t)now.tv_nsec,
        (uint64_t)(uintmax_t)getpid(),
        (uintptr_t)&value
    );
}

static void qlmdv_default_medium_name(char *path, uint8_t medium_name[10]) {
    memset(medium_name, ' ', 10);
    char *basename = strrchr(path, '/');
    if (basename == NULL) {
        basename = path;
    } else {
        basename += 1;
    }
    uint32_t length = (uint32_t)strlen(basename);
    if (length >= 4 && ascii_equal_ignore_case(basename + length - 4, ".mdv")) {
        length -= 4;
    }
    if (length > 10) {
        length = 10;
    }
    memcpy(medium_name, basename, length);
}
