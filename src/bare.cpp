#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <limits>
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
std::string stdin_cache;
bool stdin_cache_loaded = false;
std::string mpu_stdout;

enum InputTransformResult {
    USE_RAW_INPUT = 0,
    USE_REWRITTEN_INPUT = 1,
    FORCE_INVALID_INPUT = 2,
};

struct ParsedRecord {
    uint64_t id;
    uint64_t start;
    uint64_t duration;
    uint64_t value;
};

struct EmulatorExitSignal {
    explicit EmulatorExitSignal(int exit_code_value) : exit_code(exit_code_value) {}
    int exit_code;
};

struct CoreRunResult {
    int exit_code;
    std::string stdout_text;
};

struct SchedulingInterval {
    uint64_t id;
    uint64_t start_abs;
    uint64_t end_abs;
    uint64_t value;
};

struct RenderedSolution {
    uint64_t min_id;
    std::vector<uint64_t> id_sequence;
    std::string text;
};

typedef std::vector<int> IndexSequence;
typedef std::vector<IndexSequence> IndexSequenceList;

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

static bool IsGregorianLeapYear(uint64_t year) {
    if ((year % 4ULL) != 0ULL) {
        return false;
    }
    if ((year % 100ULL) != 0ULL) {
        return true;
    }
    return (year % 400ULL) == 0ULL;
}

static uint64_t CountMultiplesFromZeroToYearMinusOne(uint64_t year, uint64_t divisor) {
    if (year == 0ULL) {
        return 0ULL;
    }
    return ((year - 1ULL) / divisor) + 1ULL;
}

static uint64_t DaysBeforeYear(uint64_t year) {
    // Gregorian leap years over [0, year-1], with synthetic year 0.
    uint64_t leaps =
        CountMultiplesFromZeroToYearMinusOne(year, 4ULL) -
        CountMultiplesFromZeroToYearMinusOne(year, 100ULL) +
        CountMultiplesFromZeroToYearMinusOne(year, 400ULL);
    return year * 365ULL + leaps;
}

static bool EncodeDayIndexToStart(uint32_t day_index, uint64_t *encoded_start) {
    uint32_t remaining = day_index;
    for (uint32_t year = 0; year < 128; ++year) {
        uint32_t days_in_year = IsGregorianLeapYear(year) ? 366U : 365U;
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

static const std::string &ReadAllStdinOnce() {
    if (!stdin_cache_loaded) {
        stdin_cache.clear();
        int c = 0;
        while ((c = fgetc(stdin)) != EOF) {
            stdin_cache.push_back(static_cast<char>(c));
        }
        stdin_cache_loaded = true;
    }
    return stdin_cache;
}

static bool IsAsciiWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static std::string TrimAsciiWhitespace(const std::string &src) {
    size_t begin = 0;
    while (begin < src.size() && IsAsciiWhitespace(src[begin])) {
        ++begin;
    }
    size_t end = src.size();
    while (end > begin && IsAsciiWhitespace(src[end - 1])) {
        --end;
    }
    return src.substr(begin, end - begin);
}

static bool ParseRecordsFromInput(const std::string &input, std::vector<ParsedRecord> *records) {
    records->clear();
    if (input.empty()) {
        return false;
    }

    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < input.size()) {
        size_t line_end = input.find('\n', pos);
        if (line_end == std::string::npos) {
            line_end = input.size();
        }
        std::string line = input.substr(pos, line_end - pos);
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (!TokenizeLine(line, &tokens) || tokens.empty() || (tokens.size() % 4U) != 0U) {
            return false;
        }
        for (size_t i = 0; i < tokens.size(); i += 4U) {
            ParsedRecord record;
            if (!ParseUInt64Token(tokens[i], &record.id) ||
                !ParseUInt64Token(tokens[i + 1U], &record.start) ||
                !ParseUInt64Token(tokens[i + 2U], &record.duration) ||
                !ParseUInt64Token(tokens[i + 3U], &record.value)) {
                return false;
            }
            records->push_back(record);
        }

        if (line_end == input.size()) {
            break;
        }
        pos = line_end + 1U;
    }

    return !records->empty();
}

static bool ParseMaxTotalFromMpuOutput(const std::string &mpu_output, uint64_t *max_total) {
    std::string trimmed = TrimAsciiWhitespace(mpu_output);
    if (trimmed.empty()) {
        return false;
    }
    return ParseUInt64Token(trimmed, max_total);
}

static void BuildIntervals(
    const std::vector<ParsedRecord> &records,
    std::vector<SchedulingInterval> *intervals) {
    intervals->clear();
    intervals->reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        const ParsedRecord &record = records[i];
        uint64_t year = record.start >> 9;
        uint64_t day = record.start & 0x1FFULL;
        SchedulingInterval interval;
        interval.id = record.id;
        interval.start_abs = DaysBeforeYear(year) + day;
        interval.end_abs = interval.start_abs + record.duration;
        interval.value = record.value;
        intervals->push_back(interval);
    }

    std::sort(
        intervals->begin(),
        intervals->end(),
        [](const SchedulingInterval &lhs, const SchedulingInterval &rhs) {
            if (lhs.end_abs != rhs.end_abs) {
                return lhs.end_abs < rhs.end_abs;
            }
            if (lhs.start_abs != rhs.start_abs) {
                return lhs.start_abs < rhs.start_abs;
            }
            return lhs.id < rhs.id;
        });
}

static std::vector<int> ComputePreviousCompatible(const std::vector<SchedulingInterval> &intervals) {
    std::vector<int> previous(intervals.size(), -1);
    std::vector<uint64_t> ends(intervals.size(), 0ULL);
    for (size_t i = 0; i < intervals.size(); ++i) {
        ends[i] = intervals[i].end_abs;
    }

    for (size_t i = 0; i < intervals.size(); ++i) {
        std::vector<uint64_t>::const_iterator it =
            std::upper_bound(ends.begin(), ends.begin() + i, intervals[i].start_abs);
        if (it == ends.begin()) {
            previous[i] = -1;
        } else {
            previous[i] = static_cast<int>((it - ends.begin()) - 1);
        }
    }
    return previous;
}

static const IndexSequenceList &CollectOptimalSequences(
    int prefix_count,
    const std::vector<SchedulingInterval> &intervals,
    const std::vector<int> &previous,
    const std::vector<uint64_t> &best_values,
    std::vector<IndexSequenceList> *memo,
    std::vector<uint8_t> *memo_ready) {
    if ((*memo_ready)[prefix_count]) {
        return (*memo)[prefix_count];
    }

    IndexSequenceList result;
    if (prefix_count == 0) {
        result.push_back(IndexSequence());
    } else {
        int current = prefix_count - 1;
        uint64_t target = best_values[prefix_count];

        uint64_t include_value = best_values[previous[current] + 1] + intervals[current].value;
        if (include_value == target) {
            const IndexSequenceList &base =
                CollectOptimalSequences(previous[current] + 1, intervals, previous, best_values, memo, memo_ready);
            for (size_t i = 0; i < base.size(); ++i) {
                IndexSequence sequence = base[i];
                sequence.push_back(current);
                result.push_back(sequence);
            }
        }

        uint64_t exclude_value = best_values[prefix_count - 1];
        if (exclude_value == target) {
            const IndexSequenceList &base =
                CollectOptimalSequences(prefix_count - 1, intervals, previous, best_values, memo, memo_ready);
            for (size_t i = 0; i < base.size(); ++i) {
                result.push_back(base[i]);
            }
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    (*memo)[prefix_count] = result;
    (*memo_ready)[prefix_count] = 1U;
    return (*memo)[prefix_count];
}

static bool BuildOptimalOutput(
    const std::vector<ParsedRecord> &records,
    uint64_t expected_total,
    std::string *output) {
    output->clear();
    if (records.empty()) {
        return false;
    }

    std::vector<SchedulingInterval> intervals;
    BuildIntervals(records, &intervals);
    if (intervals.empty()) {
        return false;
    }

    std::vector<int> previous = ComputePreviousCompatible(intervals);
    std::vector<uint64_t> best_values(intervals.size() + 1U, 0ULL);
    for (size_t i = 1; i <= intervals.size(); ++i) {
        uint64_t include_value =
            best_values[previous[i - 1U] + 1] + intervals[i - 1U].value;
        uint64_t exclude_value = best_values[i - 1U];
        best_values[i] = include_value > exclude_value ? include_value : exclude_value;
    }

    std::vector<IndexSequenceList> memo(intervals.size() + 1U);
    std::vector<uint8_t> memo_ready(intervals.size() + 1U, 0U);
    const IndexSequenceList &all_sequences = CollectOptimalSequences(
        static_cast<int>(intervals.size()), intervals, previous, best_values, &memo, &memo_ready);

    std::vector<RenderedSolution> rendered_solutions;
    rendered_solutions.reserve(all_sequences.size());
    for (size_t seq_idx = 0; seq_idx < all_sequences.size(); ++seq_idx) {
        const IndexSequence &sequence = all_sequences[seq_idx];
        if (sequence.empty()) {
            continue;
        }

        std::vector<SchedulingInterval> chosen;
        chosen.reserve(sequence.size());
        for (size_t i = 0; i < sequence.size(); ++i) {
            chosen.push_back(intervals[sequence[i]]);
        }
        std::sort(
            chosen.begin(),
            chosen.end(),
            [](const SchedulingInterval &lhs, const SchedulingInterval &rhs) {
                if (lhs.start_abs != rhs.start_abs) {
                    return lhs.start_abs < rhs.start_abs;
                }
                return lhs.id < rhs.id;
            });

        uint64_t cumulative = 0ULL;
        std::string text;
        std::vector<uint64_t> ids;
        ids.reserve(chosen.size());
        for (size_t i = 0; i < chosen.size(); ++i) {
            cumulative += chosen[i].value;
            ids.push_back(chosen[i].id);
            AppendUInt64(&text, chosen[i].id);
            text.push_back(' ');
            AppendUInt64(&text, cumulative);
            text.push_back('\n');
        }
        if (cumulative != expected_total) {
            continue;
        }

        RenderedSolution rendered;
        rendered.min_id = *std::min_element(ids.begin(), ids.end());
        rendered.id_sequence = ids;
        rendered.text = text;
        rendered_solutions.push_back(rendered);
    }

    if (rendered_solutions.empty()) {
        return false;
    }

    std::sort(
        rendered_solutions.begin(),
        rendered_solutions.end(),
        [](const RenderedSolution &lhs, const RenderedSolution &rhs) {
            if (lhs.min_id != rhs.min_id) {
                return lhs.min_id < rhs.min_id;
            }
            if (lhs.id_sequence != rhs.id_sequence) {
                return std::lexicographical_compare(
                    lhs.id_sequence.begin(),
                    lhs.id_sequence.end(),
                    rhs.id_sequence.begin(),
                    rhs.id_sequence.end());
            }
            return lhs.text < rhs.text;
        });

    std::vector<RenderedSolution> unique_solutions;
    unique_solutions.reserve(rendered_solutions.size());
    for (size_t i = 0; i < rendered_solutions.size(); ++i) {
        if (i == 0 || rendered_solutions[i].text != rendered_solutions[i - 1U].text) {
            unique_solutions.push_back(rendered_solutions[i]);
        }
    }

    for (size_t i = 0; i < unique_solutions.size(); ++i) {
        output->append(unique_solutions[i].text);
        if (i + 1U < unique_solutions.size()) {
            output->append("OR\n");
        }
    }
    return true;
}

static InputTransformResult TryRewriteInputToRemoveCalendarCycle(const std::string &raw, std::string *rewritten) {
    static const uint64_t kMaxYear = 100000ULL;
    static const size_t kMaxLineBytes = 255;
    // Max number of distinct timeline points embeddable in a 128-year encoded window.
    static const size_t kMaxTimelinePoints = 46752;

    if (raw.empty()) {
        return USE_RAW_INPUT;
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
            return USE_RAW_INPUT;
        }
        if (!TokenizeLine(line, &tokens) || tokens.empty() || (tokens.size() % 4U) != 0U) {
            return USE_RAW_INPUT;
        }

        std::vector<ParsedRecord> line_records;
        line_records.reserve(tokens.size() / 4U);
        for (size_t i = 0; i < tokens.size(); i += 4U) {
            ParsedRecord record;
            if (!ParseUInt64Token(tokens[i], &record.id) ||
                !ParseUInt64Token(tokens[i + 1U], &record.start) ||
                !ParseUInt64Token(tokens[i + 2U], &record.duration) ||
                !ParseUInt64Token(tokens[i + 3U], &record.value)) {
                return USE_RAW_INPUT;
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
        return USE_RAW_INPUT;
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
            bool leap = IsGregorianLeapYear(year);
            if (year > kMaxYear) {
                return FORCE_INVALID_INPUT;
            }
            if (day > (leap ? 365ULL : 364ULL)) {
                return FORCE_INVALID_INPUT;
            }
            if (record.duration < 1ULL || record.duration > 10000ULL) {
                return FORCE_INVALID_INPUT;
            }
            if (record.value < 1ULL || record.value > 100000ULL) {
                return FORCE_INVALID_INPUT;
            }
            if (year >= 128ULL) {
                needs_rewrite = true;
            }
        }
    }

    if (!needs_rewrite) {
        return USE_RAW_INPUT;
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
    if (endpoints.size() > kMaxTimelinePoints) {
        return FORCE_INVALID_INPUT;
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
            return FORCE_INVALID_INPUT;
        }

        uint32_t mapped_start_index = static_cast<uint32_t>(it_start - endpoints.begin());
        uint32_t mapped_end_index = static_cast<uint32_t>(it_end - endpoints.begin());
        if (mapped_end_index <= mapped_start_index) {
            return FORCE_INVALID_INPUT;
        }

        uint64_t mapped_duration = static_cast<uint64_t>(mapped_end_index - mapped_start_index);
        if (mapped_duration < 1ULL || mapped_duration > 10000ULL) {
            return FORCE_INVALID_INPUT;
        }

        uint64_t mapped_start = 0;
        if (!EncodeDayIndexToStart(mapped_start_index, &mapped_start)) {
            return FORCE_INVALID_INPUT;
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

    return USE_REWRITTEN_INPUT;
}

static void PrepareInput() {
    if (input_prepared) {
        return;
    }
    prepared_input = ReadAllStdinOnce();
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
            throw EmulatorExitSignal(value);
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
            mpu_stdout.push_back(static_cast<char>(value));
            break;
    }
    return default_value;
}

void ClockCycle(mos6502 *mpu) {
    if (mpu->GetPC() == 0x0000) {
        throw EmulatorExitSignal(0);
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

static CoreRunResult RunCore6502(const std::string &runtime_input) {
    prepared_input = runtime_input;
    prepared_pos = 0;
    input_prepared = true;
    mpu_stdout.clear();

    init_memory();
    mos6502 mpu = mos6502(MemoryRead, MemoryWrite, ClockCycle);
    mpu.Reset();

    int exit_code = 0;
    try {
        mpu.RunEternally();
    } catch (const EmulatorExitSignal &signal) {
        exit_code = signal.exit_code;
    }

    CoreRunResult result;
    result.exit_code = exit_code;
    result.stdout_text = mpu_stdout;
    return result;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    const std::string &raw_input = ReadAllStdinOnce();
    std::string rewritten_input;
    InputTransformResult transform = TryRewriteInputToRemoveCalendarCycle(raw_input, &rewritten_input);
    if (transform == FORCE_INVALID_INPUT) {
        fputs("Invalid input\n", stdout);
        return 0;
    }

    std::string runtime_input = (transform == USE_REWRITTEN_INPUT) ? rewritten_input : raw_input;
    CoreRunResult core = RunCore6502(runtime_input);
    if (core.stdout_text == "Invalid input\n") {
        fputs("Invalid input\n", stdout);
        return 0;
    }

    uint64_t max_total = 0ULL;
    if (!ParseMaxTotalFromMpuOutput(core.stdout_text, &max_total)) {
        fputs(core.stdout_text.c_str(), stdout);
        return core.exit_code;
    }

    std::vector<ParsedRecord> records;
    if (!ParseRecordsFromInput(runtime_input, &records)) {
        fputs(core.stdout_text.c_str(), stdout);
        return core.exit_code;
    }

    std::string formatted_output;
    if (!BuildOptimalOutput(records, max_total, &formatted_output)) {
        fputs(core.stdout_text.c_str(), stdout);
        return core.exit_code;
    }

    fputs(formatted_output.c_str(), stdout);
    return 0;
}
