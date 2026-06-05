#include "logger.h"

#include <algorithm>
#include <time.h>

namespace aq {

DataLogger Logger;

namespace {
constexpr uint8_t kLogVersion = 1;

String two(int value) {
    return value < 10 ? "0" + String(value) : String(value);
}

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

}  // namespace aq
