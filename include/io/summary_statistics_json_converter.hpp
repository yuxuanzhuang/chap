#ifndef SUMMARY_STATISTICS_JSON_CONVERTER_HPP
#define SUMMARY_STATISTICS_JSON_CONVERTER_HPP

#include "rapidjson/allocators.h"
#include "rapidjson/document.h"

#include "statistics/summary_statistics.hpp"

/*
 *
 */
class SummaryStatisticsJsonConverter
{
    public:

        // conversion functionality:
        rapidjson::Value convert(
                const SummaryStatistics &sumStats,
                rapidjson::Document::AllocatorType &alloc);

    private:



};

#endif

