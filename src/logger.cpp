#include "logger.h"

#include <algorithm>
#include <time.h>

namespace aq {

DataLogger Logger;

namespace {
constexpr uint8_t kLogVersion = 1;
constexpr size_t kMaxBaselineHours = 168;
constexpr size_t kHoursPerDay = 24;

String two(int value) {
    return value < 10 ? "0" + String(value) : String(value);
}

struct DayBucket {
    uint32_t dayStart = 0;
    int16_t deviation[kHoursPerDay] = {};
    uint8_t count = 0;
};

String dateStamp(uint32_t epoch) {
    time_t t = epoch;
    struct tm tmv {};
    localtime_r(&t, &tmv);
    return String(tmv.tm_year + 1900) + two(tmv.tm_mon + 1) + two(tmv.tm_mday);
}

uint32_t hourStart(uint32_t epoch) {
    return epoch - (epoch % 3600UL);
}
}

void DataLogger::RunningMetric::add(int16_t value) {
    if (value == INT16_MAX || value == INT16_MIN) {
        return;
    }
    sum += value;
    min = std::min(min, value);
    max = std::max(max, value);
}

HourMetric DataLogger::RunningMetric::finish(uint16_t count) const {
    if (count == 0 || min == INT16_MAX) {
        return {INT16_MAX, INT16_MAX, INT16_MAX};
    }
    return {static_cast<int16_t>(sum / count), min, max};
}

void DataLogger::HourAccumulator::reset(uint32_t newHour) {
    *this = HourAccumulator{};
    hourStart = newHour;
}

void DataLogger::HourAccumulator::add(const LogRecord& record) {
    if (count == 0) {
        hourStart = aq::hourStart(record.timestamp);
    }
    count++;
    co2.add(static_cast<int16_t>(record.co2));
    pm25.add(static_cast<int16_t>(record.pm25_x10));
    pm10.add(static_cast<int16_t>(record.pm10_x10));
    temp.add(record.temp_x100);
    hum.add(static_cast<int16_t>(record.hum_x100));
    voc.add(static_cast<int16_t>(record.voc_x10));
    nox.add(static_cast<int16_t>(record.nox_x10));
}

HourSummaryRecord DataLogger::HourAccumulator::finish() const {
    return {
        hourStart,
        co2.finish(count),
        pm25.finish(count),
        pm10.finish(count),
        temp.finish(count),
        hum.finish(count),
        voc.finish(count),
        nox.finish(count),
        count,
    };
}

bool DataLogger::begin() {
    if (!SD.exists("/log")) {
        SD.mkdir("/log");
    }
    ready_ = SD.exists("/log");
    if (!ready_) {
        lastError_ = "Log directory unavailable";
    }
    return ready_;
}

bool DataLogger::append(const SensorSample& sample) {
    if (!ready_ || !sample.valid) {
        return false;
    }

    const LogRecord record {
        sample.timestamp,
        sample.co2,
        clampU16(sample.pm2p5, 10.0f),
        clampU16(sample.pm10p0, 10.0f),
        clampI16(sample.temperature, 100.0f),
        clampU16(sample.humidity, 100.0f),
        clampU16(sample.vocIndex, 10.0f),
        clampU16(sample.noxIndex, 10.0f),
        clampU16(sample.pm1p0, 10.0f),
        clampU16(sample.pm4p0, 10.0f),
        kLogVersion,
        100,
    };

    flushHourIfNeeded(sample.timestamp);

    File file = SD.open(dailyPath(sample.timestamp), FILE_APPEND);
    if (!file) {
        lastError_ = "Daily log open failed";
        return false;
    }
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    file.close();
    if (written != sizeof(record)) {
        lastError_ = "Daily log write short";
        return false;
    }

    hour_.add(record);
    return true;
}

bool DataLogger::appendAlert(const String& message, uint32_t timestamp) {
    File file = SD.open("/alerts.log", FILE_APPEND);
    if (!file) {
        return false;
    }
    time_t t = timestamp;
    struct tm tmv {};
    localtime_r(&t, &tmv);
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    file.print(prefix);
    file.println(message);
    file.close();
    return true;
}

size_t DataLogger::readDay(uint32_t dayEpoch, Metric metric, HistoryPoint* out, size_t capacity, size_t targetPoints) {
    if (!out || capacity == 0) {
        return 0;
    }
    File file = SD.open(dailyPath(dayEpoch), FILE_READ);
    if (!file) {
        return 0;
    }
    const size_t total = file.size() / sizeof(LogRecord);
    const size_t stride = std::max<size_t>(1, total / std::max<size_t>(1, std::min(capacity, targetPoints)));
    size_t count = 0;
    for (size_t i = 0; i < total && count < capacity; i += stride) {
        LogRecord record;
        if (!readRecordAt(file, i, record)) {
            break;
        }
        out[count].timestamp = record.timestamp;
        out[count].value = metricValue(record, metric);
        count++;
    }
    file.close();
    return count;
}

size_t DataLogger::readMonth(uint32_t nowEpoch, Metric metric, HistoryPoint* out, size_t capacity, size_t targetPoints) {
    if (!out || capacity == 0) {
        return 0;
    }
    File file = SD.open("/log/monthly_summary.dat", FILE_READ);
    if (!file) {
        return 0;
    }
    const size_t total = file.size() / sizeof(HourSummaryRecord);
    const size_t stride = std::max<size_t>(1, total / std::max<size_t>(1, std::min(capacity, targetPoints)));
    size_t count = 0;
    const uint32_t oldest = nowEpoch > 30UL * 24UL * 3600UL ? nowEpoch - 30UL * 24UL * 3600UL : 0;
    for (size_t i = 0; i < total && count < capacity; i += stride) {
        HourSummaryRecord record;
        if (!file.seek(i * sizeof(record)) || file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
            break;
        }
        if (record.hourStart >= oldest) {
            out[count].timestamp = record.hourStart;
            out[count].value = metricValue(record, metric);
            count++;
        }
    }
    file.close();
    return count;
}

bool DataLogger::computeParticulateBaseline(uint32_t startEpoch, uint32_t endEpoch, BaselineResult& result) {
    result = BaselineResult{};
    if (endEpoch <= startEpoch) {
        return false;
    }
    File file = SD.open("/log/monthly_summary.dat", FILE_READ);
    if (!file) {
        lastError_ = "Summary log unavailable for baseline";
        return false;
    }

    int16_t pm25[kMaxBaselineHours];
    int16_t pm10[kMaxBaselineHours];
    size_t count = 0;
    const size_t total = file.size() / sizeof(HourSummaryRecord);
    for (size_t i = 0; i < total && count < kMaxBaselineHours; ++i) {
        HourSummaryRecord record;
        if (!file.seek(i * sizeof(record)) || file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
            break;
        }
        if (record.hourStart < startEpoch || record.hourStart >= endEpoch || record.count == 0) {
            continue;
        }
        if (record.pm25_x10.mean == INT16_MAX || record.pm10_x10.mean == INT16_MAX) {
            continue;
        }
        pm25[count] = record.pm25_x10.mean;
        pm10[count] = record.pm10_x10.mean;
        count++;
    }
    file.close();

    if (count < 12) {
        lastError_ = "Need at least 12 hourly baseline points";
        return false;
    }

    result.pm25 = trimmedMean(pm25, count, 10) / 10.0f;
    result.pm10 = trimmedMean(pm10, count, 10) / 10.0f;
    result.hours = static_cast<uint16_t>(count);
    result.valid = isfinite(result.pm25) && isfinite(result.pm10) && result.pm25 > 0.1f && result.pm10 > 0.1f;
    return result.valid;
}

size_t DataLogger::readFilterDeviation(uint32_t nowEpoch, const AppSettings& settings, HistoryPoint* out, size_t capacity, FilterDeviationSummary* summary) {
    if (summary) {
        *summary = FilterDeviationSummary{};
    }
    if (!out || capacity == 0 || !settings.filterBaselineReady ||
        !isfinite(settings.filterBaselinePm25) || !isfinite(settings.filterBaselinePm10) ||
        settings.filterBaselinePm25 <= 0.1f || settings.filterBaselinePm10 <= 0.1f) {
        return 0;
    }

    File file = SD.open("/log/monthly_summary.dat", FILE_READ);
    if (!file) {
        return 0;
    }

    DayBucket bucket;
    size_t count = 0;
    int32_t sum = 0;
    int16_t peak = INT16_MIN;
    const uint32_t oldest = nowEpoch > 30UL * 24UL * 3600UL ? nowEpoch - 30UL * 24UL * 3600UL : 0;
    const size_t total = file.size() / sizeof(HourSummaryRecord);

    auto flushDay = [&]() {
        if (bucket.count == 0 || count >= capacity) {
            bucket = DayBucket{};
            return;
        }
        const float trimmed = trimmedMean(bucket.deviation, bucket.count, 15);
        const int16_t pct = static_cast<int16_t>(lroundf(trimmed));
        out[count].timestamp = bucket.dayStart;
        out[count].value = pct;
        count++;
        sum += pct;
        peak = std::max(peak, pct);
        bucket = DayBucket{};
    };

    for (size_t i = 0; i < total; ++i) {
        HourSummaryRecord record;
        if (!file.seek(i * sizeof(record)) || file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
            break;
        }
        if (record.hourStart < oldest || record.count == 0 ||
            record.pm25_x10.mean == INT16_MAX || record.pm10_x10.mean == INT16_MAX) {
            continue;
        }

        const uint32_t day = record.hourStart - (record.hourStart % 86400UL);
        if (bucket.count > 0 && bucket.dayStart != day) {
            flushDay();
        }
        if (bucket.count == 0) {
            bucket.dayStart = day;
        }

        const float pm25 = record.pm25_x10.mean / 10.0f;
        const float pm10 = record.pm10_x10.mean / 10.0f;
        const float pm25Pct = ((pm25 - settings.filterBaselinePm25) / settings.filterBaselinePm25) * 100.0f;
        const float pm10Pct = ((pm10 - settings.filterBaselinePm10) / settings.filterBaselinePm10) * 100.0f;
        const float combined = std::max(pm25Pct, pm10Pct);
        bucket.deviation[bucket.count++] = static_cast<int16_t>(lroundf(std::max(-50.0f, std::min(250.0f, combined))));
        if (bucket.count >= kHoursPerDay) {
            flushDay();
        }
    }
    flushDay();
    file.close();

    if (summary && count > 0) {
        summary->days = static_cast<uint16_t>(count);
        summary->currentPercent = out[count - 1].value;
        summary->peakPercent = peak;
        summary->meanPercent = static_cast<int16_t>(sum / static_cast<int32_t>(count));
        summary->valid = true;
    }
    return count;
}

bool DataLogger::flushHourIfNeeded(uint32_t timestamp) {
    const uint32_t currentHour = hourStart(timestamp);
    if (hour_.count == 0) {
        hour_.reset(currentHour);
        return true;
    }
    if (hour_.hourStart == currentHour) {
        return true;
    }
    const bool ok = writeSummary(hour_.finish());
    hour_.reset(currentHour);
    return ok;
}

bool DataLogger::writeSummary(const HourSummaryRecord& record) {
    if (record.count == 0) {
        return true;
    }
    File file = SD.open("/log/monthly_summary.dat", FILE_APPEND);
    if (!file) {
        lastError_ = "Summary log open failed";
        return false;
    }
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    file.close();
    if (written != sizeof(record)) {
        lastError_ = "Summary log write short";
        return false;
    }
    return true;
}

String DataLogger::dailyPath(uint32_t epoch) const {
    return "/log/" + dateStamp(epoch) + ".dat";
}

int16_t DataLogger::metricValue(const LogRecord& record, Metric metric) const {
    switch (metric) {
        case Metric::Co2: return static_cast<int16_t>(record.co2);
        case Metric::Pm25: return static_cast<int16_t>(record.pm25_x10);
        case Metric::Pm10: return static_cast<int16_t>(record.pm10_x10);
        case Metric::Temperature: return record.temp_x100;
        case Metric::Humidity: return static_cast<int16_t>(record.hum_x100);
        case Metric::Voc: return static_cast<int16_t>(record.voc_x10);
        case Metric::Nox: return static_cast<int16_t>(record.nox_x10);
        case Metric::Pm1: return static_cast<int16_t>(record.pm1_x10);
        case Metric::Pm4: return static_cast<int16_t>(record.pm4_x10);
    }
    return INT16_MAX;
}

int16_t DataLogger::metricValue(const HourSummaryRecord& record, Metric metric) const {
    switch (metric) {
        case Metric::Co2: return record.co2.mean;
        case Metric::Pm25: return record.pm25_x10.mean;
        case Metric::Pm10: return record.pm10_x10.mean;
        case Metric::Temperature: return record.temp_x100.mean;
        case Metric::Humidity: return record.hum_x100.mean;
        case Metric::Voc: return record.voc_x10.mean;
        case Metric::Nox: return record.nox_x10.mean;
        case Metric::Pm1:
        case Metric::Pm4: return INT16_MAX;
    }
    return INT16_MAX;
}

bool DataLogger::readRecordAt(File& file, size_t index, LogRecord& record) {
    if (!file.seek(index * sizeof(record))) {
        return false;
    }
    return file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record);
}

float DataLogger::trimmedMean(int16_t* values, size_t count, uint8_t trimPercent) const {
    if (!values || count == 0) {
        return NAN;
    }
    std::sort(values, values + count);
    size_t trim = (count * trimPercent) / 100;
    if (count < 10) {
        trim = 0;
    }
    if (trim * 2 >= count) {
        trim = 0;
    }
    int32_t sum = 0;
    size_t used = 0;
    for (size_t i = trim; i < count - trim; ++i) {
        sum += values[i];
        used++;
    }
    return used > 0 ? sum / static_cast<float>(used) : NAN;
}

}  // namespace aq
