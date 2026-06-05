#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include "app_types.h"

namespace aq {

class DataLogger {
public:
    bool begin();
    bool append(const SensorSample& sample);
    bool appendAlert(const String& message, uint32_t timestamp);
    size_t readDay(uint32_t dayEpoch, Metric metric, HistoryPoint* out, size_t capacity, size_t targetPoints);
    size_t readMonth(uint32_t nowEpoch, Metric metric, HistoryPoint* out, size_t capacity, size_t targetPoints);
    bool computeParticulateBaseline(uint32_t startEpoch, uint32_t endEpoch, BaselineResult& result);
    size_t readFilterDeviation(uint32_t nowEpoch, const AppSettings& settings, HistoryPoint* out, size_t capacity, FilterDeviationSummary* summary);
    String lastError() const { return lastError_; }

private:
    struct RunningMetric {
        int32_t sum = 0;
        int16_t min = INT16_MAX;
        int16_t max = INT16_MIN;
        void add(int16_t value);
        HourMetric finish(uint16_t count) const;
    };

    struct HourAccumulator {
        uint32_t hourStart = 0;
        uint16_t count = 0;
        RunningMetric co2;
        RunningMetric pm25;
        RunningMetric pm10;
        RunningMetric temp;
        RunningMetric hum;
        RunningMetric voc;
        RunningMetric nox;
        void reset(uint32_t newHour);
        void add(const LogRecord& record);
        HourSummaryRecord finish() const;
    };

    bool flushHourIfNeeded(uint32_t timestamp);
    bool writeSummary(const HourSummaryRecord& record);
    String dailyPath(uint32_t epoch) const;
    int16_t metricValue(const LogRecord& record, Metric metric) const;
    int16_t metricValue(const HourSummaryRecord& record, Metric metric) const;
    bool readRecordAt(File& file, size_t index, LogRecord& record);
    float trimmedMean(int16_t* values, size_t count, uint8_t trimPercent) const;

    bool ready_ = false;
    HourAccumulator hour_;
    String lastError_;
};

extern DataLogger Logger;

}  // namespace aq
