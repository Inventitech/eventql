/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Paul Asmuth <paul@eventql.io>
 *   - Laura Schlimmer <laura@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <eventql/util/inspect.h>
#include <eventql/util/human.h>
#include <eventql/util/wallclock.h>
#include <eventql/sql/expressions/datetime.h>
#include <eventql/sql/svalue.h>

namespace csql {
namespace expressions {

enum class Unit {
  MINUTE_SECOND,
  HOUR_SECOND,
  HOUR_MINUTE,
  DAY_SECOND,
  DAY_MINUTE,
  DAY_HOUR,
  YEAR_MONTH
};

static const std::unordered_map<std::string, Unit> units = {
  {"minute_second", Unit::MINUTE_SECOND},
  {"hour_second", Unit::HOUR_SECOND},
  {"hour_minute", Unit::HOUR_MINUTE},
  {"day_second", Unit::DAY_SECOND},
  {"day_minute", Unit::DAY_MINUTE},
  {"day_hour", Unit::DAY_HOUR},
  {"year_month", Unit::YEAR_MONTH}
};

static const std::unordered_map<std::string, uint64_t> time_windows = {
  {"ms", kMicrosPerMilli},
  {"msec", kMicrosPerMilli},
  {"millisecond", kMicrosPerMilli},
  {"milliseconds", kMicrosPerMilli},
  {"s", kMicrosPerSecond},
  {"sec", kMicrosPerSecond},
  {"second", kMicrosPerSecond},
  {"seconds", kMicrosPerSecond},
  {"min", kMicrosPerMinute},
  {"minute", kMicrosPerMinute},
  {"minutes", kMicrosPerMinute},
  {"h", kMicrosPerHour},
  {"hour", kMicrosPerHour},
  {"hours", kMicrosPerHour},
  {"d", kMicrosPerDay},
  {"day", kMicrosPerDay},
  {"days", kMicrosPerDay},
  {"w", kMicrosPerWeek},
  {"week", kMicrosPerWeek},
  {"weeks", kMicrosPerWeek},
  {"month", kMicrosPerDay * 30},
  {"months", kMicrosPerDay * 30},
  {"y", kMicrosPerYear},
  {"year", kMicrosPerYear},
  {"years", kMicrosPerYear}
};

void now_call(sql_txn* ctx, VMStack* stack) {
  pushTimestamp64(stack, WallClock::unixMicros());
}

const SFunction now(
    {  },
    SType::TIMESTAMP64,
    &now_call);

void from_timestamp_int64_call(sql_txn* ctx, VMStack* stack) {
  auto value = popInt64(stack);
  pushTimestamp64(stack, value * kMicrosPerSecond);
}

const SFunction from_timestamp_int64(
    { SType::INT64 },
    SType::TIMESTAMP64,
    &from_timestamp_int64_call);

void from_timestamp_float64_call(sql_txn* ctx, VMStack* stack) {
  auto value = popFloat64(stack);
  pushTimestamp64(stack, value * kMicrosPerSecond);
}

const SFunction from_timestamp_float64(
    { SType::FLOAT64 },
    SType::TIMESTAMP64,
    &from_timestamp_float64_call);

void date_trunc_timestamp64_call(sql_txn* ctx, VMStack* stack) {
  auto timestamp = popTimestamp64(stack);
  auto window = popString(stack);
  uint64_t window_multiplicator;
  std::string window_name;
  try {
    size_t sz;
    window_multiplicator = std::stoull(window, &sz);
    window_name = window.substr(sz);
  } catch (const std::exception& e) {
    window_multiplicator = 1;
    window_name = window;
  }

  auto window_value = time_windows.find(window_name);
  if (window_value == time_windows.end()) {
    throw std::runtime_error(
        StringUtil::format("unknown time window $0", window));
  }

  auto truncater = window_value->second * window_multiplicator;
  auto truncated = ((uint64_t)timestamp / truncater) * truncater;
  pushTimestamp64(stack, truncated);
}

const SFunction date_trunc_timestamp64(
    { SType::STRING, SType::TIMESTAMP64 },
    SType::TIMESTAMP64,
    &date_trunc_timestamp64_call);

std::vector<uint64_t> parseUnitExpr(
    const std::string& unit,
    const std::string& expr) {
  std::vector<uint64_t> return_values;

  /* parse simple unit */
  {
    auto unit_value = time_windows.find(unit);
    if (unit_value != time_windows.end()) {
      try {
        size_t idx;
        auto interval = std::stof(expr, &idx);
        if (idx < expr.size() - 1) {
          throw std::runtime_error(
              StringUtil::format("can't parse expr $0", expr));
        }

        return_values.emplace_back(interval * unit_value->second);
        return return_values;

      } catch (const std::exception& e) {
        throw std::runtime_error(StringUtil::format("can't parse expr $0", expr));
     }
    }
  }

  /* parse composite unit */
  {
    auto unit_value = units.find(unit);
    if (unit_value == units.end()) {
      throw std::runtime_error(StringUtil::format("can't parse unit $0", unit));
    }

    switch (unit_value->second) {
      case Unit::MINUTE_SECOND: {
        auto values = StringUtil::split(expr, ":");
        if (values.size() == 2 &&
            StringUtil::isNumber(values[0]) &&
            StringUtil::isNumber(values[1])) {

          try {
            return_values.emplace_back(std::stoull(values[0]) * kMicrosPerMinute);
            return_values.emplace_back(std::stoull(values[1]) * kMicrosPerSecond);
            return return_values;

          } catch (const std::exception& e) {
            /* fallthrough */
          }
        }
        throw std::runtime_error(StringUtil::format(
            "expected expr of type minutes:seconds, got: $0",
            expr));
      }

      case Unit::HOUR_SECOND: {
        auto values = StringUtil::split(expr, ":");
        if (values.size() == 3 &&
            StringUtil::isNumber(values[0]) &&
            StringUtil::isNumber(values[1]) &&
            StringUtil::isNumber(values[2])) {

          try {
            return_values.emplace_back(std::stoull(values[0]) * kMicrosPerHour);
            return_values.emplace_back(std::stoull(values[1]) * kMicrosPerMinute);
            return_values.emplace_back(std::stoull(values[2]) * kMicrosPerSecond);
            return return_values;

          } catch (const std::exception& e) {
            /* fallthrough */
          }
        }
        throw std::runtime_error(StringUtil::format(
            "expected expr of type hours:minutes:seconds, got: $0",
            expr));
      }

      case Unit::HOUR_MINUTE: {
        auto values = StringUtil::split(expr, ":");
        if (values.size() == 2 &&
            StringUtil::isNumber(values[0]) &&
            StringUtil::isNumber(values[1])) {

          try {
            return_values.emplace_back(std::stoull(values[0]) * kMicrosPerHour);
            return_values.emplace_back(
                std::stoull(values[1]) * kMicrosPerMinute);
            return return_values;

          } catch (const std::exception& e) {
            /* fallthrough */
          }
        }
        throw std::runtime_error(StringUtil::format(
            "expected expr of type hours:minutes, got: $0",
            expr));
      }

      case Unit::DAY_SECOND: {
        auto day = StringUtil::split(expr, " ");
        if (day.size() == 2 && StringUtil::isNumber(day[0])) {
          auto values = StringUtil::split(day[1], ":");
          if (values.size() == 3 &&
              StringUtil::isNumber(values[0]) &&
              StringUtil::isNumber(values[1]) &&
              StringUtil::isNumber(values[2])) {

            try {
              return_values.emplace_back(std::stoull(day[0]) * kMicrosPerDay);
              return_values.emplace_back(
                  std::stoull(values[0]) * kMicrosPerHour);
              return_values.emplace_back(
                  std::stoull(values[1]) * kMicrosPerMinute);
              return_values.emplace_back(
                  std::stoull(values[2]) * kMicrosPerSecond);
              return return_values;

            } catch (const std::exception& e) {
              /* fallthrough */
            }
          }
        }

        throw std::runtime_error(StringUtil::format(
            "expected expr of type days hours:minutes:seconds, got: $0",
            expr));
      }

      case Unit::DAY_MINUTE: {
        auto day = StringUtil::split(expr, " ");
        if (day.size() == 2 && StringUtil::isNumber(day[0])) {
          auto values = StringUtil::split(day[1], ":");
          if (values.size() == 2 &&
              StringUtil::isNumber(values[0]) &&
              StringUtil::isNumber(values[1])) {

            try {
              return_values.emplace_back(std::stoull(day[0]) * kMicrosPerDay);
              return_values.emplace_back(
                  std::stoull(values[0]) * kMicrosPerHour);
              return_values.emplace_back(
                  std::stoull(values[1]) * kMicrosPerMinute);
              return return_values;

            } catch (const std::exception& e) {
              /* fallthrough */
            }
          }
        }

        throw std::runtime_error(StringUtil::format(
            "expected expr of type days hours:minutes, got: $0",
            expr));
      }
      case Unit::DAY_HOUR: {
        auto values = StringUtil::split(expr, " ");
        if (values.size() == 2 &&
            StringUtil::isNumber(values[0]) &&
            StringUtil::isNumber(values[1])) {

          try {
            return_values.emplace_back(std::stoull(values[0]) * kMicrosPerDay);
            return_values.emplace_back(std::stoull(values[1]) * kMicrosPerHour);
            return return_values;

          } catch (const std::exception& e) {
            /* fallthrough */
          }
        }

        throw std::runtime_error(StringUtil::format(
            "expected expr of type days hours, got: $0",
            expr));
      }

      case Unit::YEAR_MONTH:
        auto values = StringUtil::split(expr, "-");
        if (values.size() == 2 &&
            StringUtil::isNumber(values[0]) &&
            StringUtil::isNumber(values[1])) {

          try {
            return_values.emplace_back(std::stoull(values[0]) * kMicrosPerYear);
            return_values.emplace_back(
              std::stoull(values[1]) * kMicrosPerDay * 30);
            return return_values;

          } catch (const std::exception& e) {
            /* fallthrough */
          }
        }

        throw std::runtime_error(StringUtil::format(
            "expected expr of type years-months, got: $0",
            expr));
    }
  }

  throw std::runtime_error(StringUtil::format("can't parse $0 $1", unit, expr));
}

void date_add_timestamp64_call(sql_txn* ctx, VMStack* stack) {
  auto unit = popString(stack);
  auto expr = popString(stack);
  auto timestamp = popTimestamp64(stack);

  StringUtil::toLower(&unit);

  auto values = parseUnitExpr(unit, expr);
  uint64_t result = 0;
  for (auto v : values) {
    result += v;
  }

  pushTimestamp64(stack, (uint64_t)timestamp + result);
}

const SFunction date_add_timestamp64(
    { SType::TIMESTAMP64, SType::STRING, SType::STRING },
    SType::TIMESTAMP64,
    &date_add_timestamp64_call);


static Option<uint64_t> parseInterval(String time_interval) {
  uint64_t num;
  String unit;

  try {
    size_t sz;
    num = std::stoull(time_interval, &sz);
    unit = time_interval.substr(sz);
    StringUtil::toLower(&unit);

  } catch (std::invalid_argument e) {
    RAISEF(
      kRuntimeError,
      "TIME_AT: invalid argument $0",
      time_interval);
  }

  auto unit_value = time_windows.find(unit);
  if (unit_value != time_windows.end()) {
    return Some(num * unit_value->second);
  } else {
    return None<uint64_t>();
  }
}

//
//void dateSubExpr(sql_txn* ctx, int argc, SValue* argv, SValue* out) {
//  checkArgs("DATE_SUB", argc, 3);
//
//  SValue val = argv[0];
//  auto date = val.getTimestamp();
//  auto unit = argv[2].getString();
//  StringUtil::toLower(&unit);
//
//  if (unit == "second") {
//    if (argv[1].isConvertibleToNumeric()) {
//      *out = SValue(SValue::TimeType(
//          uint64_t(date) - (argv[1].getFloat() * kMicrosPerSecond)));
//      return;
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        argv[1].getString(),
//        argv[2].getString());
//  }
//
//  if (unit == "minute") {
//    if (argv[1].isConvertibleToNumeric()) {
//      *out = SValue(SValue::TimeType(
//          uint64_t(date) - (argv[1].getFloat() * kMicrosPerMinute)));
//      return;
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        argv[1].getString(),
//        argv[2].getString());
//  }
//
//  if (unit == "hour") {
//    if (argv[1].isConvertibleToNumeric()) {
//      *out = SValue(SValue::TimeType(
//          uint64_t(date) - (argv[1].getFloat() * kMicrosPerHour)));
//      return;
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        argv[1].getString(),
//        argv[2].getString());
//  }
//
//  if (unit == "day") {
//    if (argv[1].isConvertibleToNumeric()) {
//      *out = SValue(SValue::TimeType(
//          uint64_t(date) - (argv[1].getFloat() * kMicrosPerDay)));
//      return;
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        argv[1].getString(),
//        argv[2].getString());
//  }
//
//  if (unit == "week") {
//    if (argv[1].isConvertibleToNumeric()) {
//      *out = SValue(SValue::TimeType(
//          uint64_t(date) - (argv[1].getFloat() * kMicrosPerWeek)));
//      return;
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        argv[1].getString(),
//        argv[2].getString());
//  }
//
//  if (unit == "month") {
//    if (argv[1].isConvertibleToNumeric()) {
//      *out = SValue(SValue::TimeType(
//          uint64_t(date) - (argv[1].getFloat() * kMicrosPerDay * 31)));
//      return;
//    }
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        argv[1].getString(),
//        argv[2].getString());
//  }
//
//  if (unit == "year") {
//    if (argv[1].isConvertibleToNumeric()) {
//      *out = SValue(SValue::TimeType(
//          uint64_t(date) - (argv[1].getFloat() * kMicrosPerYear)));
//      return;
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        argv[1].getString(),
//        argv[2].getString());
//  }
//
//  auto expr = argv[1].getString();
//  if (unit == "minute_second") {
//    auto values = StringUtil::split(expr, ":");
//    if (values.size() == 2 &&
//        StringUtil::isNumber(values[0]) &&
//        StringUtil::isNumber(values[1])) {
//
//      try {
//        *out = SValue(SValue::TimeType(
//            uint64_t(date) -
//            (std::stoull(values[0]) * kMicrosPerMinute) +
//            (std::stoull(values[1]) * kMicrosPerSecond)));
//        return;
//      } catch (std::invalid_argument e) {
//        /* fallthrough */
//      }
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        expr,
//        argv[2].getString());
//  }
//
//  if (unit == "hour_second") {
//    auto values = StringUtil::split(expr, ":");
//    if (values.size() == 3 &&
//        StringUtil::isNumber(values[0]) &&
//        StringUtil::isNumber(values[1]) &&
//        StringUtil::isNumber(values[2])) {
//
//      try {
//        *out = SValue(SValue::TimeType(
//            uint64_t(date) -
//            (std::stoull(values[0]) * kMicrosPerHour) +
//            (std::stoull(values[1]) * kMicrosPerMinute) +
//            (std::stoull(values[2]) * kMicrosPerSecond)));
//        return;
//      } catch (std::invalid_argument e) {
//        /* fallthrough */
//      }
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        expr,
//        argv[2].getString());
//  }
//
//  if (unit == "hour_minute") {
//    auto values = StringUtil::split(expr, ":");
//    if (values.size() == 2 &&
//        StringUtil::isNumber(values[0]) &&
//        StringUtil::isNumber(values[1])) {
//
//      try {
//        *out = SValue(SValue::TimeType(
//            uint64_t(date) -
//            (std::stoull(values[0]) * kMicrosPerHour) +
//            (std::stoull(values[1]) * kMicrosPerMinute)));
//        return;
//      } catch (std::invalid_argument e) {
//        /* fallthrough */
//      }
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        expr,
//        argv[2].getString());
//  }
//
//  if (unit == "day_second") {
//    auto values = StringUtil::split(expr, " ");
//    if (values.size() == 2 && StringUtil::isNumber(values[0])) {
//
//      auto time_values = StringUtil::split(values[1], ":");
//      if (time_values.size() == 3 &&
//          StringUtil::isNumber(time_values[0]) &&
//          StringUtil::isNumber(time_values[1]) &&
//          StringUtil::isNumber(time_values[2])) {
//
//        try {
//          *out = SValue(SValue::TimeType(
//              uint64_t(date) -
//              (std::stoull(values[0]) * kMicrosPerDay) +
//              (std::stoull(time_values[0]) * kMicrosPerHour) +
//              (std::stoull(time_values[1]) * kMicrosPerMinute) +
//              (std::stoull(time_values[2]) * kMicrosPerSecond)));
//          return;
//        } catch (std::invalid_argument e) {
//          /* fallthrough */
//        }
//      }
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        expr,
//        argv[2].getString());
//  }
//
//  if (unit == "day_minute") {
//    auto values = StringUtil::split(expr, " ");
//    if (values.size() == 2 && StringUtil::isNumber(values[0])) {
//
//      auto time_values = StringUtil::split(values[1], ":");
//      if (time_values.size() == 2 &&
//          StringUtil::isNumber(time_values[0]) &&
//          StringUtil::isNumber(time_values[1])) {
//
//        try {
//          *out = SValue(SValue::TimeType(
//              uint64_t(date) -
//              (std::stoull(values[0]) * kMicrosPerDay) +
//              (std::stoull(time_values[0]) * kMicrosPerHour) +
//              (std::stoull(time_values[1]) * kMicrosPerMinute)));
//          return;
//        } catch (std::invalid_argument e) {
//          /* fallthrough */
//        }
//      }
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        expr,
//        argv[2].getString());
//  }
//
//  if (unit == "day_hour") {
//    auto values = StringUtil::split(expr, " ");
//    if (values.size() == 2 &&
//        StringUtil::isNumber(values[0]) &&
//        StringUtil::isNumber(values[1])) {
//
//      try {
//        *out = SValue(SValue::TimeType(
//            uint64_t(date) -
//            (std::stoull(values[0]) * kMicrosPerDay) +
//            (std::stoull(values[1]) * kMicrosPerHour)));
//        return;
//      } catch (std::invalid_argument e) {
//        /* fallthrough */
//      }
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        expr,
//        argv[2].getString());
//  }
//
//  if (unit == "year_month") {
//    auto values = StringUtil::split(expr, "-");
//    if (values.size() == 2 &&
//        StringUtil::isNumber(values[0]) &&
//        StringUtil::isNumber(values[1])) {
//
//      try {
//        *out = SValue(SValue::TimeType(
//            uint64_t(date) -
//            (std::stoull(values[0]) * kMicrosPerYear) +
//            (std::stoull(values[1]) * kMicrosPerDay * 31)));
//        return;
//      } catch (std::invalid_argument e) {
//        /* fallthrough */
//      }
//    }
//
//    RAISEF(
//        kRuntimeError,
//        "DATE_SUB: invalid expression $0 for unit $1",
//        expr,
//        argv[2].getString());
//  }
//
//  RAISEF(
//      kRuntimeError,
//      "DATE_SUB: invalid unit $0",
//      argv[2].getString());
//}

void time_at_call(sql_txn* ctx, VMStack* stack) {
  auto time_str = popString(stack);

  StringUtil::toLower(&time_str);
  if (time_str == "now") {
    pushTimestamp64(stack, WallClock::unixMicros());
    return;
  }

  if (StringUtil::beginsWith(time_str, "-")) {
    try {
      auto now = uint64_t(WallClock::now());
      Option<uint64_t> time_val = parseInterval(time_str.substr(1));
      if (!time_val.isEmpty()) {
        pushTimestamp64(stack, now - time_val.get());
        return;
      }
    } catch (...) {
      RAISEF(
        kRuntimeError,
        "TIME_AT: invalid argument $0",
        time_str);
    }
  }

  if (StringUtil::endsWith(time_str, "ago")) {
    try {
      auto now = uint64_t(WallClock::now());
      Option<uint64_t> time_val = parseInterval(
          time_str.substr(0, time_str.length() - 4));
      if (!time_val.isEmpty()) {
        pushTimestamp64(stack, now - time_val.get());
        return;
      }
    } catch (...) {
      RAISEF(
        kRuntimeError,
        "TIME_AT: invalid argument $0",
        time_str);
    }
  }

  auto time_opt = Human::parseTime(time_str);
  if (time_opt.isEmpty()) {
    RAISEF(
       kTypeError,
        "can't convert '$0' to TIMESTAMP",
        time_str);
  }

  pushTimestamp64(stack, time_opt.get().unixMicros());
}

const SFunction time_at(
    { SType::STRING },
    SType::TIMESTAMP64,
    &time_at_call);

}
}
