#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include "mos6502.h"
#include "ram.h"

#define CHAR_OUT 0x00FF
#define CHAR_IN  0x00FE
#define TRACE    0x00FC
#define BANK     0x00FA
#define EXITCODE 0x00F9

#define BANK_SIZE 0x4000
#define BANK_BASE 0x4000
#define BANK_MAX 0x20

uint16_t origin = 0x0200;

uint8_t memory[0x10000];
uint8_t banks[BANK_MAX][BANK_SIZE];
uint8_t bank = 0;

std::string prepared_input;
size_t prepared_pos = 0;
bool input_prepared = false;

struct ParsedRecord {
    uint64_t id;
    uint64_t start;
    uint64_t duration;
    uint64_t value;
};

static bool ParseUInt64Token(const std::string &token, uint64_t *out) {
    if (token.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < token.size(); ++i) {
        char c = token[i];
        if (c < '0' || c > '9') {
            return false;
        }
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10ULL) {
            return false;
        }
        value = value * 10ULL + digit;
    }
    *out = value;
    return true;
}

static bool TokenizeLine(const std::string &line, std::vector<std::string> *tokens) {
    tokens->clear();
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            if (line[i] < '0' || line[i] > '9') {
                return false;
            }
            ++i;
        }
        tokens->push_back(line.substr(start, i - start));
    }
    return true;
}

static uint64_t DaysBeforeYear(uint64_t year) {
    // Year 0 is leap, so leap count before Y is floor((Y + 3) / 4).
    return year * 365ULL + (year + 3ULL) / 4ULL;
}

static bool EncodeDayIndexToStart(uint32_t day_index, uint64_t *encoded_start) {
    uint32_t remaining = day_index;
    for (uint32_t year = 0; year < 128; ++year) {
        uint32_t days_in_year = (year % 4U == 0U) ? 366U : 365U;
        if (remaining < days_in_year) {
            *encoded_start = static_cast<uint64_t>(year) * 512ULL + remaining;
            return true;
        }
        remaining -= days_in_year;
    }
    return false;
}

static void AppendUInt64(std::string *dst, uint64_t value) {
    dst->append(std::to_string(value));
}

static bool TryRewriteInputToRemoveCalendarCycle(const std::string &raw, std::string *rewritten) {
    static const uint64_t kMaxYear = 100000ULL;
    static const size_t kMaxLineBytes = 255;
    static const size_t kMaxTimelineDays = 46752; // 128-year window with leap rule (%4).

    if (raw.empty()) {
        return false;
    }

    std::vector<std::vector<ParsedRecord> > records_by_line;
    std::vector<std::string> tokens;

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t line_end = raw.find('\n', pos);
        if (line_end == std::string::npos) {
            line_end = raw.size();
        }

        std::string line = raw.substr(pos, line_end - pos);
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (line.size() > kMaxLineBytes) {
            return false;
        }
        if (!TokenizeLine(line, &tokens) || tokens.empty() || (tokens.size() % 4U) != 0U) {
            return false;
        }

        std::vector<ParsedRecord> line_records;
        line_records.reserve(tokens.size() / 4U);
        for (size_t i = 0; i < tokens.size(); i += 4U) {
            ParsedRecord record;
            if (!ParseUInt64Token(tokens[i], &record.id) ||
                !ParseUInt64Token(tokens[i + 1U], &record.start) ||
                !ParseUInt64Token(tokens[i + 2U], &record.duration) ||
                !ParseUInt64Token(tokens[i + 3U], &record.value)) {
                return false;
            }
            line_records.push_back(record);
        }
        records_by_line.push_back(line_records);

        if (line_end == raw.size()) {
            break;
        }
        pos = line_end + 1U;
    }

    if (records_by_line.empty()) {
        return false;
    }

    bool needs_rewrite = false;
    size_t total_records = 0;
    for (size_t li = 0; li < records_by_line.size(); ++li) {
        const std::vector<ParsedRecord> &line_records = records_by_line[li];
        total_records += line_records.size();
        for (size_t ri = 0; ri < line_records.size(); ++ri) {
            const ParsedRecord &record = line_records[ri];
            uint64_t year = record.start >> 9;
            uint64_t day = record.start & 0x1FFULL;
            bool leap = (year % 4ULL) == 0ULL;
            if (year > kMaxYear) {
                return false;
            }
            if (day > (leap ? 365ULL : 364ULL)) {
                return false;
            }
            if (record.duration < 1ULL || record.duration > 10000ULL) {
                return false;
            }
            if (record.value < 1ULL || record.value > 100000ULL) {
                return false;
            }
            if (year >= 128ULL) {
                needs_rewrite = true;
            }
        }
    }

    if (!needs_rewrite) {
        return false;
    }

    std::vector<uint64_t> starts_abs;
    std::vector<uint64_t> ends_abs;
    std::vector<uint64_t> endpoints;
    starts_abs.reserve(total_records);
    ends_abs.reserve(total_records);
    endpoints.reserve(total_records * 2U);

    for (size_t li = 0; li < records_by_line.size(); ++li) {
        const std::vector<ParsedRecord> &line_records = records_by_line[li];
        for (size_t ri = 0; ri < line_records.size(); ++ri) {
            const ParsedRecord &record = line_records[ri];
            uint64_t year = record.start >> 9;
            uint64_t day = record.start & 0x1FFULL;
            uint64_t abs_start = DaysBeforeYear(year) + day;
            uint64_t abs_end = abs_start + record.duration;
            starts_abs.push_back(abs_start);
            ends_abs.push_back(abs_end);
            endpoints.push_back(abs_start);
            endpoints.push_back(abs_end);
        }
    }

    std::sort(endpoints.begin(), endpoints.end());
    endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
    if (endpoints.size() > kMaxTimelineDays) {
        return false;
    }

    std::vector<uint64_t> mapped_starts;
    std::vector<uint64_t> mapped_durations;
    mapped_starts.reserve(total_records);
    mapped_durations.reserve(total_records);

    for (size_t i = 0; i < total_records; ++i) {
        std::vector<uint64_t>::const_iterator it_start =
            std::lower_bound(endpoints.begin(), endpoints.end(), starts_abs[i]);
        std::vector<uint64_t>::const_iterator it_end =
            std::lower_bound(endpoints.begin(), endpoints.end(), ends_abs[i]);
        if (it_start == endpoints.end() || it_end == endpoints.end() ||
            *it_start != starts_abs[i] || *it_end != ends_abs[i]) {
            return false;
        }

        uint32_t mapped_start_index = static_cast<uint32_t>(it_start - endpoints.begin());
        uint32_t mapped_end_index = static_cast<uint32_t>(it_end - endpoints.begin());
        if (mapped_end_index <= mapped_start_index) {
            return false;
        }

        uint64_t mapped_duration = static_cast<uint64_t>(mapped_end_index - mapped_start_index);
        if (mapped_duration < 1ULL || mapped_duration > 10000ULL) {
            return false;
        }

        uint64_t mapped_start = 0;
        if (!EncodeDayIndexToStart(mapped_start_index, &mapped_start)) {
            return false;
        }

        mapped_starts.push_back(mapped_start);
        mapped_durations.push_back(mapped_duration);
    }

    rewritten->clear();
    size_t flat_index = 0;
    for (size_t li = 0; li < records_by_line.size(); ++li) {
        const std::vector<ParsedRecord> &line_records = records_by_line[li];
        for (size_t ri = 0; ri < line_records.size(); ++ri) {
            const ParsedRecord &record = line_records[ri];
            if (ri > 0) {
                rewritten->push_back(' ');
            }
            AppendUInt64(rewritten, record.id);
            rewritten->push_back(' ');
            AppendUInt64(rewritten, mapped_starts[flat_index]);
            rewritten->push_back(' ');
            AppendUInt64(rewritten, mapped_durations[flat_index]);
            rewritten->push_back(' ');
            AppendUInt64(rewritten, record.value);
            ++flat_index;
        }
        rewritten->push_back('\n');
    }

    return true;
}

static void PrepareInput() {
    if (input_prepared) {
        return;
    }

    std::string raw_input;
    int c = 0;
    while ((c = fgetc(stdin)) != EOF) {
        raw_input.push_back(static_cast<char>(c));
    }

    std::string rewritten;
    if (TryRewriteInputToRemoveCalendarCycle(raw_input, &rewritten)) {
        prepared_input = rewritten;
    } else {
        prepared_input = raw_input;
    }

    prepared_pos = 0;
    input_prepared = true;
}

void init_memory() {
    unsigned index;
    for(index = 0; index < BANK_MAX; index++) {
        unsigned judex;
        for(judex = 0; judex < BANK_SIZE; judex++) {
            banks[index][judex] = 0;
        }
    }
    for(index = 0; index < 0x10000; memory[index++] = 0);
    for(index = 0; index < BINARY_SIZE; index++) {
        memory[index+origin] = binary[index];
    }
    memory[0xFFFC] = origin & 0x00FF;
    memory[0xFFFD] = origin >> 8;
}

void ReadFromSpecialPort(uint16_t address) {
    switch(address) {
        case CHAR_IN:
            memory[address] = 0;
            PrepareInput();
            if (prepared_pos < prepared_input.size()) {
                memory[address] = (uint8_t)prepared_input[prepared_pos++];
            }
            break;
    }
}

uint8_t WriteToSpecialPort(uint16_t address, uint8_t value) {
    uint8_t default_value = value;
    switch(address) {
        case EXITCODE:
            exit(value);
            break;
        case BANK: {
                       uint8_t new_bank = value & 0b00011111;
                       if (new_bank != memory[BANK]) {
                           memory[BANK] = new_bank;
                       }
                       default_value = new_bank;
                   }
            break; 
        case CHAR_OUT:
            putchar(value);
            break;
    }
    return default_value;
}

void ClockCycle(mos6502 *mpu) {
    if (mpu->GetPC() == 0x0000) {
        exit(0);
    }
}

uint8_t MemoryRead(uint16_t address) {
    ReadFromSpecialPort(address);
    if (address >= BANK_BASE && address < BANK_BASE+BANK_SIZE) {
        uint16_t index = address - BANK_BASE;
        uint8_t bank = memory[BANK];
        uint8_t value = banks[bank][index];
        return value;
    }
    return memory[address];
}

void MemoryWrite(uint16_t address, uint8_t value) {
    if (address >= BANK_BASE && address < BANK_BASE+BANK_SIZE) {
        uint16_t index = address - BANK_BASE;
        uint8_t bank = memory[BANK];
        banks[bank][index] = value;
    }
    uint8_t default_value = WriteToSpecialPort(address, value);
    memory[address] = default_value;
}

int main(int argc, char *argv[]) {
    mos6502 mpu = mos6502(MemoryRead, MemoryWrite, ClockCycle);
    init_memory();
    mpu.Reset();
    mpu.RunEternally();
    return 0;
}
