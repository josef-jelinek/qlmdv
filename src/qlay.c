enum {
    qlay_sector_size = 686,
    qlay_sector_header_offset = 12,
    qlay_sector_header_size = 14,
    qlay_sector_header_checksum_offset = 26,
    qlay_block_preamble_offset = 28,
    qlay_block_gap_end_offset = 34,
    qlay_block_header_offset = 40,
    qlay_block_header_size = 2,
    qlay_block_header_checksum_offset = 42,
    qlay_data_preamble_offset = 44,
    qlay_data_offset = 52,
    qlay_data_size = 512,
    qlay_data_checksum_offset = 564,
    qlay_gap_offset = 566,
    qlay_sector_header_flag = 0xFF,
    qlay_checksum_seed = 0x0F0F,
    qlay_map_file_id = 0xF8,
    qlay_alternate_map_file_id = 0x80,
    qlay_free_file_id = 0xFD,
    qlay_bad_file_id = 0xFE,
    qlay_bad_file_id_alternate = 0xFF,
    qlay_max_sectors = 255
};

_Static_assert(
    qlay_sector_header_offset + qlay_sector_header_size == qlay_sector_header_checksum_offset,
    "QLAY sector header layout changed"
);
_Static_assert(
    qlay_block_header_offset + qlay_block_header_size == qlay_block_header_checksum_offset,
    "QLAY block header layout changed"
);
_Static_assert(
    qlay_data_offset + qlay_data_size == qlay_data_checksum_offset &&
        qlay_data_checksum_offset + 2 == qlay_gap_offset &&
        qlay_gap_offset < qlay_sector_size,
    "QLAY data layout changed"
);

static uint16_t qlay_checksum(const uint8_t *bytes, uint32_t byte_count) {
    uint32_t checksum = qlay_checksum_seed;
    for (uint32_t i = 0; i < byte_count; i += 1) {
        checksum = (checksum + bytes[i]) & UINT16_MAX;
    }
    return (uint16_t)checksum;
}

static uint16_t qlay_read_checksum(const uint8_t *bytes) {
    return (uint16_t)((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8));
}

static void qlay_write_checksum(uint8_t *bytes, uint16_t checksum) {
    bytes[0] = (uint8_t)checksum;
    bytes[1] = (uint8_t)(checksum >> 8);
}
